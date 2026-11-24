/* conn.c — 연결 하나의 전체 수명: 받기(L1) → 파싱(L2) → 라우팅(L5) → 응답(L3) → 반복 → 닫기.
 *
 * 레이어 구조에서의 위치
 *   L6 스레드 풀의 워커가 conn_serve 를 부른다. 이 파일이 L1~L5 를 이어 붙이는 접착층이다.
 *
 * 구성
 *   - conn: 연결 상태 (fd, peer, 설정, 수신 버퍼와 그 안의 바이트 수)
 *   - receive_request: 요청 하나가 완성될 때까지 tcp_recv_some + http_parse 반복
 *   - serve_one: 요청 하나 받기 → 라우팅 → 로그 → 응답 보내기 → 버퍼 정리
 *   - conn_serve: keep-alive 루프와 연결 닫기
 *
 * keep-alive: 응답 후 연결을 닫지 않고 다음 요청을 기다린다.
 *   - 한 번의 recv 에 다음 요청이 섞여 들어올 수 있다(파이프라이닝).
 *     파서가 알려 주는 consumed 뒤의 바이트를 버퍼 앞으로 당겨 다음 요청으로 쓴다.
 *   - 다음 요청이 KEEPALIVE_IDLE_MS 안에 시작하지 않으면 닫는다 (워커를 오래 붙잡지 않게).
 *   - 파싱 오류, Connection: close, HTTP/1.0(keep-alive 요청 없음)이면 응답 후 닫는다.
 */
#define _POSIX_C_SOURCE 200809L

#include "conn.h"
#include "http_request.h"
#include "http_response.h"
#include "router.h"
#include "tcp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_BODY (1024 * 1024)                           /* POST 본문 최대 1MB, 넘으면 413 */
#define CONN_BUF_SIZE (HTTP_MAX_HEADER_BYTES + MAX_BODY) /* 헤더 + 본문이 다 들어가는 크기 */
#define REQUEST_TIMEOUT_MS 5000                          /* 첫 요청 하나를 다 받는 데 주는 시간 */
#define KEEPALIVE_IDLE_MS 5000                           /* 다음 요청을 기다리는 시간 */
#define SEND_TIMEOUT_MS 5000                             /* 응답 하나를 다 보내는 데 주는 시간 */
#define MAX_REQUESTS_PER_CONN 1000                       /* 한 연결이 무한히 붙잡지 않도록 */

typedef struct {
    int fd;
    const char *peer;
    const server_config *cfg;
    int quiet;
    char *buf;
    size_t len; /* buf 에 들어 있는 바이트 (앞쪽은 아직 처리하지 않은 요청) */
} conn;

typedef enum { RECV_OK, RECV_CLOSED, RECV_FAILED } recv_status;

/* 요청 하나가 완성되거나 오류가 날 때까지 받는다 */
static recv_status receive_request(conn *c, http_request *req, http_parse_result *result, size_t *used,
                                   long long deadline)
{
    for (;;) {
        *result = http_parse(c->buf, c->len, MAX_BODY, req, used);
        if (*result != HTTP_PARSE_INCOMPLETE)
            return RECV_OK;
        if (c->len == CONN_BUF_SIZE) { /* 파서의 크기 제한이 먼저 걸리므로 오지 않아야 하는 곳 */
            *result = HTTP_PARSE_BAD_REQUEST;
            return RECV_OK;
        }
        ssize_t n = tcp_recv_some(c->fd, c->buf + c->len, CONN_BUF_SIZE - c->len, deadline);
        if (n < 0) {
            /* 요청 사이의 대기 시간 초과는 정상적인 keep-alive 종료라 로그를 남기지 않는다 */
            if (c->len > 0 || errno != ETIMEDOUT)
                fprintf(stderr, "[%s] recv: %s (%zu bytes so far)\n", c->peer, strerror(errno), c->len);
            return RECV_FAILED;
        }
        if (n == 0) {
            if (c->len == 0)
                return RECV_CLOSED; /* 요청 사이에 클라이언트가 닫음: 정상 */
            *result = HTTP_PARSE_BAD_REQUEST; /* 요청 도중에 닫힘 */
            return RECV_OK;
        }
        c->len += (size_t)n;
    }
}

/* 요청 하나 처리. 반환: 1 연결 유지, 0 닫기 */
static int serve_one(conn *c, long long deadline, int last, int *responded)
{
    http_request *req = malloc(sizeof *req); /* path_buf 때문에 8KB 가 넘어 스택 대신 힙에 */
    if (!req)
        return 0;
    http_parse_result r;
    size_t used = 0;
    recv_status rs = receive_request(c, req, &r, &used, deadline);
    if (rs != RECV_OK) {
        free(req);
        return 0;
    }

    http_response res;
    http_response_init(&res, 200);
    if (r == HTTP_PARSE_OK) {
        req->remote_addr = c->peer;
        router_dispatch(c->cfg, req, &res); /* L5: CGI 또는 L4 정적 파일 */
        res.keep_alive = res.keep_alive && req->keep_alive && !last;
        if (!c->quiet)
            printf("[%s] %s %s HTTP/1.%d -> %d\n", c->peer, req->method_str, req->path, req->version_minor,
                   res.status);
    } else {
        http_response_error(&res, http_parse_status(r));
        res.keep_alive = 0; /* 어디까지가 요청이었는지 모르므로 더 읽지 않는다 */
        printf("[%s] bad request -> %d\n", c->peer, res.status);
    }
    fflush(stdout);

    int head_only = r == HTTP_PARSE_OK && req->method == HTTP_HEAD;
    int keep = res.keep_alive;
    int sent = http_response_send(c->fd, &res, head_only, tcp_now_ms() + SEND_TIMEOUT_MS);
    http_response_free(&res);
    free(req);
    if (sent < 0) {
        fprintf(stderr, "[%s] send: %s\n", c->peer, strerror(errno));
        return 0;
    }
    *responded = 1;

    /* 처리한 요청을 버퍼에서 빼고, 남은 바이트(다음 요청의 시작)를 앞으로 당긴다 */
    memmove(c->buf, c->buf + used, c->len - used);
    c->len -= used;
    return keep;
}

/* 연결 하나를 끝까지 처리한다: 버퍼를 만들고 serve_one 을 keep-alive 가 끝날 때까지(최대 1000번) 반복,
 * 마감 시각은 첫 요청은 REQUEST_TIMEOUT_MS, 그 뒤로는 KEEPALIVE_IDLE_MS. 응답을 보냈으면 우아하게 닫는다 */
void conn_serve(int fd, const char *peer, const server_config *cfg, int quiet)
{
    conn c = {.fd = fd, .peer = peer, .cfg = cfg, .quiet = quiet, .buf = malloc(CONN_BUF_SIZE), .len = 0};
    if (!c.buf) {
        fprintf(stderr, "[%s] out of memory\n", peer);
        close(fd);
        return;
    }
    int responded = 0;
    long long deadline = tcp_now_ms() + REQUEST_TIMEOUT_MS;
    for (int i = 0; i < MAX_REQUESTS_PER_CONN; i++) {
        if (!serve_one(&c, deadline, i == MAX_REQUESTS_PER_CONN - 1, &responded))
            break;
        deadline = tcp_now_ms() + KEEPALIVE_IDLE_MS;
    }
    if (responded)
        tcp_close_graceful(fd); /* 응답을 보낸 연결은 RST 없이 닫는다 */
    else
        close(fd);
    free(c.buf);
}
