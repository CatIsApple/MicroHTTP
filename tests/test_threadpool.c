/* test_threadpool.c — L6 스레드 풀 단위 테스트 (fd 대신 숫자를 넘겨 작업 실행만 확인) */
#define _POSIX_C_SOURCE 200809L

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define EXPECT(c)                                                        \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open = 1;
static long sum = 0;
static int ran = 0;
static int peers_ok = 1;

static void job(int n, const char *peer, void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    while (!gate_open) /* 테스트가 문을 열 때까지 워커를 붙잡아 둔다 */
        pthread_cond_wait(&gate_cv, &m);
    char want[32];
    snprintf(want, sizeof want, "peer-%d", n);
    if (strcmp(peer, want) != 0)
        peers_ok = 0;
    sum += n;
    ran++;
    pthread_mutex_unlock(&m);
}

static void set_gate(int open)
{
    pthread_mutex_lock(&m);
    gate_open = open;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&m);
}

/* 많은 작업이 빠짐없이 한 번씩 실행되고, shutdown 이 남은 작업까지 처리한다 */
static void runs_every_job(void)
{
    sum = 0, ran = 0, peers_ok = 1;
    threadpool *tp = tp_create(4, 64, job, NULL);
    EXPECT(tp != NULL);
    long want = 0;
    for (int i = 1; i <= 1000; i++) {
        char peer[32];
        snprintf(peer, sizeof peer, "peer-%d", i);
        while (tp_submit(tp, i, peer) < 0) { /* 가득 차면 잠깐 쉬었다 다시 */
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
        }
        want += i;
    }
    tp_shutdown(tp);
    EXPECT(ran == 1000);
    EXPECT(sum == want);
    EXPECT(peers_ok);
}

/* 워커가 모두 바쁘고 대기열이 차면 tp_submit 이 -1 */
static void full_queue_rejects(void)
{
    sum = 0, ran = 0;
    set_gate(0);
    threadpool *tp = tp_create(2, 3, job, NULL);
    EXPECT(tp != NULL);
    int accepted = 0;
    for (int i = 1; i <= 10; i++) {
        char peer[32];
        snprintf(peer, sizeof peer, "peer-%d", i);
        if (tp_submit(tp, i, peer) == 0)
            accepted++;
        struct timespec ts = {0, 2000000}; /* 워커가 작업을 꺼내 갈 시간 */
        nanosleep(&ts, NULL);
    }
    EXPECT(accepted == 5); /* 워커 2개가 하나씩 들고 대기 + 대기열 3칸 */
    set_gate(1);
    tp_shutdown(tp);
    EXPECT(ran == accepted);
}

static void bad_args(void)
{
    EXPECT(tp_create(0, 4, job, NULL) == NULL);
    EXPECT(tp_create(4, 0, job, NULL) == NULL);
}

int main(void)
{
    runs_every_job();
    full_queue_rejects();
    bad_args();
    if (failures) {
        fprintf(stderr, "test_threadpool: %d failed\n", failures);
        return 1;
    }
    printf("test_threadpool: ok\n");
    return 0;
}
