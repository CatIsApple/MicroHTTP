#define _POSIX_C_SOURCE 200809L

#include "tcp_server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define RECV_BUF_SIZE 8192
#define CLIENT_TIMEOUT_MS 5000 /* 요청 수신, 응답 송신 각각의 전체 마감 시간 */

static volatile sig_atomic_t g_running = 1;

static void on_shutdown_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 미사용 → 블로킹 accept() 가 EINTR 로 깨어남 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    /* 쓰는 도중 클라이언트가 끊으면 SIGPIPE 로 프로세스가 죽는 것을 방지 */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        return -1;
    return 0;
}

static int parse_port(const char *s, uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

/* ---- 고정 응답: 내용이 바뀌지 않으므로 컴파일 시점에 완성해 둔다 ----
 * (요청마다 snprintf 로 다시 만들 필요 없음. 길이도 sizeof 로 구함) */
#define OK_BODY "Hello from MicroHTTP TCP server\n"
static const char RESP_200[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 32\r\n" /* = sizeof(OK_BODY) - 1, 아래 _Static_assert 로 검증 */
    "Connection: close\r\n"
    "\r\n" OK_BODY;
_Static_assert(sizeof(OK_BODY) - 1 == 32, "Content-Length 와 OK_BODY 길이가 다름");

/* 본문 없는 오류 응답 */
#define ERROR_RESP(status) \
    "HTTP/1.1 " status "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
static const char RESP_400[] = ERROR_RESP("400 Bad Request");
static const char RESP_431[] = ERROR_RESP("431 Request Header Fields Too Large");

static int g_quiet = 0; /* -q: 요청마다 찍는 로그 끔 (부하 테스트용) */

static int send_resp(int cfd, const char *peer, const char *resp, size_t len)
{
    if (tcp_send_all(cfd, resp, len, CLIENT_TIMEOUT_MS) < 0) {
        fprintf(stderr, "[%s] send error: %s\n", peer, strerror(errno));
        return -1;
    }
    return 0;
}

/* 연결 하나 처리: 요청 수신 → 응답 송신
 * (HTTP 파싱/응답 생성은 다음 단계에서 구현. 지금은 고정 응답)
 * 반환: 응답까지 보냈으면 0, 타임아웃·오류·빈 연결이면 -1 */
static int handle_client(int cfd, const char *peer)
{
    char buf[RECV_BUF_SIZE];
    size_t header_len;
    ssize_t n = tcp_recv_request(cfd, buf, sizeof(buf), CLIENT_TIMEOUT_MS, &header_len);
    if (n < 0) {
        if (errno == ETIMEDOUT)
            fprintf(stderr, "[%s] recv timeout\n", peer);
        else
            fprintf(stderr, "[%s] recv error: %s\n", peer, strerror(errno));
        return -1;
    }
    if (n == 0) {
        if (!g_quiet)
            printf("[%s] closed without data\n", peer);
        return -1;
    }

    /* 헤더 끝(빈 줄)을 못 찾은 경우: 잘린 요청을 정상으로 처리하지 않는다 */
    if (header_len == 0) {
        if ((size_t)n >= sizeof(buf) - 1) { /* 버퍼가 꽉 참 = 헤더가 너무 큼 */
            fprintf(stderr, "[%s] header too large (> %zu bytes) -> 431\n", peer, sizeof(buf) - 1);
            return send_resp(cfd, peer, RESP_431, sizeof(RESP_431) - 1);
        }
        /* 헤더가 끝나기 전에 상대가 송신을 닫음(half-close) */
        fprintf(stderr, "[%s] incomplete request (%zd bytes) -> 400\n", peer, n);
        return send_resp(cfd, peer, RESP_400, sizeof(RESP_400) - 1);
    }

    /* 첫 줄(요청 라인)만 로그 */
    if (!g_quiet) {
        size_t line_len = strcspn(buf, "\r\n");
        printf("[%s] %zu bytes header | %.*s\n", peer, header_len, (int)line_len, buf);
    }

    return send_resp(cfd, peer, RESP_200, sizeof(RESP_200) - 1);
}

/* ---- accept() 실패 처리 ----
 * fd 가 바닥나면(EMFILE/ENFILE) 대기 중인 연결이 계속 남아 있어서 accept 가
 * 즉시 다시 실패한다 → 그냥 continue 하면 CPU 100% 로 헛돈다.
 * 예비 fd 하나를 미리 열어 두었다가, 이때 잠깐 닫고 accept → close 로 그 연결을
 * 정리한 뒤 다시 예비 fd 를 연다. (libev/nginx 계열에서 쓰는 방법) */
static int g_spare_fd = -1;

static void handle_accept_error(int lfd)
{
    int err = errno;
    if ((err == EMFILE || err == ENFILE) && g_spare_fd >= 0) {
        close(g_spare_fd);
        int cfd = accept(lfd, NULL, NULL);
        if (cfd >= 0)
            close(cfd); /* 처리할 여력이 없으니 바로 끊는다 (클라이언트는 연결 거부를 봄) */
        g_spare_fd = open("/dev/null", O_RDONLY);
        fprintf(stderr, "accept: %s -> dropped one connection\n", strerror(err));
        return;
    }
    fprintf(stderr, "accept: %s\n", strerror(err));
    /* ENOBUFS/ENOMEM 같은 일시적 자원 부족: 잠깐 쉬어서 헛돌지 않게 한다 */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
    uint16_t port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            g_quiet = 1;
        } else if (parse_port(argv[i], &port) < 0) {
            fprintf(stderr, "usage: %s [-q] [port]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (install_signal_handlers() < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    int lfd = tcp_listen(port, BACKLOG);
    if (lfd < 0)
        return EXIT_FAILURE;

    g_spare_fd = open("/dev/null", O_RDONLY);

    /* stdout 버퍼링은 기본값 사용: 터미널이면 줄 단위, 파일·파이프면 블록 단위로
     * 모아서 쓴다(요청마다 write 시스템콜 X). 시작 메시지만 즉시 내보낸다. */
    printf("listening on 0.0.0.0:%u (Ctrl+C to stop)%s\n", (unsigned)port,
           g_quiet ? " [quiet]" : "");
    fflush(stdout);

    while (g_running) {
        char peer[64];
        int cfd = tcp_accept(lfd, peer, sizeof(peer));
        if (cfd < 0) {
            if (errno == EINTR)
                continue; /* 시그널 → 루프 조건에서 g_running 확인 */
            handle_accept_error(lfd);
            continue;
        }
        /* 반복 서버라 클라이언트 하나가 멈추면 전체가 멈춘다.
         * recv/send 에 "전체" 마감 시간을 줘서 한 연결이 쓸 수 있는 시간을 묶는다. */
        if (handle_client(cfd, peer) == 0)
            tcp_close_graceful(cfd); /* 응답을 보낸 경우만 RST 방지용 정상 종료 */
        else
            close(cfd); /* 이미 포기한 연결은 기다리지 않고 바로 닫음 */
    }

    close(lfd);
    if (g_spare_fd >= 0)
        close(g_spare_fd);
    printf("\nshutdown complete\n");
    return EXIT_SUCCESS;
}
