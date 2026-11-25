/* test_functional.c — 기능 테스트: 실제 서버 실행 파일(./microhttp)을 띄우고 TCP 로 요청을 보낸다.
 *
 * curl 로 손으로 확인하던 것을 자동화한 것. 외부 도구(curl, nc) 없이 소켓으로 직접 보내서
 * macOS / Linux 어디서나 같은 결과가 나오게 했다. 단위 테스트가 못 보는 main.c 의 흐름
 * (정적 파일 200/301/304/403/404·HEAD·큰 파일, 431, 반쯤 닫힌 요청 400, echo, 100-continue,
 *  비정상 종료 뒤 생존, keep-alive·파이프라이닝·쉬는 연결 타임아웃·쉬는 연결이 워커를 안 잡는지,
 *  쉬는 연결이 있는 채 SIGINT 종료, Reverse Proxy: 라운드 로빈·failover·헤더 다시 쓰기·chunked/EOF/1xx 중계·502/504,
 *  Forward Proxy: absolute-form 전달·Host 교체·CONNECT 터널·400/502/504)
 *  을 검사한다. 문서 루트는 임시 디렉터리에 만들어 -r 로 넘긴다. 프록시 백엔드는 fork 한 가짜 서버.
 * 서버는 기본 설정 / -t 2 -i 300 (쉬는 시간 타임아웃) / -t 2 (쉬는 연결 50개) / -P (리버스) / -F local (포워드) 로 띄운다.
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
#include <sys/stat.h>   /* mkdir */
#include <sys/time.h>   /* struct timeval (SO_RCVTIMEO) */
#include <sys/wait.h>   /* waitpid, WIFEXITED */
#include <time.h>       /* nanosleep */
#include <unistd.h>     /* fork, execl, read, write, close, dup2 */

static struct sockaddr_in g_addr; /* 서버 주소 (127.0.0.1:포트) */
static pid_t g_server = -1;       /* 서버 프로세스 */
static char g_root[] = "/tmp/microhttp_ft_XXXXXX"; /* 임시 문서 루트 */

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

/* 서버 실행 후 접속될 때까지 대기. 성공 0. threads / idle_ms 가 NULL 이 아니면 -t / -i 로 넘김.
 * extra: 포트 앞에 더 붙일 인자들 (NULL 로 끝나는 배열, 최대 12개) 또는 NULL */
static int start_server_ex(const char *bin, const char *threads, const char *idle_ms, const char *const *extra)
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
        char *args[24];                          /* bin -q -r root [-t n] [-i ms] [extra...] port NULL */
        int k = 0;
        args[k++] = (char *)bin;
        args[k++] = "-q";
        args[k++] = "-r";
        args[k++] = g_root;
        if (threads) {
            args[k++] = "-t";
            args[k++] = (char *)threads;
        }
        if (idle_ms) {
            args[k++] = "-i";
            args[k++] = (char *)idle_ms;
        }
        for (int e = 0; extra && extra[e] && e < 12; e++)
            args[k++] = (char *)extra[e];
        args[k++] = ps;
        args[k] = NULL;
        execv(bin, args);
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

static char g_resp[4 * 1024 * 1024]; /* 마지막 응답 (큰 파일 테스트용으로 넉넉히) */
static size_t g_len;            /* 마지막 응답 길이 */
static int g_read_err;          /* 읽다가 오류(ECONNRESET 등)가 났으면 errno, 아니면 0 */

/* buf[0..len) 의 맨 앞 응답 하나가 다 왔으면 그 전체 길이, 아직(또는 길이를 모름)이면 0.
 * head_req: HEAD 요청에 대한 응답 (본문 없음). 204 / 304 도 본문 없음 */
static size_t resp_len(const char *buf, size_t len, int head_req)
{
    const char *e = NULL;
    for (size_t i = 0; i + 4 <= len && e == NULL; i++)        /* 머리 끝 */
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0)
            e = buf + i;
    if (e == NULL)
        return 0;
    size_t hl = (size_t)(e - buf) + 4;
    int st = len >= 12 ? (int)strtol(buf + 9, NULL, 10) : 0;  /* "HTTP/1.1 200" */
    size_t cl = 0;
    if (!head_req && st != 204 && st != 304) {
        const char *c = NULL;                                 /* 머리 안의 Content-Length */
        for (const char *p = buf; p + 18 <= e && c == NULL; p++)
            if (memcmp(p, "\r\nContent-Length: ", 18) == 0)
                c = p + 18;
        if (c == NULL)
            return 0;                                         /* 길이를 모름 → EOF 까지 */
        cl = (size_t)strtoull(c, NULL, 10);
    }
    return len >= hl + cl ? hl + cl : 0;
}

static int start_server(const char *bin, const char *threads, const char *idle_ms)
{
    return start_server_ex(bin, threads, idle_ms, NULL);
}

/* ---- 프록시 테스트용 가짜 백엔드 (fork 한 자식 프로세스, 연결을 하나씩 순서대로 처리) ----
 * 경로별 동작:
 *   /api/slow     1.5초 동안 아무 답도 안 하고 닫음 (→ 504)
 *   /api/garbage  HTTP 가 아닌 응답 (→ 502)
 *   /api/chunked  chunked 응답 후 닫음
 *   /api/eof      길이 없이 보내고 닫음
 *   /api/continue 100 Continue 다음에 진짜 응답
 *   /api/short    Content-Length 100 이라 해 놓고 10바이트만 보내고 닫음
 *   /api/big      1MB 본문 (바이트마다 정해진 값)
 *   그 밖         200 + 받은 요청 요약 "id=A m=GET u=/api/x xff=... conn=... secret=... body=..." */
#define BIG_LEN (1024 * 1024)
static char big_byte(size_t i) { return (char)('a' + (i * 7 + i / 1000) % 26); }

/* 요청 머리 req[0..hl) 에서 "Name: 값" 의 값을 out 에 (없으면 "-") */
static void hcopy(const char *req, size_t hl, const char *name, char *out, size_t cap)
{
    char key[64];
    snprintf(key, sizeof(key), "\r\n%s: ", name);
    const char *h = strstr(req, key);
    if (h == NULL || (size_t)(h - req) >= hl) {
        snprintf(out, cap, "-");
        return;
    }
    h += strlen(key);
    size_t n = strcspn(h, "\r");
    if (n >= cap)
        n = cap - 1;
    memcpy(out, h, n);
    out[n] = '\0';
}

static void backend_loop(int lfd, char id)
{
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0)
            continue;
        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        static char req[65536];
        size_t n = 0;
        char *hend = NULL;
        while (n < sizeof(req) - 1 && hend == NULL) {           /* 머리 끝까지 */
            ssize_t r = read(c, req + n, sizeof(req) - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
            req[n] = '\0';
            hend = strstr(req, "\r\n\r\n");
        }
        if (hend == NULL) {
            close(c);
            continue;
        }
        size_t hl = (size_t)(hend + 4 - req);
        char v[512];
        hcopy(req, hl, "Content-Length", v, sizeof(v));
        size_t cl = v[0] == '-' ? 0 : (size_t)strtoul(v, NULL, 10);
        while (n < hl + cl && n < sizeof(req) - 1) {             /* 본문 끝까지 */
            ssize_t r = read(c, req + n, sizeof(req) - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
        }
        req[n] = '\0';
        char method[16] = "", uri[1024] = "";
        sscanf(req, "%15s %1023s", method, uri);

        if (strcmp(uri, "/api/slow") == 0) {
            sleep_ms(1500);
        } else if (strcmp(uri, "/api/garbage") == 0) {
            write_all(c, "NOT HTTP AT ALL\r\n\r\n", 19);
        } else if (strcmp(uri, "/api/chunked") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
            write_all(c, r, strlen(r));
        } else if (strcmp(uri, "/api/eof") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil-close";
            write_all(c, r, strlen(r));
        } else if (strcmp(uri, "/api/continue") == 0) {
            const char *r = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            write_all(c, r, strlen(r));
        } else if (strcmp(uri, "/api/short") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789";
            write_all(c, r, strlen(r));
        } else if (strcmp(uri, "/api/big") == 0) {
            char *b = malloc(BIG_LEN);
            for (size_t i = 0; i < BIG_LEN; i++)
                b[i] = big_byte(i);
            char h[128];
            int hn = snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", BIG_LEN);
            write_all(c, h, (size_t)hn);
            write_all(c, b, BIG_LEN);
            free(b);
        } else {
            char xff[256], conn[256], secret[256], host[256];
            hcopy(req, hl, "X-Forwarded-For", xff, sizeof(xff));
            hcopy(req, hl, "Connection", conn, sizeof(conn));
            hcopy(req, hl, "X-Secret", secret, sizeof(secret));
            hcopy(req, hl, "Host", host, sizeof(host));
            char body[2048];
            int bn = snprintf(body, sizeof(body), "id=%c m=%s u=%s xff=%s conn=%s secret=%s host=%s body=%.*s",
                              id, method, uri, xff, conn, secret, host, (int)(n - hl), req + hl);
            char h[256];
            int hn = snprintf(h, sizeof(h),
                              "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                              "Connection: close\r\nX-Backend: %c\r\n\r\n", bn, id);
            write_all(c, h, (size_t)hn);
            if (strcmp(method, "HEAD") != 0)                     /* HEAD 는 머리만 */
                write_all(c, body, (size_t)bn);
        }
        close(c);
    }
}

/* 백엔드 자식 프로세스 시작. *port 에 포트, 반환: pid (-1 실패) */
static pid_t start_backend(char id, unsigned *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 64) < 0) {
        close(lfd);
        return -1;
    }
    socklen_t len = sizeof(a);
    getsockname(lfd, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    pid_t pid = fork();
    if (pid == 0) {
        backend_loop(lfd, id);
        _Exit(0);
    }
    close(lfd);                                                  /* 부모는 안 씀 (자식이 들고 있음) */
    return pid;
}

/* fd 에서 응답을 받아 g_resp 에. 상대가 닫을 때(EOF·오류)까지 읽되,
 * stop: 0 = "Connection: keep-alive" 응답이 다 왔을 때만 멈춤 (close 응답은 EOF 까지 읽어서 RST 여부까지 본다)
 *       1 = 응답 하나가 다 오면 멈춤 / 2 = 멈추지 않고 EOF 까지 (파이프라이닝 응답 여러 개) */
static void read_resp(int fd, int head_req, int stop)
{
    g_len = 0;
    g_resp[0] = '\0';
    g_read_err = 0;
    for (;;) {
        ssize_t r = read(fd, g_resp + g_len, sizeof(g_resp) - 1 - g_len);
        if (r < 0)
            g_read_err = errno;
        if (r <= 0)
            break;
        g_len += (size_t)r;
        g_resp[g_len] = '\0';
        if (stop != 2 && resp_len(g_resp, g_len, head_req) != 0 &&           /* 2: 무조건 EOF 까지 (파이프라이닝) */
            (stop == 1 || strstr(g_resp, "\r\nConnection: keep-alive\r\n") != NULL))
            break;
    }
    g_resp[g_len] = '\0';
}

/* 요청을 보내고 응답을 받는다 (close 응답이면 서버가 닫을 때까지). half_close: 보낸 뒤 송신 방향 닫기 (FIN) */
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
    read_resp(fd, len >= 5 && memcmp(req, "HEAD ", 5) == 0, 0);
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
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 문서 루트에 파일 만들기 */
static void put_file(const char *name, const char *data, size_t len)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", g_root, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write_all(fd, data, len);
        close(fd);
    }
}

/* 응답 머리에서 헤더 값 위치 (없으면 NULL). 값은 다음 "\r\n" 까지 — 비교는 strncmp 로 */
static const char *header(const char *name)
{
    char key[64];
    snprintf(key, sizeof(key), "\r\n%s: ", name);
    const char *b = body();
    const char *h = strstr(g_resp, key);
    return (h && b && h < b) ? h + strlen(key) : NULL;
}

/* 응답 형식 검사: Content-Length 가 실제 본문 길이와 같고, Connection 헤더가 있고, 오류 없이 받음
 * (keep-alive 가 생긴 뒤로 정상 응답은 keep-alive, 오류 응답은 close — 그건 is_close / is_keep 으로 따로 본다) */
static int framing_ok(void)
{
    const char *b = body();
    const char *cl = strstr(g_resp, "\r\nContent-Length: ");
    if (b == NULL || cl == NULL || cl > b || g_read_err != 0)
        return 0;
    size_t want = (size_t)strtoull(cl + 18, NULL, 10);
    const char *conn = header("Connection");
    return want == g_len - (size_t)(b - g_resp) && conn != NULL &&
           (strncmp(conn, "close\r\n", 7) == 0 || strncmp(conn, "keep-alive\r\n", 12) == 0);
}

static int is_close(void) /* 응답 머리가 Connection: close */
{
    const char *conn = header("Connection");
    return conn != NULL && strncmp(conn, "close\r\n", 7) == 0;
}

static int is_keep(void) /* 응답 머리가 Connection: keep-alive */
{
    const char *conn = header("Connection");
    return conn != NULL && strncmp(conn, "keep-alive\r\n", 12) == 0;
}

/* SIGINT 를 보내고 limit_ms 안에 종료 코드 0 으로 끝나는지. 안 끝나면 강제 종료 */
static int stop_server(int limit_ms)
{
    kill(g_server, SIGINT);
    int st = 0, done = 0;
    for (int i = 0; i < limit_ms / 10 && !done; i++) {
        if (waitpid(g_server, &st, WNOHANG) == g_server)
            done = 1;
        else
            sleep_ms(10);
    }
    if (!done) {                                          /* 안 죽었으면 강제 종료 */
        fprintf(stderr, "server did not exit within %d ms\n", limit_ms);
        kill(g_server, SIGKILL);
        waitpid(g_server, NULL, 0);
    }
    g_server = -1;
    return done && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* 같은 연결에 요청 하나를 보내고 응답 하나를 받는다 (keep-alive 연결용) */
static void send_on(int fd, const char *req)
{
    write_all(fd, req, strlen(req));
    read_resp(fd, strncmp(req, "HEAD ", 5) == 0, 1);
}

/* keep-alive 연결 n 개를 열어 각각 GET 하나씩 보내고 응답을 받은 채 둔다. 성공한 개수 */
static int open_idle(int *fds, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        fds[i] = connect_server();
        if (fds[i] < 0)
            continue;
        send_on(fds[i], "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
        if (status() == 200 && is_keep())
            ok++;
    }
    return ok;
}

static void close_all(int *fds, int n)
{
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *bin = getenv("MICROHTTP_BIN");
    if (bin == NULL)
        bin = "./microhttp";

    /* 임시 문서 루트 */
    if (mkdtemp(g_root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    static const char INDEX[] = "<h1>index</h1>\n";
    put_file("index.html", INDEX, sizeof(INDEX) - 1);
    put_file("style.css", "body{}", 6);
    put_file("hello world.txt", "hi", 2);
    char sub[256];
    snprintf(sub, sizeof(sub), "%s/sub", g_root);
    mkdir(sub, 0755);
    put_file("sub/index.html", "sub", 3);
    put_file(".secret", "no", 2);
    size_t big_len = 3 * 1024 * 1024 + 123;                   /* 3MB + 조금: sendfile 이 여러 번 도는 크기 */
    char *big = malloc(big_len);
    for (size_t i = 0; i < big_len; i++)
        big[i] = (char)(i * 31 + (i >> 12));
    put_file("big.bin", big, big_len);

    if (start_server(bin, NULL, NULL) != 0) {
        fprintf(stderr, "cannot start server: %s (run `make` first)\n", bin);
        return 1;
    }

    /* ---- 정적 파일 ---- */
    send_str("GET / HTTP/1.1\r\nHost: t\r\n\r\n");          /* "/" → index.html */
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK(strstr(g_resp, "\r\nDate: ") != NULL && strstr(g_resp, "\r\nServer: microhttp\r\n") != NULL);
    CHECK_STR(body(), INDEX);
    CHECK(header("Content-Type") && strncmp(header("Content-Type"), "text/html; charset=utf-8\r\n", 26) == 0);
    CHECK(header("Last-Modified") != NULL);

    send_str("GET /style.css?v=1 HTTP/1.1\r\nHost: t\r\n\r\n"); /* 쿼리는 무시 */
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK_STR(body(), "body{}");
    CHECK(header("Content-Type") && strncmp(header("Content-Type"), "text/css", 8) == 0);

    send_str("GET /hello%20world.txt HTTP/1.1\r\nHost: t\r\n\r\n"); /* %20 디코딩 */
    CHECK_INT(status(), 200);
    CHECK_STR(body(), "hi");

    send_str("GET /sub/ HTTP/1.1\r\nHost: t\r\n\r\n");      /* 하위 디렉터리 index */
    CHECK_INT(status(), 200);
    CHECK_STR(body(), "sub");

    send_str("GET /sub?x=1 HTTP/1.1\r\nHost: t\r\n\r\n");   /* '/' 빠진 디렉터리 → 301 */
    CHECK_INT(status(), 301);
    CHECK(framing_ok());
    CHECK(header("Location") && strncmp(header("Location"), "/sub/?x=1\r\n", 11) == 0);

    send_str("GET /nope.html HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK_INT(status(), 404);
    CHECK(framing_ok());
    CHECK_STR(body(), "404 Not Found\n");

    send_str("GET /../../etc/passwd HTTP/1.1\r\nHost: t\r\n\r\n"); /* 디렉터리 탈출 */
    CHECK_INT(status(), 403);
    send_str("GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK_INT(status(), 403);
    send_str("GET /.secret HTTP/1.1\r\nHost: t\r\n\r\n");   /* 숨김 파일 */
    CHECK_INT(status(), 403);
    CHECK(body() && strstr(body(), "no") == NULL);

    /* 조건부 GET: 받은 Last-Modified 를 그대로 보내면 304 (본문·Content-Length 없음) */
    {
        send_str("GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
        char lm[64] = {0};
        const char *h = header("Last-Modified");
        if (h)
            snprintf(lm, sizeof(lm), "%.*s", (int)strcspn(h, "\r"), h);
        char req[256];
        snprintf(req, sizeof(req), "GET /style.css HTTP/1.1\r\nHost: t\r\nIf-Modified-Since: %s\r\n\r\n", lm);
        send_str(req);
        CHECK_INT(status(), 304);
        CHECK(body() && *body() == '\0');                       /* 본문 없음 */
        CHECK(header("Content-Length") == NULL);
        CHECK(g_read_err == 0);
    }

    /* HEAD: 머리는 GET 과 같고 (Content-Length = 파일 크기) 본문은 없음 */
    send_str("HEAD /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK_INT(status(), 200);
    CHECK(header("Content-Length") && strncmp(header("Content-Length"), "6\r\n", 3) == 0);
    CHECK(body() && *body() == '\0');
    send_str("HEAD /nope HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK_INT(status(), 404);
    CHECK(body() && *body() == '\0');

    /* 큰 파일: 내용이 바이트 단위로 같아야 함 (sendfile 이 여러 번 나눠 보내는 경로) */
    send_str("GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK(header("Content-Type") && strncmp(header("Content-Type"), "application/octet-stream", 24) == 0);
    CHECK(body() && g_len - (size_t)(body() - g_resp) == big_len && memcmp(body(), big, big_len) == 0);

    /* 작은 파일 응답이 지연 없이 옴 (머리 MSG_MORE 가 sendfile 뒤에 밀려 나가지 않고 남아 있으면
     * 커널 cork 타이머 200ms, Nagle + 지연 ACK 면 40ms 씩 걸린다) → 20번에 1초 미만 */
    {
        long long t0 = now_ms();
        int ok = 1;
        for (int i = 0; i < 20; i++) {
            send_str("GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
            ok = ok && status() == 200;
        }
        long long dt = now_ms() - t0;
        CHECK(ok);
        CHECK(dt < 1000);
        if (dt >= 1000)
            fprintf(stderr, "20 small GETs took %lld ms\n", dt);
    }

    /* ---- POST → echo ---- */
    send_str("POST /hello?a=1 HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
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
        read_resp(fd, 0, 1);                              /* 응답 하나 (keep-alive 라 서버는 안 닫음) */
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
        CHECK(is_close());                                /* 오류 응답은 항상 close */
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
        CHECK(body() && strcmp(body(), "414 URI Too Long\n") == 0);
        memmove(req + 1, req, strlen(req) + 1);           /* 같은 요청을 HEAD 로: 한 칸 밀고 */
        memcpy(req, "HEAD", 4);                           /* "GET" → "HEAD" (오류여도 본문 없어야 함) */
        send_str(req);
        CHECK_INT(status(), 414);
        CHECK(header("Content-Length") != NULL);          /* 머리는 GET 과 같음 */
        CHECK(body() && *body() == '\0');
    }
    /* HEAD + 파서 오류 → 오류 응답도 본문 없음 */
    send_str("HEAD / HTTP/1.1\r\n\r\n");                  /* Host 없음 → 400 */
    CHECK_INT(status(), 400);
    CHECK(body() && *body() == '\0');

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
        int n = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"); /* close 경로 */
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
        send_str("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK_INT(status(), 200);                         /* 다음 요청은 정상 처리 */
    }

    /* ---- keep-alive ---- */

    /* 한 연결에서 GET 두 번 순차: 둘 다 200, 첫 응답은 keep-alive (Content-Length 만큼만 읽고 다음 요청) */
    {
        int fd = connect_server();
        CHECK(fd >= 0);
        send_on(fd, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK_INT(status(), 200);
        CHECK(framing_ok());
        CHECK(is_keep());
        CHECK_STR(body(), "body{}");
        send_on(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");     /* 같은 연결로 두 번째 */
        CHECK_INT(status(), 200);
        CHECK(framing_ok());
        CHECK_STR(body(), INDEX);
        send_on(fd, "HEAD /style.css HTTP/1.1\r\nHost: t\r\n\r\n"); /* HEAD 도 keep-alive (본문 없음) */
        CHECK_INT(status(), 200);
        CHECK(is_keep());
        CHECK(body() && *body() == '\0');
        send_on(fd, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n");  /* 404 는 요청을 끝까지 읽은 정상 응답 → 연결 유지 */
        CHECK_INT(status(), 404);
        CHECK(framing_ok());
        CHECK(is_keep());
        CHECK_STR(body(), "404 Not Found\n");
        send_on(fd, "GET /nope HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"); /* 클라가 close 요청 */
        CHECK_INT(status(), 404);
        CHECK(is_close());
        char c;
        CHECK(read(fd, &c, 1) == 0);                          /* 서버가 닫음 */
        close(fd);
    }

    /* 파이프라이닝: GET 두 개를 한 번에 → 응답 두 개가 순서대로 (두 번째가 close 라 서버가 닫음) */
    {
        int fd = connect_server();
        CHECK(fd >= 0);
        const char *two = "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n"
                          "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        write_all(fd, two, strlen(two));
        read_resp(fd, 0, 2);                                  /* EOF 까지 (두 응답 모두) */
        close(fd);
        CHECK(g_read_err == 0);
        size_t first = resp_len(g_resp, g_len, 0);
        CHECK(first > 0);
        CHECK_INT(status(), 200);
        CHECK(is_keep());
        CHECK(first > 6 && memcmp(g_resp + first - 6, "body{}", 6) == 0);
        if (first > 0 && first < g_len) {                     /* 두 번째 응답만 앞으로 당겨서 같은 검사 */
            memmove(g_resp, g_resp + first, g_len - first + 1);
            g_len -= first;
            CHECK_INT(status(), 200);
            CHECK(framing_ok());                              /* 남은 길이 = 정확히 응답 하나 */
            CHECK(is_close());
            CHECK_STR(body(), INDEX);
        } else {
            CHECK(!"second pipelined response missing");
        }
    }

    /* 파이프라이닝 + 본문: POST(본문 3바이트) 바로 뒤에 GET → 본문 뒤 잔여가 다음 요청으로 */
    {
        int fd = connect_server();
        CHECK(fd >= 0);
        const char *two = "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc"
                          "GET /style.css HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        write_all(fd, two, strlen(two));
        read_resp(fd, 0, 2);                                  /* EOF 까지 */
        close(fd);
        size_t first = resp_len(g_resp, g_len, 0);
        CHECK_INT(status(), 200);
        CHECK(first > 3 && memcmp(g_resp + first - 3, "abc", 3) == 0); /* echo 본문 끝 = abc */
        if (first > 0 && first < g_len) {
            memmove(g_resp, g_resp + first, g_len - first + 1);
            g_len -= first;
            CHECK_INT(status(), 200);
            CHECK(framing_ok());
            CHECK_STR(body(), "body{}");
        } else {
            CHECK(!"response after POST missing");
        }
    }

    /* Connection: close 요청 → close 응답 뒤 서버가 닫음 (exchange 는 EOF 까지 읽음, 오류·타임아웃 없어야 함) */
    send_str("GET /style.css HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK(is_close());

    /* HTTP/1.0 (keep-alive 헤더 없음) → close / HTTP/1.0 + Connection: keep-alive → 유지 */
    send_str("GET /style.css HTTP/1.0\r\n\r\n");
    CHECK_INT(status(), 200);
    CHECK(framing_ok());
    CHECK(is_close());
    {
        int fd = connect_server();
        CHECK(fd >= 0);
        send_on(fd, "GET /style.css HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        CHECK_INT(status(), 200);
        CHECK(is_keep());
        send_on(fd, "GET /style.css HTTP/1.0\r\n\r\n");
        CHECK_INT(status(), 200);
        CHECK(is_close());
        close(fd);
    }

    /* ---- SIGINT → 정상 종료 (종료 코드 0): keep-alive 로 쉬는 연결이 열려 있어도 2초 안에 ---- */
    {
        int fds[8];
        CHECK_INT(open_idle(fds, 8), 8);
        CHECK(stop_server(2000));
        close_all(fds, 8);
    }

    /* ---- 쉬는 연결 타임아웃: -i 300 → 응답 뒤 아무것도 안 보내면 ~0.3초 뒤 서버가 닫음 ---- */
    if (start_server(bin, "2", "300") != 0) {
        CHECK(!"cannot start server -t 2 -i 300");
    } else {
        int fd = connect_server();
        CHECK(fd >= 0);
        send_on(fd, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
        CHECK_INT(status(), 200);
        CHECK(is_keep());
        long long t0 = now_ms();
        char c;
        ssize_t r = read(fd, &c, 1);                          /* 서버가 닫으면 0 (SO_RCVTIMEO 3초) */
        long long dt = now_ms() - t0;
        CHECK_INT(r, 0);
        CHECK(dt >= 200 && dt < 1500);
        if (r != 0 || dt < 200 || dt >= 1500)
            fprintf(stderr, "idle close: r=%zd after %lld ms\n", r, dt);
        close(fd);
        CHECK(stop_server(2000));
    }

    /* ---- 쉬는 연결이 워커를 안 잡음: 워커 2개 서버에 keep-alive 연결 50개가 쉬는 중에도 새 요청이 바로 처리됨 ---- */
    if (start_server(bin, "2", NULL) != 0) {                  /* 쉬는 시간은 기본(5초) → 테스트 동안 안 닫힘 */
        CHECK(!"cannot start server -t 2");
    } else {
        int fds[50];
        CHECK_INT(open_idle(fds, 50), 50);
        long long t0 = now_ms();
        send_str("GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
        long long dt = now_ms() - t0;
        CHECK_INT(status(), 200);
        CHECK(dt < 1000);
        int fd = fds[0];                                      /* 쉬던 연결도 다시 쓸 수 있음 (poller → 워커) */
        if (fd >= 0) {
            send_on(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK_STR(body(), INDEX);
        }
        CHECK(stop_server(2000));                             /* 쉬는 연결 50개를 둔 채 종료 */
        close_all(fds, 50);
    }

    /* ---- 빈 연결이 워커를 안 잡음: 워커 2개 서버에 "연결만 하고 아무것도 안 보내는" 연결 20개가 있어도
     *      새 요청이 바로 처리됨 (예전엔 새 연결이 곧장 워커로 가서 recv 에서 5초씩 붙잡혔다) ---- */
    if (start_server(bin, "2", NULL) != 0) {
        CHECK(!"cannot start server -t 2 (empty conns)");
    } else {
        int fds[20];
        for (int i = 0; i < 20; i++)
            fds[i] = connect_server();                        /* handshake 만 하고 침묵 */
        sleep_ms(100);                                        /* 서버가 모두 accept 할 시간 */
        long long t0 = now_ms();
        send_str("GET /style.css HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        long long dt = now_ms() - t0;
        CHECK_INT(status(), 200);
        CHECK(dt < 1000);
        if (dt >= 1000)
            fprintf(stderr, "request behind empty conns took %lld ms\n", dt);
        if (fds[0] >= 0) {                                    /* 침묵하던 연결도 나중에 보내면 정상 처리 (poller → 워커) */
            send_on(fds[0], "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK_STR(body(), INDEX);
        }
        CHECK(stop_server(2000));                             /* 빈 연결을 둔 채 SIGINT → 바로 종료 */
        close_all(fds, 20);
    }

    /* ---- Reverse Proxy: 가짜 백엔드 A, B + 꺼진 포트 하나 ---- */
    {
        unsigned pa = 0, pb = 0;
        pid_t ba = start_backend('A', &pa), bb = start_backend('B', &pb);
        unsigned dead = free_port();                              /* 아무도 안 듣는 포트 → connect 거절 */
        CHECK(ba > 0 && bb > 0);
        char r_api[128], r_down[128], r_fo[128];
        snprintf(r_api, sizeof(r_api), "/api/=127.0.0.1:%u,127.0.0.1:%u", pa, pb);
        snprintf(r_down, sizeof(r_down), "/down/=127.0.0.1:%u", dead);
        snprintf(r_fo, sizeof(r_fo), "/fo/=127.0.0.1:%u,127.0.0.1:%u", dead, pa); /* 하나는 죽어 있음 → failover */
        const char *extra[] = {"-P", r_api, "-P", r_down, "-P", r_fo, "-T", "300", NULL};
        if (ba <= 0 || bb <= 0 || start_server_ex(bin, NULL, NULL, extra) != 0) {
            CHECK(!"cannot start proxy server");
        } else {
            /* 라운드 로빈: 연속 두 요청이 A, B 에 하나씩 */
            send_str("GET /api/x HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK(framing_ok());
            CHECK(is_keep());                                     /* 백엔드는 close 였지만 길이를 아니까 클라이언트는 유지 */
            char first = body() && strncmp(body(), "id=", 3) == 0 ? body()[3] : '?';
            send_str("GET /api/x HTTP/1.1\r\nHost: t\r\n\r\n");
            char second = body() && strncmp(body(), "id=", 3) == 0 ? body()[3] : '?';
            CHECK((first == 'A' && second == 'B') || (first == 'B' && second == 'A'));
            CHECK(header("X-Backend") != NULL);                   /* 백엔드 헤더는 그대로 전달 */

            /* 요청 다시 쓰기: Host 유지, X-Forwarded-For 덧붙임, hop-by-hop 제거, 백엔드에는 Connection: close */
            send_str("GET /api/q?a=1 HTTP/1.1\r\nHost: shop.example\r\nX-Forwarded-For: 9.9.9.9\r\n"
                     "Connection: keep-alive, X-Secret\r\nX-Secret: s3\r\nKeep-Alive: timeout=5\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK(body() && strstr(body(), " u=/api/q?a=1 ") != NULL);
            CHECK(body() && strstr(body(), " xff=9.9.9.9, 127.0.0.1 ") != NULL);
            CHECK(body() && strstr(body(), " conn=close ") != NULL);
            CHECK(body() && strstr(body(), " secret=- ") != NULL);  /* Connection 에 적힌 헤더는 안 넘어감 */
            CHECK(body() && strstr(body(), " host=shop.example ") != NULL);

            /* 한 연결에서 프록시 요청 여러 번 + 사이에 정적 파일 (keep-alive 유지) */
            {
                int fd = connect_server();
                CHECK(fd >= 0);
                send_on(fd, "GET /api/k1 HTTP/1.1\r\nHost: t\r\n\r\n");
                CHECK_INT(status(), 200);
                CHECK(is_keep());
                send_on(fd, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
                CHECK_INT(status(), 200);
                CHECK_STR(body(), "body{}");
                send_on(fd, "POST /api/k2 HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello");
                CHECK_INT(status(), 200);
                CHECK(body() && strstr(body(), " m=POST ") != NULL);
                CHECK(body() && strstr(body(), "body=hello") != NULL);
                send_on(fd, "HEAD /api/h HTTP/1.1\r\nHost: t\r\n\r\n");  /* HEAD: 머리만, 연결 유지 */
                CHECK_INT(status(), 200);
                CHECK(body() && body()[0] == '\0');
                CHECK(is_keep());
                send_on(fd, "PUT /api/item/1 HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc"); /* 프록시는 메서드 무관 */
                CHECK_INT(status(), 200);
                CHECK(body() && strstr(body(), " m=PUT u=/api/item/1 ") != NULL);
                CHECK(body() && strstr(body(), "body=abc") != NULL);
                close(fd);
            }
            send_str("DELETE /style.css HTTP/1.1\r\nHost: t\r\n\r\n"); /* 프록시 경로가 아니면 여전히 501 */
            CHECK_INT(status(), 501);

            /* 백엔드 응답 형식들 */
            send_str("GET /api/chunked HTTP/1.1\r\nHost: t\r\n\r\n");   /* chunked: 그대로 중계, 클라이언트는 close */
            CHECK_INT(status(), 200);
            CHECK(is_close());
            CHECK(header("Transfer-Encoding") && strncmp(header("Transfer-Encoding"), "chunked\r\n", 9) == 0);
            CHECK_STR(body(), "5\r\nhello\r\n0\r\n\r\n");
            CHECK_INT(g_read_err, 0);
            send_str("GET /api/eof HTTP/1.1\r\nHost: t\r\n\r\n");       /* 길이 없음: 백엔드가 닫을 때까지 */
            CHECK_INT(status(), 200);
            CHECK(is_close());
            CHECK_STR(body(), "until-close");
            send_str("GET /api/continue HTTP/1.1\r\nHost: t\r\n\r\n");  /* 100 Continue 는 삼키고 진짜 응답만 */
            CHECK_INT(status(), 200);
            CHECK(framing_ok());
            CHECK_STR(body(), "ok");
            send_str("GET /api/big HTTP/1.1\r\nHost: t\r\n\r\n");       /* 1MB 를 16KB 버퍼로 중계 */
            CHECK_INT(status(), 200);
            CHECK(framing_ok());
            {
                const char *b = body();
                int same = b != NULL && g_len - (size_t)(b - g_resp) == BIG_LEN;
                for (size_t i = 0; same && i < BIG_LEN; i++)
                    same = b[i] == big_byte(i);
                CHECK(same);
            }
            send_str("GET /api/short HTTP/1.1\r\nHost: t\r\n\r\n");     /* 백엔드가 덜 보내고 닫음 → 잘린 채 연결 끊김 */
            CHECK_INT(status(), 200);
            CHECK(!framing_ok());
            CHECK(body() && strcmp(body(), "0123456789") == 0);

            /* 오류: 502 / 504 */
            send_str("GET /api/garbage HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 502);
            CHECK(framing_ok());
            CHECK(is_close());
            send_str("GET /down/x HTTP/1.1\r\nHost: t\r\n\r\n");         /* 백엔드가 꺼져 있음 */
            CHECK_INT(status(), 502);
            long long t0 = now_ms();
            send_str("GET /api/slow HTTP/1.1\r\nHost: t\r\n\r\n");       /* -T 300 → 0.3초 뒤 504 */
            long long dt = now_ms() - t0;
            CHECK_INT(status(), 504);
            CHECK(dt >= 250 && dt < 1200);
            if (dt < 250 || dt >= 1200)
                fprintf(stderr, "504 after %lld ms\n", dt);
            sleep_ms(1300);                                       /* slow 를 처리하느라 막힌 백엔드가 풀릴 때까지 */

            /* failover: 첫 후보가 죽어 있어도 다음 백엔드로 → 두 번 다 성공 */
            send_str("GET /fo/a HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK(body() && strncmp(body(), "id=A", 4) == 0);
            send_str("GET /fo/b HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK(body() && strncmp(body(), "id=A", 4) == 0);

            /* dot-segment 로 /api/ 경계를 넘으려는 요청은 백엔드로 가지 않음 */
            send_str("GET /api/../style.css HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK(status() != 200 || (body() && strstr(body(), "id=") == NULL));

            /* 포워드 프록시는 꺼져 있음: CONNECT → 501, absolute-form → 기존처럼 정적 파일 */
            send_str("CONNECT 127.0.0.1:1 HTTP/1.1\r\nHost: 127.0.0.1:1\r\n\r\n");
            CHECK_INT(status(), 501);
            send_str("GET http://elsewhere.example/style.css HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            CHECK_STR(body(), "body{}");
            CHECK(stop_server(2000));
        }

        /* ---- Forward Proxy (-F local): 목적지는 클라이언트가 고름 ---- */
        const char *fextra[] = {"-F", "local", "-T", "300", NULL};
        if (ba <= 0 || start_server_ex(bin, NULL, NULL, fextra) != 0) {
            CHECK(!"cannot start forward proxy server");
        } else {
            char req[512];
            /* absolute-form: 요청 줄은 origin-form 으로, Host 는 대상 것으로 바뀌어 A 에 도착 */
            snprintf(req, sizeof(req),
                     "GET http://127.0.0.1:%u/f/x?y=1 HTTP/1.1\r\nHost: wrong.example\r\n"
                     "Proxy-Connection: keep-alive\r\nProxy-Authorization: Basic eDp5\r\n\r\n", pa);
            send_str(req);
            CHECK_INT(status(), 200);
            CHECK(framing_ok());
            CHECK(is_keep());
            char want[128];
            snprintf(want, sizeof(want), " u=/f/x?y=1 ");
            CHECK(body() && strncmp(body(), "id=A m=GET", 10) == 0);
            CHECK(body() && strstr(body(), want) != NULL);
            snprintf(want, sizeof(want), " host=127.0.0.1:%u ", pa);
            CHECK(body() && strstr(body(), want) != NULL);
            CHECK(body() && strstr(body(), " xff=127.0.0.1 ") != NULL);

            /* 한 연결에서 포워드 요청 + 정적 파일 + POST 포워드 (keep-alive 유지) */
            {
                int fd = connect_server();
                CHECK(fd >= 0);
                snprintf(req, sizeof(req), "GET http://127.0.0.1:%u/k1 HTTP/1.1\r\nHost: t\r\n\r\n", pa);
                send_on(fd, req);
                CHECK_INT(status(), 200);
                CHECK(is_keep());
                send_on(fd, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
                CHECK_INT(status(), 200);
                CHECK_STR(body(), "body{}");
                snprintf(req, sizeof(req),
                         "POST http://127.0.0.1:%u/p HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nbody", pa);
                send_on(fd, req);
                CHECK_INT(status(), 200);
                CHECK(body() && strstr(body(), " m=POST u=/p ") != NULL);
                CHECK(body() && strstr(body(), "body=body") != NULL);
                close(fd);
            }

            /* 오류: 꺼진 목적지 → 502, 대상 형식 오류 → 400, 느린 목적지 → 504 */
            snprintf(req, sizeof(req), "GET http://127.0.0.1:%u/ HTTP/1.1\r\nHost: t\r\n\r\n", dead);
            send_str(req);
            CHECK_INT(status(), 502);
            send_str("GET http://u@127.0.0.1/ HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 400);
            send_str("GET http://no-such-host.invalid/ HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 502);
            snprintf(req, sizeof(req), "GET http://127.0.0.1:%u/api/slow HTTP/1.1\r\nHost: t\r\n\r\n", pb);
            send_str(req);
            CHECK_INT(status(), 504);
            sleep_ms(1300);                                       /* B 가 slow 처리에서 풀릴 때까지 */

            /* CONNECT 터널: 200 뒤에 보낸 바이트가 그대로 A 에 가고, A 의 응답이 그대로 돌아옴 */
            {
                int fd = connect_server();
                CHECK(fd >= 0);
                snprintf(req, sizeof(req), "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", pa, pa);
                write_all(fd, req, strlen(req));
                char got[64] = "";
                size_t gl = 0;
                const char *est = "HTTP/1.1 200 Connection Established\r\n\r\n";
                while (gl < strlen(est)) {                        /* 터널 성립 응답만 먼저 받음 */
                    ssize_t r = read(fd, got + gl, strlen(est) - gl);
                    if (r <= 0)
                        break;
                    gl += (size_t)r;
                }
                got[gl] = '\0';
                CHECK_STR(got, est);
                const char *inner = "GET /tunneled HTTP/1.1\r\nHost: inner\r\nX-Secret: through\r\n\r\n";
                write_all(fd, inner, strlen(inner));              /* 터널 안의 요청 (프록시는 해석하지 않음) */
                read_resp(fd, 0, 2);                              /* A 가 응답 후 닫음 → 터널도 닫힘 */
                CHECK_INT(status(), 200);
                CHECK(body() && strstr(body(), " u=/tunneled ") != NULL);
                CHECK(body() && strstr(body(), " secret=through ") != NULL); /* 헤더도 손대지 않음 */
                CHECK(body() && strstr(body(), " xff=- ") != NULL);           /* 터널은 X-Forwarded-For 를 못 붙임 */
                CHECK_INT(g_read_err, 0);
                close(fd);
            }
            /* CONNECT 와 첫 바이트를 한 번에 보내도(파이프라이닝) 앞 바이트가 사라지지 않음 */
            {
                int fd = connect_server();
                CHECK(fd >= 0);
                snprintf(req, sizeof(req),
                         "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: x\r\n\r\nGET /early HTTP/1.1\r\nHost: i\r\n\r\n", pa);
                write_all(fd, req, strlen(req));
                read_resp(fd, 0, 2);
                const char *pre = "HTTP/1.1 200 Connection Established\r\n\r\nHTTP/1.1 200 OK\r\n";
                CHECK(strncmp(g_resp, pre, strlen(pre)) == 0);
                CHECK(strstr(g_resp, " u=/early ") != NULL);
                close(fd);
            }
            snprintf(req, sizeof(req), "CONNECT 127.0.0.1:%u HTTP/1.1\r\nHost: x\r\n\r\n", dead);
            send_str(req);                                        /* 목적지 꺼짐 → 502 (터널 안 열림) */
            CHECK_INT(status(), 502);
            send_str("CONNECT nohost HTTP/1.1\r\nHost: x\r\n\r\n"); /* 포트 없음 → 400 */
            CHECK_INT(status(), 400);

            /* 포워드 대상이 아닌 일반 요청은 그대로 */
            send_str("GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 200);
            send_str("DELETE /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
            CHECK_INT(status(), 501);
            CHECK(stop_server(2000));
        }
        if (ba > 0) {
            kill(ba, SIGKILL);
            waitpid(ba, NULL, 0);
        }
        if (bb > 0) {
            kill(bb, SIGKILL);
            waitpid(bb, NULL, 0);
        }
    }

    /* 문서 루트 정리 */
    free(big);
    const char *names[] = {"index.html", "style.css", "hello world.txt", "sub/index.html", ".secret", "big.bin"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/%s", g_root, names[i]);
        unlink(p);
    }
    rmdir(sub);
    rmdir(g_root);

    return TEST_REPORT();
}
