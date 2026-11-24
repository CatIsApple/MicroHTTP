/* test_threadpool.c — L6 스레드 풀 단위 테스트 (fd 대신 숫자를 넘겨 작업 실행만 확인)
 *
 * 구성: job 이 받은 숫자를 sum 에 더하고 ran 을 센다. gate 를 닫으면 워커를 job 안에 붙잡아 둘 수 있어서
 * "워커가 모두 바쁘고 대기열이 가득 찬" 상태를 일부러 만들 수 있다. 공유 변수는 모두 m 으로 보호한다. */
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

/* 테스트용 작업: 문이 열릴 때까지 기다렸다가 peer 가 "peer-<n>" 인지 확인하고 sum, ran 에 기록한다 */
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

/* 문을 닫거나(0: 워커가 job 안에서 멈춤) 열고(1), 기다리는 워커를 모두 깨운다 */
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

/* 워커 수나 대기열 크기가 0 이면 tp_create 가 NULL */
static void bad_args(void)
{
    EXPECT(tp_create(0, 4, job, NULL) == NULL);
    EXPECT(tp_create(4, 0, job, NULL) == NULL);
}

/* 모든 테스트를 실행하고, 실패가 있으면 개수를 찍고 1 을 돌려준다 */
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
