/* test_http_response.c — http_reason / http_format_date / http_format_head 단위 테스트,
 * 그리고 socketpair 로 http_send_response / http_send_error / tcp_send_all2 송신 테스트. */

#define _POSIX_C_SOURCE 200809L /* socketpair, fork, sigaction 등 POSIX 함수 */

#include "http_response.h" /* 테스트 대상 */
#include "tcp_server.h"    /* tcp_send_all2 */
#include "test.h"          /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <signal.h>        /* signal, SIGPIPE, SIG_IGN */
#include <stdlib.h>        /* malloc, free, _Exit */
#include <string.h>        /* strcmp, strlen, strstr, memcmp */
#include <sys/socket.h>    /* socketpair, AF_UNIX, SOCK_STREAM */
#include <sys/wait.h>      /* waitpid */
#include <unistd.h>        /* fork, read, close */

/* fd 에서 EOF 까지 전부 읽어 malloc 버퍼로 돌려준다. *len 에 길이 (끝에 '\0' 도 붙임) */
static char *read_all(int fd, size_t *len)
{
    size_t cap = 4096, used = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (used + 1 >= cap) {                  /* '\0' 자리까지 포함해 모자라면 두 배로 */
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t n = read(fd, buf + used, cap - 1 - used);
        if (n <= 0)                             /* EOF 또는 오류 */
            break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    *len = used;
    return buf;
}

/* 자식 프로세스에서 send_fn 을 실행하고 소켓을 닫는다 → 부모가 EOF 까지 읽을 수 있음.
 * 반환: 부모가 읽을 fd (자식이 보낸 전체 바이트가 나옴) */
typedef int (*sender_fn)(int fd, void *arg);
static int spawn_sender(sender_fn fn, void *arg, pid_t *pid)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    *pid = fork();
    if (*pid == 0) {                            /* 자식: 보내고 닫고 종료 */
        close(sv[0]);
        int rc = fn(sv[1], arg);
        close(sv[1]);
        _Exit(rc == 0 ? 0 : 1);                 /* _Exit: 부모와 공유한 stdio 버퍼를 다시 비우지 않음 */
    }
    close(sv[1]);                               /* 부모: 읽기 쪽만 유지 */
    return sv[0];
}

static int send_resp_fn(int fd, void *arg) { return http_send_response(fd, (const http_response *)arg, 5000); }
static int send_err_405(int fd, void *arg) { (void)arg; return http_send_error(fd, 405, "Allow: GET, POST\r\n", 5000); }
static int send_err_404(int fd, void *arg) { (void)arg; return http_send_error(fd, 404, NULL, 5000); }

typedef struct { const char *a; size_t alen; const char *b; size_t blen; } two_parts;
static int send2_fn(int fd, void *arg)
{
    const two_parts *p = arg;
    return tcp_send_all2(fd, p->a, p->alen, p->b, p->blen, 5000);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* 끊긴 소켓에 쓸 때 프로세스가 죽지 않고 -1/EPIPE 를 받게 (main.c 와 동일) */

    /* ---- 이유 문구 표 ---- */
    CHECK_STR(http_reason(200), "OK");
    CHECK_STR(http_reason(400), "Bad Request");
    CHECK_STR(http_reason(403), "Forbidden");
    CHECK_STR(http_reason(404), "Not Found");
    CHECK_STR(http_reason(405), "Method Not Allowed");
    CHECK_STR(http_reason(411), "Length Required");
    CHECK_STR(http_reason(413), "Payload Too Large");
    CHECK_STR(http_reason(414), "URI Too Long");
    CHECK_STR(http_reason(417), "Expectation Failed");
    CHECK_STR(http_reason(431), "Request Header Fields Too Large");
    CHECK_STR(http_reason(500), "Internal Server Error");
    CHECK_STR(http_reason(501), "Not Implemented");
    CHECK_STR(http_reason(505), "HTTP Version Not Supported");
    CHECK_STR(http_reason(299), "Unknown");                      /* 표에 없는 코드 */

    /* ---- Date 형식 (RFC 7231 의 예시 시각) ---- */
    char date[64];
    CHECK_INT(http_format_date(date, sizeof(date), 784111777), 29);
    CHECK_STR(date, "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK_INT(http_format_date(date, sizeof(date), 0), 29);
    CHECK_STR(date, "Thu, 01 Jan 1970 00:00:00 GMT");            /* epoch */
    CHECK_INT(http_format_date(date, 29, 0), -1);                /* '\0' 자리가 없음 */
    CHECK_INT(http_format_date(date, 30, 0), 29);                /* 딱 맞음 */

    /* ---- 응답 머리 ---- */
    char out[512];
    http_response r = {.status = 404, .content_type = "text/html", .body_len = 12, .keep_alive = 0};
    int n = http_format_head(out, sizeof(out), &r, 784111777);
    CHECK_STR(out, "HTTP/1.1 404 Not Found\r\n"
                   "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
                   "Server: microhttp\r\n"
                   "Content-Type: text/html\r\n"
                   "Content-Length: 12\r\n"
                   "Connection: close\r\n"
                   "\r\n");
    CHECK_INT(n, (int)strlen(out));                              /* 반환값 = 쓴 길이 */

    r = (http_response){.status = 200, .content_type = "text/plain", .keep_alive = 1};
    n = http_format_head(out, sizeof(out), &r, 0);
    CHECK(n > 0 && strstr(out, "Connection: keep-alive\r\n") != NULL);
    CHECK(strstr(out, "Content-Length: 0\r\n") != NULL);         /* 본문 없어도 Content-Length: 0 */

    /* 추가 헤더는 Connection 뒤, 빈 줄 앞에 그대로 들어감 */
    r = (http_response){.status = 405, .content_type = "text/plain",
                        .extra_headers = "Allow: GET, POST\r\nCache-Control: no-store\r\n"};
    n = http_format_head(out, sizeof(out), &r, 0);
    CHECK(n > 0 && strstr(out, "Connection: close\r\nAllow: GET, POST\r\nCache-Control: no-store\r\n\r\n") != NULL);
    CHECK(strcmp(out + n - 4, "\r\n\r\n") == 0);                 /* 머리는 항상 빈 줄로 끝남 */

    /* 큰 Content-Length (size_t 전체 범위) */
    r = (http_response){.status = 200, .content_type = "text/plain", .body_len = (size_t)5000000000ULL};
    n = http_format_head(out, sizeof(out), &r, 0);
    CHECK(n > 0 && strstr(out, "Content-Length: 5000000000\r\n") != NULL);

    /* 잘못된 입력 → -1 (잘못된 응답을 보내지 않게) */
    r = (http_response){.status = 200, .content_type = "text/plain"};
    CHECK_INT(http_format_head(out, sizeof(out), NULL, 0), -1);  /* res 없음 */
    r.content_type = NULL;
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);    /* Content-Type 없음 */
    r = (http_response){.status = 99, .content_type = "text/plain"};
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);    /* 상태 코드 범위 밖 */
    r.status = 600;
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);
    r = (http_response){.status = 200, .content_type = "text/plain\r\nSet-Cookie: x=1"};
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);    /* 헤더 주입 시도 */
    r = (http_response){.status = 200, .content_type = "text/plain\nX: y"};
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);    /* LF 만으로도 거부 */
    r = (http_response){.status = 200, .content_type = "text/plain", .extra_headers = "Allow: GET"};
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);    /* CRLF 로 안 끝나는 추가 헤더 */
    r.extra_headers = "\n";
    CHECK_INT(http_format_head(out, sizeof(out), &r, 0), -1);    /* 너무 짧은 추가 헤더 */

    /* 버퍼 부족 → -1 */
    r = (http_response){.status = 200, .content_type = "text/plain"};
    char tiny[20];
    CHECK_INT(http_format_head(tiny, sizeof(tiny), &r, 0), -1);
    n = http_format_head(out, sizeof(out), &r, 0);               /* 정확한 길이 계산 */
    CHECK_INT(http_format_head(out, (size_t)n, &r, 0), -1);      /* '\0' 자리가 없음 */
    CHECK_INT(http_format_head(out, (size_t)n + 1, &r, 0), n);   /* 딱 맞음 */

    /* ---- 송신: 작은 응답 (머리 + 본문이 한 번에) ---- */
    {
        pid_t pid;
        http_response res = {.status = 200, .content_type = "text/plain", .body = "hello", .body_len = 5, .keep_alive = 1};
        int fd = spawn_sender(send_resp_fn, &res, &pid);
        size_t len;
        char *got = read_all(fd, &len);
        int st;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);            /* 송신 성공 */
        CHECK(strncmp(got, "HTTP/1.1 200 OK\r\nDate: ", 23) == 0);
        CHECK(strstr(got, "Server: microhttp\r\n") != NULL);
        CHECK(strstr(got, "Content-Length: 5\r\n") != NULL);
        CHECK(strstr(got, "Connection: keep-alive\r\n") != NULL);
        CHECK(len > 5 && memcmp(got + len - 9, "\r\n\r\nhello", 9) == 0); /* 빈 줄 바로 뒤에 본문 */
        free(got);
        close(fd);
    }

    /* ---- 송신: 본문 없는 응답 ---- */
    {
        pid_t pid;
        http_response res = {.status = 200, .content_type = "text/plain"};
        int fd = spawn_sender(send_resp_fn, &res, &pid);
        size_t len;
        char *got = read_all(fd, &len);
        waitpid(pid, NULL, 0);
        CHECK(len > 4 && strcmp(got + len - 4, "\r\n\r\n") == 0); /* 머리만 오고 끝 */
        free(got);
        close(fd);
    }

    /* ---- 송신: 소켓 버퍼보다 큰 본문 (부분 전송 처리) ---- */
    {
        pid_t pid;
        size_t blen = 4 * 1024 * 1024;                           /* 4MB: AF_UNIX 소켓 버퍼(수백 KB)보다 훨씬 큼 */
        char *body = malloc(blen);
        for (size_t i = 0; i < blen; i++)
            body[i] = (char)('a' + i % 26);                      /* 위치를 알 수 있는 패턴 → 순서·누락 검증 */
        http_response res = {.status = 200, .content_type = "application/octet-stream", .body = body, .body_len = blen};
        int fd = spawn_sender(send_resp_fn, &res, &pid);
        size_t len;
        char *got = read_all(fd, &len);
        int st;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        const char *sep = strstr(got, "\r\n\r\n");
        CHECK(sep != NULL);
        if (sep) {
            CHECK_INT(len - (size_t)(sep + 4 - got), blen);      /* 본문이 한 바이트도 안 빠지고 안 늘어남 */
            CHECK(memcmp(sep + 4, body, blen) == 0);             /* 내용·순서 동일 */
            CHECK(strstr(got, "Content-Length: 4194304\r\n") != NULL);
        }
        free(got);
        free(body);
        close(fd);
    }

    /* ---- 송신: 오류 응답 ---- */
    {
        pid_t pid;
        int fd = spawn_sender(send_err_404, NULL, &pid);
        size_t len;
        char *got = read_all(fd, &len);
        waitpid(pid, NULL, 0);
        CHECK(strncmp(got, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
        CHECK(strstr(got, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
        CHECK(strstr(got, "Content-Length: 14\r\n") != NULL);    /* "404 Not Found\n" = 14 바이트 */
        CHECK(strstr(got, "Connection: close\r\n") != NULL);     /* 오류 뒤에는 항상 close */
        CHECK(len > 14 && strcmp(got + len - 14, "404 Not Found\n") == 0);
        free(got);
        close(fd);

        fd = spawn_sender(send_err_405, NULL, &pid);             /* 추가 헤더가 있는 오류 */
        got = read_all(fd, &len);
        waitpid(pid, NULL, 0);
        CHECK(strncmp(got, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
        CHECK(strstr(got, "Allow: GET, POST\r\n") != NULL);
        free(got);
        close(fd);
    }

    /* ---- 송신: 상대가 이미 끊음 → -1 (SIGPIPE 는 무시됨) ---- */
    {
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        close(sv[0]);                                            /* 읽는 쪽을 닫아 둠 */
        CHECK_INT(http_send_error(sv[1], 500, NULL, 1000), -1);
        close(sv[1]);
    }

    /* ---- 송신: 잘못된 응답 → -1 (아무것도 보내지 않음) ---- */
    {
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        http_response bad = {.status = 42, .content_type = "text/plain"};
        CHECK_INT(http_send_response(sv[1], &bad, 1000), -1);
        close(sv[1]);
        char c;
        CHECK_INT(read(sv[0], &c, 1), 0);                        /* 보낸 바이트 없이 EOF */
        close(sv[0]);
    }

    /* ---- tcp_send_all2: 두 덩어리 이어 보내기, 빈 덩어리 ---- */
    {
        pid_t pid;
        two_parts p = {"HEAD|", 5, "body", 4};
        int fd = spawn_sender(send2_fn, &p, &pid);
        size_t len;
        char *got = read_all(fd, &len);
        waitpid(pid, NULL, 0);
        CHECK_STR(got, "HEAD|body");
        free(got);
        close(fd);

        two_parts only_a = {"only-a", 6, NULL, 0};               /* b 가 비어 있음 */
        fd = spawn_sender(send2_fn, &only_a, &pid);
        got = read_all(fd, &len);
        waitpid(pid, NULL, 0);
        CHECK_STR(got, "only-a");
        free(got);
        close(fd);

        two_parts only_b = {NULL, 0, "only-b", 6};               /* a 가 비어 있음 */
        fd = spawn_sender(send2_fn, &only_b, &pid);
        got = read_all(fd, &len);
        waitpid(pid, NULL, 0);
        CHECK_STR(got, "only-b");
        free(got);
        close(fd);
    }

    /* ---- tcp_send_all2: 읽는 쪽이 안 읽으면 마감 시간에 맞춰 실패 ---- */
    {
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        size_t big = 8 * 1024 * 1024;                            /* 소켓 버퍼를 넘겨서 반드시 막힘 */
        char *buf = calloc(big, 1);
        CHECK_INT(tcp_send_all2(sv[1], "x", 1, buf, big, 200), -1); /* 200ms 안에 못 보냄 */
        free(buf);
        close(sv[0]);
        close(sv[1]);
    }

    return TEST_REPORT();
}
