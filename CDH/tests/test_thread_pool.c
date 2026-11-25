/* test_thread_pool.c — 스레드 풀 단위 테스트.
 * 주의: test.h 의 CHECK 카운터는 스레드 안전하지 않다 → CHECK 는 메인 스레드에서만 부른다.
 * 워커(fn) 쪽에서는 mutex 로 보호한 변수에 기록만 하고, 메인이 모아서 검사한다. */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 함수를 헤더에서 보이게 함. include 보다 먼저 와야 함 */

#include "thread_pool.h"
#include "test.h"

#include <errno.h>   /* errno, EAGAIN, EINVAL, ECANCELED, EBADF */
#include <fcntl.h>   /* open, fcntl, F_GETFD, O_RDONLY */
#include <poll.h>    /* poll, struct pollfd */
#include <pthread.h> /* mutex, cond, 스레드 */
#include <sched.h>   /* sched_yield */
#include <signal.h>  /* sigset_t, sigismember, pthread_sigmask */
#include <stdlib.h>  /* calloc, free */
#include <time.h>    /* clock_gettime, nanosleep */
#include <unistd.h>  /* close */

static void sleep_ms(long ms) /* ms 밀리초 잠들기 (usleep 은 POSIX 2008 에서 빠져서 nanosleep 사용) */
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { /* 시그널로 끊기면 남은 시간만큼 다시 */
    }
}

static double now_sec(void) /* 단조 시계 (초) */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int fd_is_open(int fd) /* fd 가 열려 있으면 1 (fcntl 이 EBADF 가 아님) */
{
    return fcntl(fd, F_GETFD) != -1;
}

static int fd_is_closed(int fd) /* fd 가 닫혀 있으면 1 (fcntl 이 EBADF) */
{
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

static int wake_readable(thread_pool *tp) /* wake fd 가 지금 읽기 가능한지 (poll 0ms) */
{
    struct pollfd p = {tp_wake_fd(tp), POLLIN, 0};
    int n = poll(&p, 1, 0);
    return n == 1 && (p.revents & POLLIN);
}

/* ---------- 테스트 1: 모든 작업이 정확히 한 번씩, fd 소유권 이전 ---------- */

#define N_JOBS 1000

typedef struct {
    pthread_mutex_t mu;      /* 아래 필드 보호 */
    pthread_cond_t done_cv;  /* 전부 처리됨 신호 */
    int seen[N_JOBS];        /* id 별 실행 횟수 */
    int total;               /* 총 실행 횟수 */
    int bad_id;              /* 범위 밖 id 가 온 횟수 */
    int close_fail;          /* fn 안의 close 가 실패한 횟수 (fd 가 유효하지 않았다는 뜻) */
} once_ctx;

static void once_fn(conn_job *job, void *ctx)
{
    once_ctx *c = ctx;
    int rc = close(job->fd); /* 소유권이 넘어왔으니 fn 이 닫는다 */
    pthread_mutex_lock(&c->mu);
    if (rc != 0) c->close_fail++;
    if (job->nreq < N_JOBS) c->seen[job->nreq]++;
    else c->bad_id++;
    c->total++;
    if (c->total == N_JOBS) pthread_cond_signal(&c->done_cv);
    pthread_mutex_unlock(&c->mu);
}

static void test_exactly_once(void)
{
    once_ctx *c = calloc(1, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->done_cv, NULL);

    thread_pool *tp = tp_create(4, 64, 0, once_fn, c);
    CHECK(tp != NULL);
    if (tp == NULL) return;
    CHECK_INT(tp_capacity(tp), 64);
    CHECK_INT(tp_stopping(tp), 0);

    int submit_fail = 0;
    for (unsigned i = 0; i < N_JOBS; i++) {
        conn_job j = {0};
        j.fd = open("/dev/null", O_RDONLY | O_CLOEXEC); /* 실제 fd 를 만들어 넘김 */
        j.nreq = i;
        if (j.fd < 0) { submit_fail++; continue; }
        while (tp_submit(tp, &j) != 0) { /* 큐가 가득이면 잠깐 양보하고 재시도 */
            if (errno != EAGAIN) { submit_fail++; close(j.fd); break; }
            sched_yield();
        }
    }
    CHECK_INT(submit_fail, 0);

    pthread_mutex_lock(&c->mu); /* 전부 끝날 때까지 대기 */
    while (c->total < N_JOBS) pthread_cond_wait(&c->done_cv, &c->mu);
    pthread_mutex_unlock(&c->mu);

    tp_shutdown(tp); /* join 후에는 워커 기록을 락 없이 읽어도 안전 */
    int wrong = 0;
    for (int i = 0; i < N_JOBS; i++) if (c->seen[i] != 1) wrong++;
    CHECK_INT(wrong, 0);
    CHECK_INT(c->total, N_JOBS);
    CHECK_INT(c->bad_id, 0);
    CHECK_INT(c->close_fail, 0);
    CHECK_INT(tp_queued(tp), 0);
    tp_destroy(tp);
    pthread_cond_destroy(&c->done_cv);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

/* ---------- 테스트 2·3: 큐 가득 / shutdown 의 남은 fd 정리 / wake fd / 종료 후 submit ---------- */

#define FULL_CAP 3

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started;        /* 막힌 작업이 시작됐는지 */
    int release;        /* 1 이 되면 막힌 작업을 풀어 줌 */
    int blocked_runs;   /* 막는 fn(id 0)이 실행된 횟수 */
    int other_runs;     /* id 0 이 아닌 작업이 실행된 횟수 (있으면 안 됨) */
} gate_ctx;

static void gate_fn(conn_job *job, void *ctx)
{
    gate_ctx *g = ctx;
    pthread_mutex_lock(&g->mu);
    if (job->nreq == 0) { /* 첫 작업만 막는다 */
        g->blocked_runs++;
        g->started = 1;
        pthread_cond_broadcast(&g->cv);
        while (!g->release) pthread_cond_wait(&g->cv, &g->mu);
    } else {
        g->other_runs++;
    }
    pthread_mutex_unlock(&g->mu);
    close(job->fd);
}

static void *shutdown_thread(void *arg) /* tp_shutdown 은 막힌 워커 때문에 join 에서 기다리므로 별도 스레드에서 */
{
    tp_shutdown(arg);
    return NULL;
}

static int open_null(void) { return open("/dev/null", O_RDONLY | O_CLOEXEC); }

static void test_full_and_shutdown(void)
{
    gate_ctx g = {0};
    pthread_mutex_init(&g.mu, NULL);
    pthread_cond_init(&g.cv, NULL);

    thread_pool *tp = tp_create(1, FULL_CAP, 0, gate_fn, &g); /* 워커 1개 → 막으면 큐가 찬다 */
    CHECK(tp != NULL);
    if (tp == NULL) return;

    CHECK_INT(wake_readable(tp), 0); /* shutdown 전엔 읽기 가능이 아님 */
    CHECK(tp_wake_fd(tp) >= 0);

    conn_job j = {0};
    j.fd = open_null();
    j.nreq = 0;
    CHECK_INT(tp_submit(tp, &j), 0); /* id 0: 워커를 막을 작업 */

    pthread_mutex_lock(&g.mu); /* 워커가 그 작업을 집어 fn 안에서 막힐 때까지 대기 */
    while (!g.started) pthread_cond_wait(&g.cv, &g.mu);
    pthread_mutex_unlock(&g.mu);

    int queued_fd[FULL_CAP];
    for (int i = 0; i < FULL_CAP; i++) { /* 큐를 가득 채움 (워커가 막혀 있으니 안 빠져나감) */
        j.fd = open_null();
        j.nreq = (unsigned)(i + 1);
        queued_fd[i] = j.fd;
        CHECK_INT(tp_submit(tp, &j), 0);
    }
    CHECK_INT(tp_queued(tp), FULL_CAP);

    conn_job rej = {0}; /* 가득 찬 뒤의 제출은 실패해야 함 */
    rej.fd = open_null();
    rej.nreq = 99;
    errno = 0;
    CHECK_INT(tp_submit(tp, &rej), -1);
    CHECK_INT(errno, EAGAIN);
    CHECK(fd_is_open(rej.fd)); /* 실패했으니 fd 는 여전히 호출자 소유 */
    CHECK_INT(tp_queued(tp), FULL_CAP);
    CHECK_INT(wake_readable(tp), 0);

    pthread_t th; /* 별도 스레드에서 shutdown (워커가 풀릴 때까지 join 에서 대기) */
    CHECK_INT(pthread_create(&th, NULL, shutdown_thread, tp), 0);
    double t0 = now_sec();
    while (!tp_stopping(tp) && now_sec() - t0 < 5.0) sleep_ms(1); /* stopping 이 켜질 때까지 */
    CHECK_INT(tp_stopping(tp), 1);

    pthread_mutex_lock(&g.mu); /* 이제 막힌 워커를 풀어 줌 → 현재 작업만 끝내고 큐는 건드리지 않고 종료해야 함 */
    g.release = 1;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    CHECK_INT(pthread_join(th, NULL), 0);

    /* 이 구간에서는 다른 fd 를 열지 않는다 (닫힌 번호가 재사용되면 EBADF 검사가 틀어짐) */
    for (int i = 0; i < FULL_CAP; i++) CHECK(fd_is_closed(queued_fd[i])); /* 큐에 남았던 fd 는 shutdown 이 close */
    CHECK_INT(tp_queued(tp), 0);
    CHECK_INT(g.blocked_runs, 1);
    CHECK_INT(g.other_runs, 0); /* 남은 큐 작업은 처리되지 않음 */

    errno = 0;
    CHECK_INT(tp_submit(tp, &rej), -1); /* shutdown 뒤 제출 */
    CHECK_INT(errno, ECANCELED);
    CHECK(fd_is_open(rej.fd)); /* 소유권은 호출자 그대로 */
    CHECK_INT(tp_stopping(tp), 1);
    CHECK_INT(wake_readable(tp), 1); /* shutdown 뒤에는 읽기 가능 */
    CHECK_INT(wake_readable(tp), 1); /* 읽지 않으니 계속 readable */

    close(rej.fd);
    tp_destroy(tp);
    pthread_cond_destroy(&g.cv);
    pthread_mutex_destroy(&g.mu);
}

/* ---------- 테스트 4: idle 워커는 shutdown 에 즉시 깨어난다 ---------- */

static void noop_fn(conn_job *job, void *ctx)
{
    (void)ctx;
    close(job->fd);
}

static void test_idle_shutdown_fast(void)
{
    thread_pool *tp = tp_create(4, 8, 0, noop_fn, NULL);
    CHECK(tp != NULL);
    if (tp == NULL) return;
    sleep_ms(100); /* 워커들이 빈 큐에서 cond_wait 에 들어갈 시간 */
    double t0 = now_sec();
    tp_shutdown(tp);
    double dt = now_sec() - t0;
    CHECK(dt < 1.0); /* 깨우는 데 오래 걸리면(타임아웃 의존) 실패 */
    CHECK_INT(tp_stopping(tp), 1);
    CHECK_INT(wake_readable(tp), 1);
    tp_destroy(tp);
}

/* ---------- 테스트 5: 잘못된 인자 ---------- */

static void test_invalid_args(void)
{
    errno = 0;
    CHECK(tp_create(0, 8, 0, noop_fn, NULL) == NULL);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK(tp_create(-1, 8, 0, noop_fn, NULL) == NULL);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK(tp_create(2, 0, 0, noop_fn, NULL) == NULL);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK(tp_create(2, 8, 0, NULL, NULL) == NULL);
    CHECK_INT(errno, EINVAL);
}

/* ---------- 테스트 6: 워커는 SIGINT/SIGTERM 이 막혀 있고, 호출 스레드의 마스크는 그대로 ---------- */

#define SIG_JOBS 8

typedef struct {
    pthread_mutex_t mu;
    int runs;           /* 실행된 작업 수 */
    int int_blocked;    /* 그중 SIGINT 가 막혀 있던 수 */
    int term_blocked;   /* 그중 SIGTERM 이 막혀 있던 수 */
} sig_ctx;

static void sig_fn(conn_job *job, void *ctx)
{
    sig_ctx *s = ctx;
    sigset_t cur;
    pthread_sigmask(SIG_BLOCK, NULL, &cur); /* how 가 SIG_BLOCK 이어도 set 이 NULL 이면 현재 마스크만 읽음 */
    int bi = sigismember(&cur, SIGINT) == 1;
    int bt = sigismember(&cur, SIGTERM) == 1;
    close(job->fd);
    pthread_mutex_lock(&s->mu);
    s->runs++;
    s->int_blocked += bi;
    s->term_blocked += bt;
    pthread_mutex_unlock(&s->mu);
}

static void test_worker_sigmask(void)
{
    sig_ctx s = {0};
    pthread_mutex_init(&s.mu, NULL);

    sigset_t before, after;
    pthread_sigmask(SIG_BLOCK, NULL, &before);
    thread_pool *tp = tp_create(3, 16, 0, sig_fn, &s);
    pthread_sigmask(SIG_BLOCK, NULL, &after);
    CHECK(tp != NULL);
    if (tp == NULL) return;
    CHECK_INT(sigismember(&after, SIGINT), sigismember(&before, SIGINT)); /* 메인 마스크 복원 */
    CHECK_INT(sigismember(&after, SIGTERM), sigismember(&before, SIGTERM));
    CHECK_INT(sigismember(&after, SIGINT), 0);

    for (unsigned i = 0; i < SIG_JOBS; i++) {
        conn_job j = {0};
        j.fd = open_null();
        j.nreq = i;
        CHECK_INT(tp_submit(tp, &j), 0);
    }
    for (int i = 0; i < 500; i++) { /* 모두 처리될 때까지 최대 약 5초 */
        pthread_mutex_lock(&s.mu);
        int r = s.runs;
        pthread_mutex_unlock(&s.mu);
        if (r >= SIG_JOBS) break;
        sleep_ms(10);
    }
    tp_shutdown(tp);
    CHECK_INT(s.runs, SIG_JOBS);
    CHECK_INT(s.int_blocked, SIG_JOBS);
    CHECK_INT(s.term_blocked, SIG_JOBS);
    tp_destroy(tp);
    pthread_mutex_destroy(&s.mu);
}

int main(void)
{
    test_exactly_once();
    test_full_and_shutdown();
    test_idle_shutdown_fast();
    test_invalid_args();
    test_worker_sigmask();

    return TEST_REPORT();
}
