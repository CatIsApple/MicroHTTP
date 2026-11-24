/* test_functional.c — 기능 테스트: 실제 서버 실행 파일(./microhttp)을 띄우고 TCP 로 요청을 보낸다.
 *
 * curl 로 손으로 확인하던 것을 자동화한 것. 외부 도구(curl, nc) 없이 소켓으로 직접 보내서
 * macOS / Linux 어디서나 같은 결과가 나오게 했다. 단위 테스트가 못 보는 main.c 의 흐름
 * (431, 반쯤 닫힌 요청 400, echo, 100-continue, 비정상 종료 뒤 생존, SIGINT 종료) 을 검사한다.
 *
 * 서버 경로: 환경 변수 MICROHTTP_BIN (기본 "./microhttp"). make test 가 먼저 빌드해 둔다. */

#define _POSIX_C_SOURCE 200809L /* fork, execl, kill, nanosleep, inet_pton 등 POSIX 함수 */

#include "test.h"       /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <arpa/inet.h>  /* inet_pton, htons, ntohs */
#include <errno.h>      /* errno */
#include <fcntl.h>      /* open, O_WRONLY */
#include <netinet/in.h> /* struct sockaddr_in */
#include <signal.h>     /* kill, SIGINT, SIGKILL, signal, SIGPIPE */
#include <stdio.h>      /* snprintf, fprintf */
#include <stdlib.h>     /* getenv, malloc, free, strtol, _Exit */
#include <string.h>     /* strlen, strstr, strncmp, memset, memcpy */
#include <sys/socket.h> /* socket, connect, shutdown, setsockopt, getsockname */
#include <sys/time.h>   /* struct timeval (SO_RCVTIMEO) */
#include <sys/wait.h>   /* waitpid, WIFEXITED */
#include <time.h>       /* nanosleep */
#include <unistd.h>     /* fork, execl, read, write, close, dup2 */

static struct sockaddr_in g_addr; /* 서버 주소 (127.0.0.1:포트) */
static pid_t g_server = -1;       /* 서버 프로세스 */

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int write_all(int fd, const char *p, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* 서버에 연결 (읽기·쓰기 3초 제한 → 서버가 멈춰도 테스트는 안 멈춤). 실패 -1 */
static int connect_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&g_addr, sizeof(g_addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 커널이 골라 준 빈 포트 번호 (bind 포트 0 → getsockname) */
static unsigned free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    close(fd);
    return ntohs(a.sin_port);
}

/* 서버 실행 후 접속될 때까지 대기. 성공 0 */
static int start_server(const char *bin)
{
    unsigned port = free_port();
    memset(&g_addr, 0, sizeof(g_addr));
    g_addr.sin_family = AF_INET;
    g_addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &g_addr.sin_addr);

    char ps[16];
    snprintf(ps, sizeof(ps), "%u", port);
    g_server = fork();
    if (g_server == 0) {                         /* 자식: 서버 실행 (로그는 버림) */
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, 1);
        dup2(dn, 2);
        execl(bin, bin, "-q", ps, (char *)NULL);
        _Exit(127);                              /* exec 실패 (경로 틀림 등) */
    }
    for (int i = 0; i < 300; i++) {              /* 최대 3초 동안 접속 시도 */
        int fd = connect_server();
        if (fd >= 0) {
            close(fd);                           /* 빈 연결 (서버는 "closed without data" 로 처리) */
            return 0;
        }
        int st;
        if (waitpid(g_server, &st, WNOHANG) == g_server) /* 서버가 이미 죽음 */
            return -1;
        sleep_ms(10);
    }
    return -1;
}

static char g_resp[512 * 1024]; /* 마지막 응답 */
static size_t g_len;            /* 마지막 응답 길이 */
static int g_read_err;          /* 읽다가 오류(ECONNRESET 등)가 났으면 errno, 아니면 0 */

/* 요청을 보내고 서버가 닫을 때까지 응답을 전부 받는다. half_close: 보낸 뒤 송신 방향 닫기 (FIN) */
static void exchange(const char *req, size_t len, int half_close)
{
    g_len = 0;
    g_resp[0] = '\0';
    g_read_err = 0;
    int fd = connect_server();
    if (fd < 0) {
        g_read_err = errno;
        return;
    }
    write_all(fd, req, len);
    if (half_close)
        shutdown(fd, SHUT_WR);
    for (;;) {
        ssize_t r = read(fd, g_resp + g_len, sizeof(g_resp) - 1 - g_len);
        if (r < 0)
            g_read_err = errno;
        if (r <= 0)
            break;
        g_len += (size_t)r;
    }
    g_resp[g_len] = '\0';
    close(fd);
}

static void send_str(const char *req) { exchange(req, strlen(req), 0); }

static int status(void) /* "HTTP/1.1 404 ..." → 404, 형식이 아니면 -1 */
{
    if (g_len < 12 || strncmp(g_resp, "HTTP/1.1 ", 9) != 0)
        return -1;
    return (int)strtol(g_resp + 9, NULL, 10);
}

static const char *body(void) /* 빈 줄 뒤 */
{
    const char *p = strstr(g_resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* 응답 형식 검사: Content-Length 가 실제 본문 길이와 같고 Connection: close 이며 정상 EOF */
static int framing_ok(void)
{
    const char *b = body();
    const char *cl = strstr(g_resp, "\r\nContent-Length: ");
    if (b == NULL || cl == NULL || cl > b || g_read_err != 0)
        return 0;
    size_t want = (size_t)strtoull(cl + 18, NULL, 10);
    return want == g_len - (size_t)(b - g_resp) && strstr(g_resp, "\r\nConnection: close\r\n") < b;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *bin = getenv("MICROHTTP_BIN");
    if (bin == NULL)
        bin = "./microhttp";
    if (start_server(bin) != 0) {
        fprintf(stderr, "cannot start server: %s (run `make` first)\n", bin);
        return 1;
    }

    /* ---- 정상 요청 ---- */
    send_str("GET /hello?a=1 HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK(strstr(g_resp, "\r\nDate: ") != NULL && strstr(g_resp, "\r\nServer: microhttp\r\n") != NULL);
    CHECK(body() && strstr(body(), "uri: /hello?a=1\n") != NULL);

    send_str("POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc");
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK(g_len >= 3 && memcmp(g_resp + g_len - 3, "abc", 3) == 0); /* 본문이 끝에 그대로 */

    send_str("GET / HTTP/1.0\r\n\r\n");                   /* 1.0 은 Host 없어도 됨 */
    CHECK_INT(status(), 200);

    /* 큰 본문 (서버 버퍼 8KB 를 넘어 힙 경로) */
    {
        size_t L = 300 * 1024;
        char head[128];
        int hn = snprintf(head, sizeof(head), "POST /big HTTP/1.1\r\nHost: t\r\nContent-Length: %zu\r\n\r\n", L);
        char *req = malloc((size_t)hn + L);
        memcpy(req, head, (size_t)hn);
        for (size_t i = 0; i < L; i++)
            req[hn + i] = (char)('a' + i % 26);
        exchange(req, (size_t)hn + L, 0);
        CHECK_INT(status(), 200);
        CHECK(framing_ok());
        CHECK(g_len > L && memcmp(g_resp + g_len - L, req + hn, L) == 0); /* 그대로 돌아옴 */
        free(req);
    }

    /* Expect: 100-continue → 100 을 받은 뒤 본문 전송 */
    {
        int fd = connect_server();
        CHECK(fd >= 0);
        const char *h = "POST /c HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n";
        write_all(fd, h, strlen(h));
        char c100[64] = {0};
        ssize_t r = read(fd, c100, 25);                   /* "HTTP/1.1 100 Continue\r\n\r\n" = 25 */
        CHECK(r == 25 && strcmp(c100, "HTTP/1.1 100 Continue\r\n\r\n") == 0);
        write_all(fd, "wxyz", 4);
        g_len = 0;
        while ((r = read(fd, g_resp + g_len, sizeof(g_resp) - 1 - g_len)) > 0)
            g_len += (size_t)r;
        g_resp[g_len] = '\0';
        g_read_err = r < 0 ? errno : 0;
        close(fd);
        CHECK_INT(status(), 200);
        CHECK(framing_ok());
        CHECK(g_len >= 4 && memcmp(g_resp + g_len - 4, "wxyz", 4) == 0);
    }

    /* ---- 오류 응답 (readme: 잘못된 요청 / 매우 긴 URI / 매우 큰 본문) ---- */
    static const struct { const char *req; int code; } BAD[] = {
        {"garbage\r\n\r\n", 400},                                             /* 요청 라인 형식 오류 */
        {"GET / HTTP/1.1\r\n\r\n", 400},                                      /* 1.1 인데 Host 없음 */
        {"GET / HTTP/1.1\nHost: t\r\n\r\n", 400},                             /* 단독 LF */
        {"PUT / HTTP/1.1\r\nHost: t\r\n\r\n", 501},                           /* 지원 안 하는 메서드 */
        {"GET / HTTP/2.0\r\nHost: t\r\n\r\n", 505},                           /* 잘못된 버전 */
        {"POST / HTTP/1.1\r\nHost: t\r\n\r\n", 411},                          /* 길이 없는 POST */
        {"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 2000000\r\n\r\n", 413}, /* 1MB 초과 */
        {"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nHost: t\r\nExpect: foo\r\nContent-Length: 1\r\n\r\n", 417},
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        send_str(BAD[i].req);
        CHECK_INT(status(), BAD[i].code);
        CHECK(framing_ok());
        char want[64];
        snprintf(want, sizeof(want), "%d ", BAD[i].code);
        CHECK(body() && strncmp(body(), want, strlen(want)) == 0); /* 본문 "<코드> <이유>\n" */
    }

    /* URI 2049 → 414 */
    {
        static char req[4096];
        int n = snprintf(req, sizeof(req), "GET /");
        memset(req + n, 'a', 2048);
        snprintf(req + n + 2048, sizeof(req) - (size_t)n - 2048, " HTTP/1.1\r\nHost: t\r\n\r\n");
        send_str(req);
        CHECK_INT(status(), 414);
    }

    /* 헤더가 버퍼(8191바이트)보다 큼 → 431 */
    {
        static char req[12000];
        int n = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: t\r\nX-Big: ");
        memset(req + n, 'b', 9000);
        snprintf(req + n + 9000, sizeof(req) - (size_t)n - 9000, "\r\n\r\n");
        send_str(req);
        CHECK_INT(status(), 431);
        CHECK(framing_ok());                              /* 남은 바이트를 비우고 닫아서 RST 없이 받음 */
    }

    /* 헤더 도중 송신 종료(half-close) → 400 */
    exchange("GET / HTTP/1.1\r\nHost: t\r\n", 25, 1);
    CHECK_INT(status(), 400);

    /* 응답 뒤에 안 읽힌 바이트가 남아도 응답이 RST 로 잘리지 않음 (tcp_close_graceful) */
    {
        static char req[20000];
        int n = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        memset(req + n, 'x', 16000);
        exchange(req, (size_t)n + 16000, 0);
        CHECK_INT(status(), 200);
        CHECK(framing_ok());                              /* g_read_err == 0 포함 */
    }

    /* ---- 클라이언트 비정상 종료 뒤에도 서버가 살아 있음 ---- */
    {
        int fd = connect_server();                        /* 아무것도 안 보내고 닫음 */
        close(fd);

        fd = connect_server();                            /* 헤더 일부만 보내고 RST 로 끊음 */
        write_all(fd, "GET / HT", 8);
        struct linger lg = {.l_onoff = 1, .l_linger = 0}; /* linger 0 → close 가 RST 를 보냄 */
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);

        sleep_ms(50);
        send_str("GET /alive HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK_INT(status(), 200);                         /* 다음 요청은 정상 처리 */
    }

    /* ---- SIGINT → 정상 종료 (종료 코드 0) ---- */
    {
        kill(g_server, SIGINT);
        int st = 0, done = 0;
        for (int i = 0; i < 300 && !done; i++) {          /* 최대 3초 */
            if (waitpid(g_server, &st, WNOHANG) == g_server)
                done = 1;
            else
                sleep_ms(10);
        }
        CHECK(done);
        CHECK(done && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        if (!done) {                                      /* 안 죽었으면 강제 종료 */
            kill(g_server, SIGKILL);
            waitpid(g_server, NULL, 0);
        }
    }

    return TEST_REPORT();
}
