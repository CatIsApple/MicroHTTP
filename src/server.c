/* server.c — 서버 수명 관리 */
#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "conn.h"
#include "tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128

static volatile sig_atomic_t g_stop = 0;

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1; /* 핸들러에서는 플래그만 세운다 (printf 등은 시그널 핸들러에서 안전하지 않음) */
}

static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop;
    sa.sa_flags = 0; /* SA_RESTART 를 안 줘야 accept() 가 EINTR 로 깨어나 g_stop 을 확인한다 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    /* 끊긴 연결에 쓰면 SIGPIPE 로 프로세스가 죽는다 → 무시하고 send 의 EPIPE 로 처리 */
    sa.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &sa, NULL);
}

/* fd 가 바닥났을 때(EMFILE/ENFILE) 대비용 예비 fd.
 * fd 가 없으면 accept 가 실패하는데, 대기 중인 연결은 큐에 그대로 남아서
 * 다음 accept 도 즉시 실패한다 → CPU 를 100% 쓰며 헛돈다.
 * 그때 예비 fd 를 잠깐 내놓고 연결 하나를 받아 바로 닫아서 큐를 비운다. */
static int g_reserve_fd = -1;

static void on_accept_error(int lfd)
{
    int err = errno;
    if ((err == EMFILE || err == ENFILE) && g_reserve_fd >= 0) {
        close(g_reserve_fd);
        int fd = accept(lfd, NULL, NULL);
        if (fd >= 0)
            close(fd);
        g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        fprintf(stderr, "accept: %s (connection dropped)\n", strerror(err));
        return;
    }
    fprintf(stderr, "accept: %s\n", strerror(err));
    /* ENOBUFS 같은 일시적 자원 부족: 잠깐 쉬어서 헛돌지 않게 */
    struct timespec pause = {0, 50 * 1000 * 1000};
    nanosleep(&pause, NULL);
}

int server_run(uint16_t port, int quiet)
{
    if (setup_signals() < 0) {
        perror("sigaction");
        return -1;
    }
    int lfd = tcp_listen(port, LISTEN_BACKLOG);
    if (lfd < 0) { /* L1 은 로그를 찍지 않으므로 여기서 이유를 알린다 */
        fprintf(stderr, "listen :%u: %s\n", (unsigned)port, strerror(errno));
        return -1;
    }
    g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

    printf("MicroHTTP listening on 0.0.0.0:%u\n", (unsigned)port);
    fflush(stdout);

    while (!g_stop) {
        char peer[64];
        int fd = tcp_accept(lfd, peer, sizeof peer);
        if (fd < 0) {
            if (errno != EINTR)
                on_accept_error(lfd);
            continue;
        }
        /* 한 번에 하나씩 처리하므로, 한 연결이 오래 붙잡지 못하게 conn_serve 가 마감 시각을 건다 */
        conn_serve(fd, peer, quiet);
    }

    close(lfd);
    if (g_reserve_fd >= 0)
        close(g_reserve_fd);
    printf("shutdown\n");
    return 0;
}
