#define _POSIX_C_SOURCE 200809L

#include "tcp_server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define RECV_BUF_SIZE 8192
#define CLIENT_TIMEOUT_MS 5000

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

/* 연결 하나 처리: 요청 수신 → 응답 송신 → close
 * (HTTP 파싱/응답 생성은 다음 단계에서 구현. 지금은 고정 응답) */
static void handle_client(int cfd, const char *peer)
{
    char buf[RECV_BUF_SIZE];
    ssize_t n = tcp_recv_request(cfd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "[%s] recv timeout\n", peer);
        else
            fprintf(stderr, "[%s] recv error: %s\n", peer, strerror(errno));
        return;
    }
    if (n == 0) {
        printf("[%s] closed without data\n", peer);
        return;
    }

    /* 첫 줄(요청 라인)만 로그 */
    size_t line_len = strcspn(buf, "\r\n");
    printf("[%s] %zd bytes | %.*s\n", peer, n, (int)line_len, buf);

    static const char body[] = "Hello from MicroHTTP TCP server\n";
    char resp[256];
    int len = snprintf(resp, sizeof(resp),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain; charset=utf-8\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       sizeof(body) - 1, body);

    if (tcp_send_all(cfd, resp, (size_t)len) < 0)
        fprintf(stderr, "[%s] send error: %s\n", peer, strerror(errno));
}

int main(int argc, char *argv[])
{
    uint16_t port = DEFAULT_PORT;
    if (argc >= 2 && parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    int lfd = tcp_listen(port, BACKLOG);
    if (lfd < 0)
        return EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("listening on 0.0.0.0:%u (Ctrl+C to stop)\n", (unsigned)port);

    while (g_running) {
        char peer[64];
        int cfd = tcp_accept(lfd, peer, sizeof(peer));
        if (cfd < 0) {
            if (errno == EINTR)
                continue; /* 시그널 → 루프 조건에서 g_running 확인 */
            perror("accept");
            continue;
        }
        /* 반복 서버라 클라이언트 하나가 멈추면 전체가 멈춘다 → 타임아웃 필수 */
        if (tcp_set_timeout(cfd, CLIENT_TIMEOUT_MS) < 0)
            perror("setsockopt(timeout)");
        handle_client(cfd, peer);
        tcp_close_graceful(cfd);
    }

    close(lfd);
    printf("\nshutdown complete\n");
    return EXIT_SUCCESS;
}
