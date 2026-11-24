/* server.c — L6: 서버 수명 관리 (시그널, accept 루프, 스레드 풀에 연결 분배, 우아한 종료)
 *
 * 레이어 구조에서의 위치 (맨 위)
 *   main.c → server_run → [메인 스레드] tcp_accept(L1) → tp_submit(L6)
 *                         [워커 스레드] conn_serve → L1 받기 → L2 파싱 → L5 라우팅 → L4/L5 핸들러 → L3 응답
 *
 * 구성
 *   - 시그널: g_stop, on_stop, setup_signals
 *   - accept 오류 대처: g_reserve_fd, on_accept_error
 *   - 스레드 풀 연결: job_ctx, serve_job
 *   - 과부하 거절: RESP_503, reject_busy
 *   - 진입점: server_run
 */
#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "conn.h"
#include "tcp.h"
#include "threadpool.h"

#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128
#define QUEUE_PER_WORKER 16 /* 대기열 크기 = 워커 수 x 16 */

static volatile sig_atomic_t g_stop = 0;

/* SIGINT/SIGTERM 핸들러: 종료 플래그만 세운다 */
static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1; /* 핸들러에서는 플래그만 세운다 (printf 등은 시그널 핸들러에서 안전하지 않음) */
}

/* SIGINT/SIGTERM 에 on_stop 을 걸고, SIGPIPE 는 무시하도록 설정한다 */
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

/* accept 실패 처리: fd 가 바닥났으면 예비 fd 로 연결 하나를 받아 버리고, 그 밖에는 잠깐 쉰다 */
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

/* 워커가 부르는 작업: 연결 하나를 끝까지 처리 */
typedef struct {
    const server_config *cfg;
    int quiet;
} job_ctx;

/* 스레드 풀의 작업 함수: job_ctx 에서 설정을 꺼내 conn_serve 로 연결 하나를 처리한다 */
static void serve_job(int fd, const char *peer, void *arg)
{
    const job_ctx *ctx = arg;
    conn_serve(fd, peer, ctx->cfg, ctx->quiet);
}

/* 대기열이 가득 찼을 때 accept 스레드가 직접 보내는 응답. 미리 만들어 둬서 거의 비용이 없다 */
static const char RESP_503[] = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: 20\r\n"
                               "Retry-After: 1\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "server is too busy\r\n";

/* 대기열이 가득 찼을 때 미리 만든 503 응답을 보내고 연결을 닫는다 */
static void reject_busy(int fd, const char *peer)
{
    /* accept 스레드가 오래 붙잡히지 않도록 아주 짧은 마감. 요청은 읽지 않고 바로 닫는다 */
    tcp_send_all(fd, RESP_503, sizeof RESP_503 - 1, tcp_now_ms() + 100);
    close(fd);
    fprintf(stderr, "[%s] queue full -> 503\n", peer);
}

/* 시그널 설정 → 리스닝 소켓 → 워커 시작 → accept 루프(연결을 풀에 넘김) → 우아한 종료 */
int server_run(const server_config *cfg, uint16_t port, size_t n_workers, int quiet)
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

    /* SIGINT/SIGTERM 은 accept 하는 메인 스레드만 받아야 accept() 가 EINTR 로 깨어난다.
     * 새 스레드는 만든 스레드의 시그널 마스크를 물려받으므로, 막은 상태에서 워커를 만들고 다시 푼다. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    job_ctx ctx = {.cfg = cfg, .quiet = quiet};
    threadpool *tp = tp_create(n_workers, n_workers * QUEUE_PER_WORKER, serve_job, &ctx);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (!tp) {
        fprintf(stderr, "thread pool: cannot start %zu workers\n", n_workers);
        close(lfd);
        return -1;
    }

    printf("MicroHTTP listening on 0.0.0.0:%u (root %s, cgi %s, %zu workers)\n", (unsigned)port, cfg->doc_root,
           cfg->cgi_dir ? cfg->cgi_dir : "off", n_workers);
    fflush(stdout);

    while (!g_stop) {
        char peer[64];
        int fd = tcp_accept(lfd, peer, sizeof peer);
        if (fd < 0) {
            if (errno != EINTR)
                on_accept_error(lfd);
            continue;
        }
        if (tp_submit(tp, fd, peer) < 0) /* 워커가 다 바쁘고 대기열도 가득 참 */
            reject_busy(fd, peer);
    }

    /* 우아한 종료: 새 연결은 그만 받고, 대기열과 처리 중인 연결을 마친 뒤 끝낸다 */
    close(lfd);
    printf("shutting down: finishing queued connections\n");
    fflush(stdout);
    tp_shutdown(tp);
    if (g_reserve_fd >= 0)
        close(g_reserve_fd);
    printf("shutdown\n");
    return 0;
}
