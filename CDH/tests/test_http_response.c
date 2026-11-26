/* test_http_response.c — http_reason / http_format_date / http_format_head 단위 테스트,
 * 그리고 socketpair 로 http_send_response / http_send_error / tcp_sendv_all / tcp_sendfile_all 송신 테스트. */

#define _POSIX_C_SOURCE 200809L /* socketpair, fork, mkstemp, pwrite 등 POSIX 함수 */

#include "http_response.h" /* 테스트 대상 */
#include "tcp_server.h"    /* tcp_sendv_all, tcp_sendfile_all */
#include "test.h"          /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <arpa/inet.h>     /* htonl */
#include <errno.h>         /* errno, ETIMEDOUT */
#include <netinet/in.h>    /* struct sockaddr_in, INADDR_LOOPBACK */
#include <signal.h>        /* signal, kill, SIGPIPE, SIGKILL, SIG_IGN */
#include <stdio.h>         /* fprintf */
#include <stdlib.h>        /* malloc, free, _Exit, mkstemp */
#include <string.h>        /* strcmp, strlen, strstr, memcmp, memset */
#include <sys/socket.h>    /* socketpair, socket, connect, accept, AF_UNIX, SOCK_STREAM */
#include <sys/wait.h>      /* waitpid */
#include <time.h>          /* clock_gettime, nanosleep */
#include <unistd.h>        /* fork, read, write, close, unlink */

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

/* 자식 프로세스에서 fn 을 실행하고 소켓을 닫는다 → 부모가 EOF 까지 읽을 수 있음.
 * 반환: 부모가 읽을 fd. 자식 종료 코드 = fn 이 0 이면 0, 아니면 1 */
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

/* 보내기 → 전부 받기 → 자식 종료 코드까지 한 번에. *ok 에 송신 성공 여부 */
static char *run_sender(sender_fn fn, void *arg, size_t *len, int *ok)
{
    pid_t pid;
    int fd = spawn_sender(fn, arg, &pid);
    char *got = read_all(fd, len);
    close(fd);
    int st = 0;
    waitpid(pid, &st, 0);
    *ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    return got;
}

static int send_resp_fn(int fd, void *arg) { return http_send_response(fd, (const http_response *)arg, 5000); }
static int send_err_405(int fd, void *arg) { (void)arg; return http_send_error(fd, 405, "Allow: GET, POST\r\n", 5000); }
static int send_err_404(int fd, void *arg) { (void)arg; return http_send_error(fd, 404, NULL, 5000); }

typedef struct { struct iovec iov[3]; int cnt; } parts;
static int sendv_fn(int fd, void *arg)
{
    parts *p = arg;
    return tcp_sendv_all(fd, p->iov, p->cnt, 5000);
}

typedef struct { const char *head; int file_fd; off_t off; size_t len; } file_part;
static int sendfile_fn(int fd, void *arg)
{
    file_part *p = arg;
    return tcp_sendfile_all(fd, p->head, p->head ? strlen(p->head) : 0, p->file_fd, p->off, p->len, 5000);
}

/* 머리를 기본 버퍼에 만들어 보고 결과를 돌려주는 도우미 */
static char g_out[HTTP_HEAD_MAX];
static int fmt(const http_response *r) { return http_format_head(g_out, sizeof(g_out), r, 784111777); }

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* 끊긴 소켓에 쓸 때 프로세스가 죽지 않고 -1/EPIPE 를 받게 (main.c 와 동일) */

    /* ---- 이유 문구 표 ---- */
    CHECK_STR(http_reason(200), "OK");
    CHECK_STR(http_reason(204), "No Content");
    CHECK_STR(http_reason(301), "Moved Permanently");
    CHECK_STR(http_reason(304), "Not Modified");
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

    /* ---- 본문 가능 여부 ---- */
    CHECK_INT(http_status_has_body(200), 1);
    CHECK_INT(http_status_has_body(404), 1);
    CHECK_INT(http_status_has_body(204), 0);
    CHECK_INT(http_status_has_body(304), 0);

    /* ---- Date 형식 (RFC 7231 의 예시 시각) ---- */
    char date[64];
    CHECK_INT(http_format_date(date, sizeof(date), 784111777), 29);
    CHECK_STR(date, "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK_INT(http_format_date(date, sizeof(date), 0), 29);
    CHECK_STR(date, "Thu, 01 Jan 1970 00:00:00 GMT");            /* epoch */
    CHECK_INT(http_format_date(date, 29, 0), -1);                /* '\0' 자리가 없음 */
    CHECK_INT(http_format_date(date, 30, 0), 29);                /* 딱 맞음 */

    /* ---- 응답 머리: 기본 ---- */
    http_response r = {.status = 404, .content_type = "text/html", .body_len = 12};
    int n = fmt(&r);
    CHECK_STR(g_out, "HTTP/1.1 404 Not Found\r\n"
                     "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
                     "Server: microhttp\r\n"
                     "Content-Type: text/html\r\n"
                     "Content-Length: 12\r\n"
                     "Connection: close\r\n"
                     "\r\n");
    CHECK_INT(n, (int)strlen(g_out));                            /* 반환값 = 쓴 길이 */

    r = (http_response){.status = 200, .content_type = "text/plain", .keep_alive = 1};
    CHECK(fmt(&r) > 0 && strstr(g_out, "Connection: keep-alive\r\n") != NULL);
    CHECK(strstr(g_out, "Content-Length: 0\r\n") != NULL);       /* 본문 없어도 Content-Length: 0 */

    /* Content-Type 은 선택: NULL 이면 헤더 생략 */
    r = (http_response){.status = 200};
    CHECK(fmt(&r) > 0 && strstr(g_out, "Content-Type") == NULL);

    /* ---- 204 / 304: Content-Length 없음 (본문 길이를 줘도) ---- */
    r = (http_response){.status = 204, .body_len = 10};
    CHECK(fmt(&r) > 0 && strstr(g_out, "Content-Length") == NULL);
    CHECK(strncmp(g_out, "HTTP/1.1 204 No Content\r\n", 25) == 0);
    r = (http_response){.status = 304, .content_type = "text/html", .body_len = 10};
    CHECK(fmt(&r) > 0 && strstr(g_out, "Content-Length") == NULL);

    /* ---- 추가 헤더: Connection 뒤, 빈 줄 앞에 그대로 ---- */
    r = (http_response){.status = 405, .content_type = "text/plain",
                        .extra_headers = "Allow: GET, POST\r\nCache-Control: no-store\r\n"};
    n = fmt(&r);
    CHECK(n > 0 && strstr(g_out, "Connection: close\r\nAllow: GET, POST\r\nCache-Control: no-store\r\n\r\n") != NULL);
    CHECK(strcmp(g_out + n - 4, "\r\n\r\n") == 0);               /* 머리는 항상 빈 줄로 끝남 */
    r.extra_headers = "X-Name: \xea\xb0\x80\tok\r\n";              /* 값의 HTAB, UTF-8(obs-text) 허용 */
    CHECK(fmt(&r) > 0);
    r.extra_headers = "";                                         /* 빈 문자열 = 추가 헤더 없음 */
    CHECK(fmt(&r) > 0);

    /* 큰 Content-Length (size_t 전체 범위) */
    r = (http_response){.status = 200, .content_type = "text/plain", .body_len = (size_t)5000000000ULL};
    CHECK(fmt(&r) > 0 && strstr(g_out, "Content-Length: 5000000000\r\n") != NULL);

    /* URI 상한(2048) 짜리 Location 도 머리 버퍼에 들어감 */
    {
        static char loc[2200];
        memcpy(loc, "Location: /", 11);
        memset(loc + 11, 'a', 2048);
        memcpy(loc + 11 + 2048, "/\r\n", 4);                     /* '\0' 포함 */
        r = (http_response){.status = 301, .content_type = "text/plain", .extra_headers = loc};
        n = fmt(&r);
        CHECK(n > 2048 && strstr(g_out, "/\r\n\r\n") != NULL);
    }

    /* ---- 잘못된 입력 → -1 (잘못된 응답을 보내지 않게) ---- */
    CHECK_INT(http_format_head(g_out, sizeof(g_out), NULL, 0), -1); /* res 없음 */
    r = (http_response){.status = 99};
    CHECK_INT(fmt(&r), -1);                                      /* 상태 코드 범위 밖 */
    r.status = 600;
    CHECK_INT(fmt(&r), -1);
    r.status = 100;
    CHECK_INT(fmt(&r), -1);                                      /* 1xx 는 이 API 로 안 보냄 */
    r.status = 199;
    CHECK_INT(fmt(&r), -1);

    /* Content-Type 헤더 주입 */
    r = (http_response){.status = 200, .content_type = "text/plain\r\nSet-Cookie: x=1"};
    CHECK_INT(fmt(&r), -1);
    r.content_type = "text/plain\nX: y";                         /* LF 만으로도 거부 */
    CHECK_INT(fmt(&r), -1);
    r.content_type = "text/\x01plain";                           /* 그 밖의 제어 문자 */
    CHECK_INT(fmt(&r), -1);

    /* 추가 헤더 형식 오류 / 응답 분할 시도 */
    static const char *const BAD_EXTRA[] = {
        "Allow: GET",                                            /* CRLF 로 안 끝남 */
        "\n",                                                    /* 단독 LF */
        "\r\n",                                                  /* 빈 줄 → 헤더가 여기서 끝나 버림 */
        "A: b\r\n\r\nHTTP/1.1 200 OK\r\n",                       /* 빈 줄 + 가짜 두 번째 응답 */
        "A: b\nC: d\r\n",                                        /* 값 안의 단독 LF */
        "A: b\rC: d\r\n",                                        /* 값 안의 단독 CR */
        ": v\r\n",                                               /* 이름 없음 */
        "A b: v\r\n",                                            /* 이름에 공백 */
        "NoColon\r\n",                                           /* ':' 없음 */
        "A: b\x01\r\n",                                          /* 값에 제어 문자 */
        "A: b\x7f\r\n",                                          /* 값에 DEL */
        "A: ok\r\nB: no-crlf",                                   /* 둘째 줄이 CRLF 로 안 끝남 */
    };
    for (size_t i = 0; i < sizeof(BAD_EXTRA) / sizeof(BAD_EXTRA[0]); i++) {
        r = (http_response){.status = 200, .content_type = "text/plain", .extra_headers = BAD_EXTRA[i]};
        CHECK_INT(fmt(&r), -1);
    }

    /* 버퍼 부족 → -1 */
    r = (http_response){.status = 200, .content_type = "text/plain"};
    char tiny[20];
    CHECK_INT(http_format_head(tiny, sizeof(tiny), &r, 0), -1);
    n = http_format_head(g_out, sizeof(g_out), &r, 0);           /* 정확한 길이 계산 */
    CHECK_INT(http_format_head(g_out, (size_t)n, &r, 0), -1);    /* '\0' 자리가 없음 */
    CHECK_INT(http_format_head(g_out, (size_t)n + 1, &r, 0), n); /* 딱 맞음 */
    CHECK_INT(http_format_head(g_out, 0, &r, 0), -1);            /* 크기 0 */

    size_t len;
    int ok;
    char *got;

    /* ---- 송신: 작은 응답 (머리 + 본문이 한 번에) ---- */
    {
        http_response res = {.status = 200, .content_type = "text/plain", .body = "hello", .body_len = 5, .keep_alive = 1};
        got = run_sender(send_resp_fn, &res, &len, &ok);
        CHECK(ok);
        CHECK(strncmp(got, "HTTP/1.1 200 OK\r\nDate: ", 23) == 0);
        CHECK(strstr(got, "Server: microhttp\r\n") != NULL);
        CHECK(strstr(got, "Content-Length: 5\r\n") != NULL);
        CHECK(strstr(got, "Connection: keep-alive\r\n") != NULL);
        CHECK(len > 9 && memcmp(got + len - 9, "\r\n\r\nhello", 9) == 0); /* 빈 줄 바로 뒤에 본문 */
        free(got);
    }

    /* ---- 송신: 본문 없는 응답 ---- */
    {
        http_response res = {.status = 200, .content_type = "text/plain"};
        got = run_sender(send_resp_fn, &res, &len, &ok);
        CHECK(ok && len > 4 && strcmp(got + len - 4, "\r\n\r\n") == 0); /* 머리만 오고 끝 */
        free(got);
    }

    /* ---- 송신: 204 는 본문을 줘도 안 보냄 ---- */
    {
        http_response res = {.status = 204, .body = "SHOULD-NOT-SEND", .body_len = 15};
        got = run_sender(send_resp_fn, &res, &len, &ok);
        CHECK(ok && strstr(got, "SHOULD-NOT-SEND") == NULL);
        CHECK(len > 4 && strcmp(got + len - 4, "\r\n\r\n") == 0);
        free(got);
    }

    /* ---- 송신: 소켓 버퍼보다 큰 메모리 본문 (부분 전송 처리) ---- */
    size_t blen = 4 * 1024 * 1024;                               /* 4MB: AF_UNIX 소켓 버퍼(수백 KB)보다 훨씬 큼 */
    char *big = malloc(blen);
    for (size_t i = 0; i < blen; i++)
        big[i] = (char)('a' + i % 26);                           /* 위치를 알 수 있는 패턴 → 순서·누락 검증 */
    {
        http_response res = {.status = 200, .content_type = "application/octet-stream", .body = big, .body_len = blen};
        got = run_sender(send_resp_fn, &res, &len, &ok);
        CHECK(ok);
        const char *sep = strstr(got, "\r\n\r\n");
        CHECK(sep != NULL);
        if (sep) {
            CHECK_INT(len - (size_t)(sep + 4 - got), blen);      /* 본문이 한 바이트도 안 빠지고 안 늘어남 */
            CHECK(memcmp(sep + 4, big, blen) == 0);              /* 내용·순서 동일 */
            CHECK(strstr(got, "Content-Length: 4194304\r\n") != NULL);
        }
        free(got);
    }

    /* ---- 파일 본문: 임시 파일에 패턴을 써 두고 sendfile 경로 검증 ---- */
    char path[] = "/tmp/microhttp_test_XXXXXX";
    int file_fd = mkstemp(path);                                 /* 임시 파일 생성 (읽기/쓰기로 열림) */
    CHECK(file_fd >= 0);
    if (file_fd >= 0) {
        unlink(path);                                            /* 이름은 바로 지움 (fd 는 살아 있음 → 테스트 끝나면 자동 삭제) */
        CHECK_INT(write(file_fd, big, blen), (long long)blen);   /* 4MB 패턴 기록 */

        /* http_send_response: 파일 [100, 100+3MB) 구간 */
        size_t flen = 3 * 1024 * 1024;
        http_response res = {.status = 200, .content_type = "application/octet-stream",
                             .use_file = 1, .file_fd = file_fd, .file_offset = 100, .body_len = flen};
        got = run_sender(send_resp_fn, &res, &len, &ok);
        CHECK(ok);
        const char *sep = strstr(got, "\r\n\r\n");
        CHECK(sep != NULL);
        if (sep) {
            CHECK_INT(len - (size_t)(sep + 4 - got), flen);      /* 정확히 요청한 길이만 */
            CHECK(memcmp(sep + 4, big + 100, flen) == 0);        /* offset 부터의 내용 */
            CHECK(strstr(got, "Content-Length: 3145728\r\n") != NULL);
        }
        free(got);

        /* 파일 위치(lseek) 는 바뀌지 않음 → 같은 fd 로 다시 보내도 결과가 같다 */
        got = run_sender(send_resp_fn, &res, &len, &ok);
        sep = strstr(got, "\r\n\r\n");
        CHECK(ok && sep != NULL && memcmp(sep + 4, big + 100, flen) == 0);
        free(got);

        /* 파일이 Content-Length 보다 짧음 → 송신 실패 (짧은 본문을 "성공" 으로 보내지 않음) */
        file_part fp = {.head = "H|", .file_fd = file_fd, .off = (off_t)blen - 10, .len = 20};
        got = run_sender(sendfile_fn, &fp, &len, &ok);
        CHECK(!ok);                                              /* 자식이 -1 받음 (EIO) */
        CHECK(len == 12 && memcmp(got, "H|", 2) == 0 && memcmp(got + 2, big + blen - 10, 10) == 0); /* 있는 만큼은 감 */
        free(got);

        /* tcp_sendfile_all: 머리 없이 파일만, 길이 0 */
        fp = (file_part){.head = NULL, .file_fd = file_fd, .off = 0, .len = 26};
        got = run_sender(sendfile_fn, &fp, &len, &ok);
        CHECK(ok);
        CHECK_STR(got, "abcdefghijklmnopqrstuvwxyz");
        free(got);
        fp = (file_part){.head = "only-head", .file_fd = file_fd, .off = 0, .len = 0};
        got = run_sender(sendfile_fn, &fp, &len, &ok);
        CHECK(ok);
        CHECK_STR(got, "only-head");
        free(got);

        /* 304 + 파일: 본문 안 보냄 */
        res.status = 304;
        got = run_sender(send_resp_fn, &res, &len, &ok);
        CHECK(ok && len > 4 && strcmp(got + len - 4, "\r\n\r\n") == 0);
        free(got);

        /* 받는 쪽이 끊음 → -1 */
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        close(sv[0]);
        CHECK_INT(tcp_sendfile_all(sv[1], "x", 1, file_fd, 0, blen, 1000), -1);
        close(sv[1]);

        /* 찔끔찔끔 읽는 클라이언트 (진짜 TCP, 버퍼 작게): 512B / 100ms = 5KB/s < TCP_SEND_MIN_RATE.
         * 진척은 계속 있어서 무진척 마감(2s)으로는 안 끊긴다 → 그것만 있으면 4MB 다 보낼 때까지 ~800초 붙잡힘.
         * 최저 속도 마감이 있으면 약 (2s + 버퍼분) / (1 - 5/16) ≈ 4~5초에 ETIMEDOUT. */
        {
            int lfd = tcp_listen(0, 1);                          /* 포트 0 = 아무 빈 포트 */
            struct sockaddr_in a;
            socklen_t al = sizeof(a);
            CHECK(lfd >= 0 && getsockname(lfd, (struct sockaddr *)&a, &al) == 0);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int cfd = socket(AF_INET, SOCK_STREAM, 0);
            int small = 4096;
            setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)); /* connect 전에 → 창 크기가 작게 잡힘 */
            CHECK_INT(connect(cfd, (struct sockaddr *)&a, sizeof(a)), 0);
            int sfd = accept(lfd, NULL, NULL);
            setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
            pid_t pid = fork();
            if (pid == 0) {                                      /* 자식: 느린 클라이언트 */
                close(sfd);
                close(lfd);
                char b[512];
                for (int i = 0; i < 600 && read(cfd, b, sizeof(b)) > 0; i++) /* 최대 60초 */
                    nanosleep(&(struct timespec){.tv_nsec = 100 * 1000000L}, NULL);
                _Exit(0);
            }
            close(cfd);
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            errno = 0;
            int rc = tcp_sendfile_all(sfd, "H", 1, file_fd, 0, blen, 2000);
            int err = errno;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long long dt = (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            CHECK_INT(rc, -1);
            CHECK_INT(err, ETIMEDOUT);
            CHECK(dt >= 2000 && dt < 12000);                     /* 무진척 마감보다는 길고, 다 보낼 때보다는 훨씬 짧게 */
            if (dt < 2000 || dt >= 12000)
                fprintf(stderr, "slow reader cut after %lld ms\n", dt);
            close(sfd);
            close(lfd);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }

        close(file_fd);
    }
    free(big);

    /* ---- 송신: 오류 응답 ---- */
    got = run_sender(send_err_404, NULL, &len, &ok);
    CHECK(ok);
    CHECK(strncmp(got, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(got, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    CHECK(strstr(got, "Content-Length: 14\r\n") != NULL);        /* "404 Not Found\n" = 14 바이트 */
    CHECK(strstr(got, "Connection: close\r\n") != NULL);         /* 오류 뒤에는 항상 close */
    CHECK(len > 14 && strcmp(got + len - 14, "404 Not Found\n") == 0);
    free(got);

    got = run_sender(send_err_405, NULL, &len, &ok);             /* 추가 헤더가 있는 오류 */
    CHECK(ok);
    CHECK(strncmp(got, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(strstr(got, "Allow: GET, POST\r\n") != NULL);
    free(got);

    /* ---- 송신: 상대가 이미 끊음 → -1 (SIGPIPE 는 무시됨) ---- */
    {
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        close(sv[0]);                                            /* 읽는 쪽을 닫아 둠 */
        CHECK_INT(http_send_error(sv[1], 500, NULL, 1000), -1);
        close(sv[1]);
    }

    /* ---- 송신: 잘못된 응답 → -1 + EINVAL (아무것도 보내지 않음) ---- */
    {
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        http_response bad = {.status = 200, .content_type = "text/plain", .extra_headers = "X: y\r\n\r\nevil"};
        errno = 0;
        CHECK_INT(http_send_response(sv[1], &bad, 1000), -1);
        CHECK_INT(errno, EINVAL);
        close(sv[1]);
        char c;
        CHECK_INT(read(sv[0], &c, 1), 0);                        /* 보낸 바이트 없이 EOF */
        close(sv[0]);
    }

    /* ---- tcp_sendv_all: 여러 조각 이어 보내기, 빈 조각 건너뛰기 ---- */
    {
        parts p = {.iov = {{"HEAD|", 5}, {NULL, 0}, {"body", 4}}, .cnt = 3}; /* 가운데 조각은 비어 있음 */
        got = run_sender(sendv_fn, &p, &len, &ok);
        CHECK(ok);
        CHECK_STR(got, "HEAD|body");
        free(got);

        parts one = {.iov = {{"only", 4}}, .cnt = 1};
        got = run_sender(sendv_fn, &one, &len, &ok);
        CHECK(ok);
        CHECK_STR(got, "only");
        free(got);

        parts none = {.iov = {{NULL, 0}, {NULL, 0}}, .cnt = 2};    /* 보낼 게 없음 → 성공, 0 바이트 */
        got = run_sender(sendv_fn, &none, &len, &ok);
        CHECK(ok && len == 0);
        free(got);
    }

    /* ---- tcp_sendv_all: 읽는 쪽이 안 읽으면 마감 시간에 맞춰 실패 ---- */
    {
        int sv[2];
        CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        size_t bigsz = 8 * 1024 * 1024;                          /* 소켓 버퍼를 넘겨서 반드시 막힘 */
        char *buf = calloc(bigsz, 1);
        struct iovec iov[2] = {{"x", 1}, {buf, bigsz}};
        errno = 0;
        CHECK_INT(tcp_sendv_all(sv[1], iov, 2, 200), -1);        /* 200ms 안에 못 보냄 */
        CHECK_INT(errno, ETIMEDOUT);
        free(buf);
        close(sv[0]);
        close(sv[1]);
    }

    return TEST_REPORT();
}
