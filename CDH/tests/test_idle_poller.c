/* test_idle_poller.c — idle poller (keep-alive 로 쉬는 연결 감시 스레드) 단위 테스트.
 *
 * socketpair 로 연결 한 쌍을 만들어 서버 쪽 fd 를 park 하고, 클라 쪽 fd 로 "다음 요청 도착 / 끊음 / 침묵" 을 흉내 낸다.
 * ready_fn 은 감시 스레드에서 불리므로 결과를 mutex+cond 로 모으고, CHECK 는 메인 스레드에서만 한다
 * (test.h 의 카운터는 스레드 안전이 아님). */

#define _POSIX_C_SOURCE 200809L /* socketpair, nanosleep, clock_gettime, getrlimit 등 POSIX 함수 */

#include "idle_poller.h" /* 테스트 대상 */
#include "test.h"        /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <errno.h>       /* errno, EAGAIN, ECANCELED, EINVAL */
#include <fcntl.h>       /* fcntl, O_NONBLOCK, F_GETFD */
#include <poll.h>        /* poll (클라 쪽에서 닫힘 기다리기) */
#include <pthread.h>     /* 동시성 테스트용 스레드, mutex, cond */
#include <sched.h>       /* sched_yield (가득 찼을 때 재시도 전 양보) */
#include <signal.h>      /* signal, SIGPIPE */
#include <stdatomic.h>   /* atomic_int (스레드 간 플래그) */
#include <stdlib.h>      /* calloc, free */
#include <string.h>      /* memset, snprintf */
#include <sys/resource.h> /* getrlimit, setrlimit, RLIMIT_NOFILE */
#include <sys/socket.h>  /* socketpair, recv */
#include <time.h>        /* nanosleep, clock_gettime */
#include <unistd.h>      /* read, write, close */

static void sleep_ms(long ms) /* 밀리초 단위로 잠들기 */
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static long long now_ms(void) /* 경과 시간 측정용 (단조 시계) */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 연결 한 쌍: sv[0] = 서버 쪽(논블로킹, park 대상), sv[1] = 클라 쪽 (블로킹). 실패 시 -1 */
static int make_pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    int fl = fcntl(sv[0], F_GETFL);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);  /* 서버 소켓은 논블로킹 (실제 서버와 같게) */
    return 0;
}

static int fd_open(int fd) { return fcntl(fd, F_GETFD) != -1; } /* fd 가 아직 열려 있으면 1 */

/* 클라 쪽에서 서버 쪽이 닫혔는지: timeout_ms 안에 읽기 가능해지고 recv 가 0(EOF) 이면 1 */
static int peer_closed(int cfd, int timeout_ms)
{
    struct pollfd p = {.fd = cfd, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) != 1)
        return 0;
    char b;
    return recv(cfd, &b, 1, MSG_DONTWAIT) == 0;
}

static int count_open_fds(void) /* 지금 열린 fd 수 (누수 검사용) */
{
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    int lim = rl.rlim_cur > 65536 ? 65536 : (int)rl.rlim_cur;
    int n = 0;
    for (int fd = 0; fd < lim; fd++)
        n += fd_open(fd);
    return n;
}

/* ---- ready_fn 결과 수집 (감시 스레드 → 메인) ---- */

#define REC_MAX 4096
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static conn_job g_rec[REC_MAX]; /* ready_fn 이 받은 job 들 */
static int g_nrec = 0;          /* 받은 수 */

/* ready_fn 을 멈춰 세우는 모드 (대기열 테스트용): g_block 이 1 이면 g_release 될 때까지 기다림 */
static int g_block = 0;         /* g_mu 보호 */
static int g_blocked = 0;       /* g_mu 보호: ready_fn 이 멈춰 있음 */
static int g_release = 0;       /* g_mu 보호 */

static void on_ready(const conn_job *job, void *ctx) /* 감시 스레드에서 불림: fd 는 닫지 않고 기록만 (메인이 확인 후 닫음) */
{
    (void)ctx;
    pthread_mutex_lock(&g_mu);
    if (g_nrec < REC_MAX)
        g_rec[g_nrec] = *job;
    g_nrec++;
    if (g_block) {                             /* 감시 스레드를 붙잡아 둠 */
        g_blocked = 1;
        pthread_cond_broadcast(&g_cv);
        while (!g_release)
            pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void rec_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_nrec = 0;
    g_block = g_blocked = g_release = 0;
    pthread_mutex_unlock(&g_mu);
}

static int rec_wait(int want, int timeout_ms) /* ready_fn 이 want 번 불릴 때까지 기다림. 반환: 그때까지의 수 */
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);        /* cond 기본 시계는 REALTIME */
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_mu);
    while (g_nrec < want)
        if (pthread_cond_timedwait(&g_cv, &g_mu, &dl) != 0)
            break;
    int n = g_nrec;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int rec_count(void)
{
    pthread_mutex_lock(&g_mu);
    int n = g_nrec;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static conn_job mkjob(int fd, const char *peer, unsigned nreq)
{
    conn_job j;
    memset(&j, 0, sizeof(j));
    j.fd = fd;
    snprintf(j.peer, sizeof(j.peer), "%s", peer);
    j.nreq = nreq;
    return j;
}

/* ---- 동시 park 테스트 ---- */

typedef struct {
    idle_poller *ip;
    int n;          /* 이 스레드가 만들 쌍 수 */
    int *srv;       /* 서버 쪽 fd (n 개) */
    int *cli;       /* 클라 쪽 fd (n 개) */
    int ok;         /* park 성공 수 */
} park_arg;

static void *park_worker(void *arg) /* n 개를 park 한 뒤 클라 쪽에서 1바이트씩 씀 */
{
    park_arg *a = arg;
    for (int i = 0; i < a->n; i++) {
        conn_job j = mkjob(a->srv[i], "t", (unsigned)i);
        if (idle_park(a->ip, &j) == 0)
            a->ok++;
    }
    for (int i = 0; i < a->n; i++) {
        char b = 'x';
        if (write(a->cli[i], &b, 1) != 1) { /* 결과는 메인이 ready 수로 검사 */
        }
    }
    return NULL;
}

/* ---- idle_stop 과 동시에 park 하는 테스트 ---- */

#define RACE_CAP 512 /* 스레드 하나가 성공시킬 수 있는 최대 수 (max_conns 로 제한됨) */
typedef struct {
    idle_poller *ip;
    atomic_int *go;
    int cli[RACE_CAP]; /* park 성공한 연결의 클라 쪽 fd */
    int nok;           /* 성공 수 */
    int nfail_other;   /* EAGAIN/ECANCELED 외 실패 수 */
    int got_cancel;    /* ECANCELED 를 받고 끝났으면 1 */
} race_arg;

static void *race_worker(void *arg) /* ECANCELED 를 받을 때까지 park 을 계속 시도 */
{
    race_arg *a = arg;
    while (!atomic_load(a->go))
        ;
    int sv[2];
    if (make_pair(sv) < 0)
        return NULL;
    for (;;) {
        conn_job j = mkjob(sv[0], "r", 0);
        if (idle_park(a->ip, &j) == 0) {        /* 성공: 서버 쪽 소유권은 poller → 새 쌍 */
            if (a->nok < RACE_CAP)
                a->cli[a->nok++] = sv[1];
            else
                close(sv[1]);                   /* 일어나지 않음 (max_conns 로 막힘) */
            if (make_pair(sv) < 0)
                return NULL;
        } else if (errno == EAGAIN) {           /* 가득: 같은 쌍으로 재시도 */
            sched_yield();
        } else {
            if (errno == ECANCELED)
                a->got_cancel = 1;
            else
                a->nfail_other++;
            close(sv[0]);                       /* 실패 → 소유권은 우리 → 우리가 닫음 */
            close(sv[1]);
            return NULL;
        }
    }
}

/* idle_stop 을 다른 스레드에서 부르기 위한 래퍼 */
static void *stop_worker(void *arg)
{
    idle_stop(arg);
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* 닫힌 소켓에 써도 죽지 않게 */
    int base_fds = count_open_fds();
    int sv[2];
    char b;

    /* ==== 잘못된 인자 → NULL / EINVAL ==== */
    {
        errno = 0;
        CHECK(idle_create(0, 10, on_ready, NULL) == NULL);
        CHECK_INT(errno, EINVAL);
        errno = 0;
        CHECK(idle_create(-5, 10, on_ready, NULL) == NULL);
        CHECK_INT(errno, EINVAL);
        errno = 0;
        CHECK(idle_create(100, 0, on_ready, NULL) == NULL);
        CHECK_INT(errno, EINVAL);
        errno = 0;
        CHECK(idle_create(100, 10, NULL, NULL) == NULL);
        CHECK_INT(errno, EINVAL);

        idle_poller *ip = idle_create(1000, 10, on_ready, NULL);
        CHECK(ip != NULL);
        conn_job bad = mkjob(-1, "x", 0);
        errno = 0;
        CHECK_INT(idle_park(ip, &bad), -1);    /* fd 음수 */
        CHECK_INT(errno, EINVAL);
        errno = 0;
        CHECK_INT(idle_park(ip, NULL), -1);
        CHECK_INT(errno, EINVAL);
        CHECK_INT(idle_count(ip), 0);
        idle_stop(ip);
    }

    /* ==== park → 클라가 1바이트 → ready_fn 1번, 바이트는 소켓에 그대로 ==== */
    {
        rec_reset();
        idle_poller *ip = idle_create(5000, 16, on_ready, NULL);
        CHECK(ip != NULL);
        CHECK_INT(make_pair(sv), 0);
        conn_job j = mkjob(sv[0], "1.2.3.4:5678", 7);
        CHECK_INT(idle_park(ip, &j), 0);
        sleep_ms(50);                          /* 감시 스레드가 등록할 시간 */
        CHECK_INT(idle_count(ip), 1);
        CHECK_INT(rec_count(), 0);             /* 아직 아무것도 안 옴 */
        b = 'G';
        CHECK_INT(write(sv[1], &b, 1), 1);     /* 다음 요청의 첫 바이트 */
        CHECK_INT(rec_wait(1, 2000), 1);
        sleep_ms(50);                          /* 중복 호출이 있다면 드러나게 */
        CHECK_INT(rec_count(), 1);             /* 정확히 1번 */
        CHECK_INT(g_rec[0].fd, sv[0]);
        CHECK_STR(g_rec[0].peer, "1.2.3.4:5678");
        CHECK_INT(g_rec[0].nreq, 7);
        CHECK_INT(idle_count(ip), 0);          /* 목록에서 빠짐 */
        char got = 0;
        CHECK_INT(recv(sv[0], &got, 1, 0), 1); /* poller 가 바이트를 먹지 않았음 */
        CHECK_INT(got, 'G');
        close(sv[0]);                          /* 소유권은 ready_fn(=테스트) */
        close(sv[1]);
        idle_stop(ip);
    }

    /* ==== 클라가 닫음 (FIN) → ready_fn 호출 (HUP 도 준비로 취급) ==== */
    {
        rec_reset();
        idle_poller *ip = idle_create(5000, 16, on_ready, NULL);
        CHECK_INT(make_pair(sv), 0);
        conn_job j = mkjob(sv[0], "hup", 1);
        CHECK_INT(idle_park(ip, &j), 0);
        sleep_ms(30);
        close(sv[1]);
        CHECK_INT(rec_wait(1, 2000), 1);
        CHECK_INT(g_rec[0].fd, sv[0]);
        CHECK_INT(recv(sv[0], &b, 1, 0), 0);   /* EOF 가 그대로 보임 */
        CHECK_INT(idle_count(ip), 0);
        close(sv[0]);
        idle_stop(ip);
    }

    /* ==== 타임아웃: 침묵 → ~200ms 뒤 서버 쪽이 닫힘, ready_fn 은 안 불림 ==== */
    {
        rec_reset();
        idle_poller *ip = idle_create(200, 16, on_ready, NULL);
        CHECK_INT(make_pair(sv), 0);
        conn_job j = mkjob(sv[0], "to", 1);
        long long t0 = now_ms();
        CHECK_INT(idle_park(ip, &j), 0);
        CHECK(peer_closed(sv[1], 2000));       /* 클라 쪽 recv == 0 */
        long long dt = now_ms() - t0;
        CHECK(dt >= 150);                      /* 너무 이르지 않게 */
        CHECK(dt < 1000);                      /* 너무 늦지 않게 */
        CHECK_INT(rec_count(), 0);
        CHECK_INT(idle_count(ip), 0);
        close(sv[1]);
        idle_stop(ip);
    }

    /* ==== 먼저 park 한 것이 먼저 만료 ==== */
    {
        rec_reset();
        idle_poller *ip = idle_create(300, 16, on_ready, NULL);
        int a[2], c[2];
        CHECK_INT(make_pair(a), 0);
        CHECK_INT(make_pair(c), 0);
        conn_job ja = mkjob(a[0], "A", 1), jc = mkjob(c[0], "C", 1);
        long long t0 = now_ms();
        CHECK_INT(idle_park(ip, &ja), 0);
        sleep_ms(150);
        CHECK_INT(idle_park(ip, &jc), 0);
        CHECK(peer_closed(a[1], 2000));        /* A 먼저 닫힘 */
        long long ta = now_ms() - t0;
        struct pollfd p = {.fd = c[1], .events = POLLIN};
        CHECK_INT(poll(&p, 1, 0), 0);          /* 그 순간 C 는 아직 열려 있음 */
        CHECK(peer_closed(c[1], 2000));        /* 그다음 C */
        long long tc = now_ms() - t0;
        CHECK(ta >= 250 && ta < 1000);
        CHECK(tc - ta >= 100);                 /* park 간격(150ms) 만큼 뒤 */
        CHECK_INT(rec_count(), 0);
        close(a[1]);
        close(c[1]);
        idle_stop(ip);
    }

    /* ==== max_conns 가득 → -1/EAGAIN, fd 는 열린 채 ==== */
    {
        rec_reset();
        idle_poller *ip = idle_create(5000, 2, on_ready, NULL);
        int p1[2], p2[2], p3[2];
        CHECK_INT(make_pair(p1), 0);
        CHECK_INT(make_pair(p2), 0);
        CHECK_INT(make_pair(p3), 0);
        conn_job j1 = mkjob(p1[0], "1", 0), j2 = mkjob(p2[0], "2", 0), j3 = mkjob(p3[0], "3", 0);
        CHECK_INT(idle_park(ip, &j1), 0);
        CHECK_INT(idle_park(ip, &j2), 0);
        errno = 0;
        CHECK_INT(idle_park(ip, &j3), -1);     /* 대기열에 있어도 개수에 포함 */
        CHECK_INT(errno, EAGAIN);
        CHECK(fd_open(p3[0]));                 /* 실패 시 닫지 않음 */
        CHECK_INT(idle_count(ip), 2);

        b = 'x';                               /* 하나가 ready 로 빠지면 자리가 생김 */
        CHECK_INT(write(p1[1], &b, 1), 1);
        CHECK_INT(rec_wait(1, 2000), 1);
        close(p1[0]);
        CHECK_INT(idle_park(ip, &j3), 0);
        CHECK_INT(idle_count(ip), 2);

        idle_stop(ip);                         /* p2, p3 서버 쪽은 poller 가 닫음 */
        CHECK(peer_closed(p2[1], 1000));
        CHECK(peer_closed(p3[1], 1000));
        close(p1[1]);
        close(p2[1]);
        close(p3[1]);
    }

    /* ==== idle_stop: 감시 중인 것 + 대기열에만 있는 것 모두 닫힘 ==== */
    {
        rec_reset();
        pthread_mutex_lock(&g_mu);
        g_block = 1;                           /* ready_fn 에서 감시 스레드를 붙잡음 */
        pthread_mutex_unlock(&g_mu);
        idle_poller *ip = idle_create(5000, 64, on_ready, NULL);
        enum { NW = 5, NP = 4 };
        int w[NW][2], q[NP][2], k[2];
        for (int i = 0; i < NW; i++) {         /* 감시 리스트에 들어갈 것들 */
            CHECK_INT(make_pair(w[i]), 0);
            conn_job j = mkjob(w[i][0], "w", 0);
            CHECK_INT(idle_park(ip, &j), 0);
        }
        CHECK_INT(make_pair(k), 0);            /* 감시 스레드를 멈출 "열쇠" 연결 */
        conn_job jk = mkjob(k[0], "k", 0);
        CHECK_INT(idle_park(ip, &jk), 0);
        sleep_ms(50);
        CHECK_INT(idle_count(ip), NW + 1);
        b = 'k';
        CHECK_INT(write(k[1], &b, 1), 1);
        pthread_mutex_lock(&g_mu);             /* ready_fn 이 멈출 때까지 기다림 */
        while (!g_blocked)
            pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        for (int i = 0; i < NP; i++) {         /* 감시 스레드가 멈춰 있으니 이들은 대기열에만 있음 */
            CHECK_INT(make_pair(q[i]), 0);
            conn_job j = mkjob(q[i][0], "q", 0);
            CHECK_INT(idle_park(ip, &j), 0);
        }
        CHECK_INT(idle_count(ip), NW + NP);    /* 대기열도 개수에 포함 */

        pthread_t st;                          /* idle_stop 은 join 에서 막히므로 다른 스레드에서 */
        CHECK_INT(pthread_create(&st, NULL, stop_worker, ip), 0);
        sleep_ms(50);                          /* stopping 이 켜질 시간 */
        pthread_mutex_lock(&g_mu);
        g_release = 1;                         /* 감시 스레드 풀어 줌 → 루프 종료 */
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        pthread_join(st, NULL);

        for (int i = 0; i < NW; i++) {
            CHECK(peer_closed(w[i][1], 1000));
            close(w[i][1]);
        }
        for (int i = 0; i < NP; i++) {
            CHECK(peer_closed(q[i][1], 1000));
            close(q[i][1]);
        }
        close(k[0]);                           /* ready_fn 으로 받은 것 (소유권은 테스트) */
        close(k[1]);
        rec_reset();
    }

    /* ==== 동시성: 스레드 4개 × 250 park → 클라 전부 1바이트 → ready_fn 정확히 1000번, 중복 없음 ==== */
    {
        rec_reset();
        enum { NT = 4 };
        int per = 250;
        struct rlimit rl;                      /* fd 2000 개 + 여유가 필요 */
        getrlimit(RLIMIT_NOFILE, &rl);
        rlim_t need = (rlim_t)(NT * per * 2 + 64);
        if (rl.rlim_cur < need) {
            rlim_t want = need;
            if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
                want = rl.rlim_max;
            rl.rlim_cur = want;
            setrlimit(RLIMIT_NOFILE, &rl);
            getrlimit(RLIMIT_NOFILE, &rl);
            if (rl.rlim_cur < need) {          /* 못 올리면 개수를 줄임 */
                per = (int)((rl.rlim_cur - 64) / (NT * 2));
                printf("note: RLIMIT_NOFILE=%llu → %d per thread\n", (unsigned long long)rl.rlim_cur, per);
            }
        }
        int total = NT * per;
        idle_poller *ip = idle_create(10000, (size_t)total, on_ready, NULL);
        CHECK(ip != NULL);
        park_arg args[NT];
        pthread_t th[NT];
        int pair_ok = 1;
        for (int t = 0; t < NT; t++) {
            args[t] = (park_arg){.ip = ip, .n = per, .srv = calloc((size_t)per, sizeof(int)),
                                 .cli = calloc((size_t)per, sizeof(int)), .ok = 0};
            for (int i = 0; i < per; i++) {
                int s[2];
                if (make_pair(s) < 0)
                    pair_ok = 0;
                args[t].srv[i] = s[0];
                args[t].cli[i] = s[1];
            }
        }
        CHECK(pair_ok);
        for (int t = 0; t < NT; t++)
            pthread_create(&th[t], NULL, park_worker, &args[t]);
        for (int t = 0; t < NT; t++)
            pthread_join(th[t], NULL);
        int ok = 0;
        for (int t = 0; t < NT; t++)
            ok += args[t].ok;
        CHECK_INT(ok, total);
        CHECK_INT(rec_wait(total, 10000), total);
        sleep_ms(50);
        CHECK_INT(rec_count(), total);         /* 더 늘지 않음 (중복 없음) */
        CHECK_INT(idle_count(ip), 0);

        int maxfd = 0;                         /* 각 서버 fd 가 정확히 한 번씩 나왔는지 */
        for (int t = 0; t < NT; t++)
            for (int i = 0; i < per; i++)
                if (args[t].srv[i] > maxfd)
                    maxfd = args[t].srv[i];
        unsigned char *seen = calloc((size_t)maxfd + 1, 1);
        int dup = 0, unknown = 0;
        int nrec = rec_count() < REC_MAX ? rec_count() : REC_MAX;
        for (int i = 0; i < nrec; i++) {
            int fd = g_rec[i].fd;              /* 감시 스레드는 이미 쓰기를 마침 (mutex 로 동기화됨) */
            if (fd < 0 || fd > maxfd) {
                unknown++;
                continue;
            }
            if (seen[fd]++)
                dup++;
        }
        int missing = 0;
        for (int t = 0; t < NT; t++)
            for (int i = 0; i < per; i++)
                if (!seen[args[t].srv[i]])
                    missing++;
        CHECK_INT(dup, 0);
        CHECK_INT(unknown, 0);
        CHECK_INT(missing, 0);
        free(seen);
        idle_stop(ip);
        for (int t = 0; t < NT; t++) {
            for (int i = 0; i < per; i++) {
                close(args[t].srv[i]);         /* ready_fn 으로 받았으니 테스트가 닫음 */
                close(args[t].cli[i]);
            }
            free(args[t].srv);
            free(args[t].cli);
        }
    }

    /* ==== idle_stop 과 동시에 park 시도 → 모든 fd 가 정확히 한 번 닫힘, 누수 없음 ====
     * idle_stop 이 free 까지 끝낸 뒤의 idle_park 는 호출자 책임(헤더) 이므로, 감시 스레드를 ready_fn 에 붙잡아
     * idle_stop 을 join 단계에 세워 둔다 → stopping 이 켜지는 순간과 park 은 경쟁하지만,
     * 모든 parker 가 ECANCELED 를 받고 끝난 뒤에야 free 가 일어나게 한다. */
    for (int round = 0; round < 3; round++) {
        rec_reset();
        enum { NT = 4 };
        idle_poller *ip = idle_create(10000, 300, on_ready, NULL); /* 상한 300 → fd 수 제한 + EAGAIN 경로도 섞임 */
        pthread_mutex_lock(&g_mu);
        g_block = 1;
        pthread_mutex_unlock(&g_mu);
        int k[2];                              /* 감시 스레드를 붙잡을 "열쇠" 연결 (parker 보다 먼저 park) */
        CHECK_INT(make_pair(k), 0);
        conn_job jk = mkjob(k[0], "k", 0);
        CHECK_INT(idle_park(ip, &jk), 0);
        atomic_int go;
        atomic_init(&go, 0);
        race_arg *ra = calloc(NT, sizeof(*ra));
        pthread_t th[NT];
        for (int t = 0; t < NT; t++) {
            ra[t].ip = ip;
            ra[t].go = &go;
            pthread_create(&th[t], NULL, race_worker, &ra[t]);
        }
        atomic_store(&go, 1);
        sleep_ms(5 + round * 10);              /* 감시 스레드가 일부를 감시 리스트에 등록하도록 */
        b = 'k';
        CHECK_INT(write(k[1], &b, 1), 1);      /* 감시 스레드를 ready_fn 에 붙잡음 → 이후 park 는 대기열에 쌓임 */
        pthread_mutex_lock(&g_mu);
        while (!g_blocked)
            pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        sleep_ms(round * 5);                   /* 대기열에도 쌓이게 */
        pthread_t st;
        CHECK_INT(pthread_create(&st, NULL, stop_worker, ip), 0); /* parker 들이 도는 중에 idle_stop 시작 */
        int nok = 0, cancel = 0, other = 0, closed = 0;
        for (int t = 0; t < NT; t++)           /* parker 는 ECANCELED 를 받아야 끝남 */
            pthread_join(th[t], NULL);
        pthread_mutex_lock(&g_mu);             /* 이제 감시 스레드를 풀어 idle_stop 을 끝까지 진행 */
        g_release = 1;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        pthread_join(st, NULL);
        close(k[0]);                           /* ready_fn 으로 받은 열쇠 (소유권은 테스트) */
        close(k[1]);
        for (int t = 0; t < NT; t++) {
            nok += ra[t].nok;
            cancel += ra[t].got_cancel;
            other += ra[t].nfail_other;
            for (int i = 0; i < ra[t].nok; i++) {
                closed += peer_closed(ra[t].cli[i], 1000); /* poller 가 서버 쪽을 닫았음 */
                close(ra[t].cli[i]);
            }
        }
        CHECK_INT(cancel, NT);                 /* 모두 ECANCELED 로 끝남 */
        CHECK_INT(other, 0);
        CHECK_INT(closed, nok);
        CHECK_INT(rec_count(), 1);             /* 열쇠 하나만 ready (parker 쪽은 아무도 안 씀) */
        CHECK(nok > 0);                        /* 실제로 park 이 일어났음 */
        free(ra);
        rec_reset();
    }

    /* ==== 전체 누수 검사: 시작할 때와 열린 fd 수가 같아야 함 ==== */
    CHECK_INT(count_open_fds(), base_fds);

    return TEST_REPORT();
}
