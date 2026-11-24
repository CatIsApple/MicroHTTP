/* conn.c — 연결 하나 처리: 받기(L1) → 파싱(L2) → 응답 → 닫기.
 *
 * 받기 루프는 "받은 바이트를 버퍼 뒤에 붙이고 http_parse() 에 물어보기" 의 반복이다.
 * 요청이 어디서 끝나는지(빈 줄, Content-Length 만큼의 본문)는 파서가 판단한다.
 *
 * [임시] L3(응답 생성)가 아직 없어서 응답은 미리 만든 고정 문자열이다.
 *        연결은 요청 하나를 처리하고 닫는다(keep-alive 는 나중 단계).
 */
#define _POSIX_C_SOURCE 200809L

#include "conn.h"
#include "http_request.h"
#include "tcp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_BODY (1024 * 1024)                        /* POST 본문 최대 1MB, 넘으면 413 */
#define CONN_BUF_SIZE (HTTP_MAX_HEADER_BYTES + MAX_BODY) /* 헤더 + 본문이 다 들어가는 크기 */
#define RECV_TIMEOUT_MS 5000                          /* 요청 전체를 받는 데 주는 시간 */
#define SEND_TIMEOUT_MS 5000                          /* 응답 전체를 보내는 데 주는 시간 */

#define BODY_200 "Hello from MicroHTTP\n"
#define HEAD_200                                  \
    "HTTP/1.1 200 OK\r\n"                         \
    "Content-Type: text/plain; charset=utf-8\r\n" \
    "Content-Length: 21\r\n"                      \
    "Connection: close\r\n"                       \
    "\r\n"
static const char RESP_200[] = HEAD_200 BODY_200;
_Static_assert(sizeof(BODY_200) - 1 == 21, "Content-Length 를 본문 길이에 맞춰 고칠 것");

#define EMPTY_RESP(status) "HTTP/1.1 " status "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

static const char *error_response(int status)
{
    switch (status) {
    case 413: return EMPTY_RESP("413 Content Too Large");
    case 431: return EMPTY_RESP("431 Request Header Fields Too Large");
    case 501: return EMPTY_RESP("501 Not Implemented");
    case 505: return EMPTY_RESP("505 HTTP Version Not Supported");
    default:  return EMPTY_RESP("400 Bad Request");
    }
}

/* 요청 하나를 받아 파싱한다.
 * 반환: 1 응답할 것이 있음(*result 채움), 0 응답 없이 닫을 것(타임아웃, 오류, 빈 연결) */
static int receive_request(int fd, const char *peer, char *buf, http_request *req, http_parse_result *result)
{
    size_t len = 0, used;
    long long deadline = tcp_now_ms() + RECV_TIMEOUT_MS; /* 요청 하나 전체에 마감 하나 */

    for (;;) {
        *result = http_parse(buf, len, MAX_BODY, req, &used);
        if (*result != HTTP_PARSE_INCOMPLETE)
            return 1;
        if (len == CONN_BUF_SIZE) { /* 파서의 크기 제한이 먼저 걸리므로 오지 않아야 하는 곳 */
            *result = HTTP_PARSE_BAD_REQUEST;
            return 1;
        }
        ssize_t n = tcp_recv_some(fd, buf + len, CONN_BUF_SIZE - len, deadline);
        if (n < 0) {
            fprintf(stderr, "[%s] recv: %s (%zu bytes so far)\n", peer, strerror(errno), len);
            return 0;
        }
        if (n == 0) {         /* 클라이언트가 닫음 */
            if (len == 0)
                return 0;     /* 아무것도 안 보냈음 */
            *result = HTTP_PARSE_BAD_REQUEST; /* 요청이 끝나기 전에 닫힘 */
            return 1;
        }
        len += (size_t)n;
    }
}

void conn_serve(int fd, const char *peer, int quiet)
{
    /* 1MB 버퍼는 스택 대신 힙에 (나중에 스레드마다 스택을 작게 잡아도 안전하도록) */
    char *buf = malloc(CONN_BUF_SIZE);
    http_request *req = malloc(sizeof *req);
    if (!buf || !req) {
        fprintf(stderr, "[%s] out of memory\n", peer);
        free(buf);
        free(req);
        close(fd);
        return;
    }

    http_parse_result r;
    if (!receive_request(fd, peer, buf, req, &r)) {
        close(fd);
        goto out;
    }

    const char *resp;
    size_t resp_len;
    if (r == HTTP_PARSE_OK) {
        resp = RESP_200;
        /* HEAD 는 헤더만 보낸다 */
        resp_len = req->method == HTTP_HEAD ? sizeof(HEAD_200) - 1 : sizeof(RESP_200) - 1;
        if (!quiet) {
            printf("[%s] %s %s HTTP/1.%d host=%s body=%zu -> 200\n", peer, req->method_str, req->path,
                   req->version_minor, req->host ? req->host : "-", req->body_len);
            fflush(stdout);
        }
    } else {
        int status = http_parse_status(r);
        resp = error_response(status);
        resp_len = strlen(resp);
        printf("[%s] bad request -> %d\n", peer, status);
        fflush(stdout);
    }

    if (tcp_send_all(fd, resp, resp_len, tcp_now_ms() + SEND_TIMEOUT_MS) < 0) {
        fprintf(stderr, "[%s] send: %s\n", peer, strerror(errno));
        close(fd);
    } else {
        tcp_close_graceful(fd);
    }
out:
    free(buf);
    free(req);
}
