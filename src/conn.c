/* conn.c — 연결 하나 처리.
 *
 * [L1 단계 임시 구현]
 * L2(요청 파서)와 L3(응답 생성)가 아직 없으므로, 지금은
 *   - 빈 줄("\r\n\r\n")이 올 때까지 받고
 *   - 미리 만들어 둔 고정 응답(200 / 400 / 431)을 보낸다.
 * 빈 줄 찾기는 HTTP 지식이라 L1(tcp.c)이 아니라 여기에 두었고, L2 가 생기면 http_parse() 로 바뀐다.
 */
#define _POSIX_C_SOURCE 200809L

#include "conn.h"
#include "tcp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define REQ_BUF_SIZE 8192    /* 요청 헤더 최대 크기. 넘으면 431 */
#define RECV_TIMEOUT_MS 5000 /* 요청 전체를 받는 데 주는 시간 */
#define SEND_TIMEOUT_MS 5000 /* 응답 전체를 보내는 데 주는 시간 */

#define BODY_200 "Hello from MicroHTTP\n"
static const char RESP_200[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: 21\r\n"
                               "Connection: close\r\n"
                               "\r\n" BODY_200;
_Static_assert(sizeof(BODY_200) - 1 == 21, "Content-Length 를 본문 길이에 맞춰 고칠 것");

static const char RESP_400[] = "HTTP/1.1 400 Bad Request\r\n"
                               "Content-Length: 0\r\nConnection: close\r\n\r\n";
static const char RESP_431[] = "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                               "Content-Length: 0\r\nConnection: close\r\n\r\n";

/* buf[0..len) 에 "\r\n\r\n" 이 있으면 1. from 부터 찾는다 */
static int has_blank_line(const char *buf, size_t from, size_t len)
{
    for (size_t i = from; i + 4 <= len; i++)
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0)
            return 1;
    return 0;
}

void conn_serve(int fd, const char *peer, int quiet)
{
    char buf[REQ_BUF_SIZE];
    size_t len = 0;
    long long deadline = tcp_now_ms() + RECV_TIMEOUT_MS; /* 요청 하나 전체에 마감 하나 */
    const char *resp = NULL;
    size_t resp_len = 0;

    while (resp == NULL) {
        if (len == sizeof buf) {
            resp = RESP_431, resp_len = sizeof RESP_431 - 1;
            break;
        }
        ssize_t n = tcp_recv_some(fd, buf + len, sizeof buf - len, deadline);
        if (n < 0) { /* 마감 초과 또는 연결 오류: 응답하지 않고 끊는다 */
            fprintf(stderr, "[%s] recv: %s\n", peer, strerror(errno));
            close(fd);
            return;
        }
        if (n == 0) { /* 클라이언트가 닫음 */
            if (len == 0) { /* 아무것도 안 보냈으면 그냥 닫는다 */
                close(fd);
                return;
            }
            resp = RESP_400, resp_len = sizeof RESP_400 - 1; /* 요청이 끝나기 전에 닫힘 */
            break;
        }
        /* "\r\n\r\n" 이 이전 조각과 새 조각에 걸칠 수 있으므로 3바이트 앞부터 찾는다 */
        size_t from = len >= 3 ? len - 3 : 0;
        len += (size_t)n;
        if (has_blank_line(buf, from, len))
            resp = RESP_200, resp_len = sizeof RESP_200 - 1;
    }

    if (!quiet || resp != RESP_200) {
        size_t line = 0; /* 요청 첫 줄 길이 (buf 는 NUL 로 끝나지 않으므로 len 안에서만 센다) */
        while (line < len && buf[line] != '\r' && buf[line] != '\n')
            line++;
        printf("[%s] %.*s -> %.3s\n", peer, (int)line, buf, resp + 9); /* 요청 첫 줄 -> 상태 코드 */
        fflush(stdout);
    }
    if (tcp_send_all(fd, resp, resp_len, tcp_now_ms() + SEND_TIMEOUT_MS) < 0) {
        fprintf(stderr, "[%s] send: %s\n", peer, strerror(errno));
        close(fd);
        return;
    }
    tcp_close_graceful(fd);
}
