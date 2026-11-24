/* threadpool.c — L6: 뮤텍스 + 조건 변수로 만든 원형 대기열과 워커 스레드 */
#define _POSIX_C_SOURCE 200809L

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int fd;
    char peer[64];
} task;

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty; /* 대기열에 작업이 생겼거나 종료 요청 */
    task *queue;              /* 원형 버퍼 */
    size_t cap, head, count;
    int stopping;
    pthread_t *workers;
    size_t n_workers;
    tp_job job;
    void *arg;
};

static void *worker_main(void *p)
{
    threadpool *tp = p;
    for (;;) {
        pthread_mutex_lock(&tp->lock);
        while (tp->count == 0 && !tp->stopping) /* 가짜 깨어남(spurious wakeup) 대비 while */
            pthread_cond_wait(&tp->not_empty, &tp->lock);
        if (tp->count == 0 && tp->stopping) { /* 남은 작업을 다 비운 뒤에야 끝낸다 */
            pthread_mutex_unlock(&tp->lock);
            return NULL;
        }
        task t = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        pthread_mutex_unlock(&tp->lock);

        tp->job(t.fd, t.peer, tp->arg); /* 잠금 밖에서 실행 (오래 걸리는 일) */
    }
}

threadpool *tp_create(size_t n_workers, size_t queue_cap, tp_job job, void *arg)
{
    if (n_workers == 0 || queue_cap == 0)
        return NULL;
    threadpool *tp = calloc(1, sizeof *tp);
    if (!tp)
        return NULL;
    tp->queue = calloc(queue_cap, sizeof *tp->queue);
    tp->workers = calloc(n_workers, sizeof *tp->workers);
    if (!tp->queue || !tp->workers) {
        free(tp->queue);
        free(tp->workers);
        free(tp);
        return NULL;
    }
    tp->cap = queue_cap;
    tp->job = job;
    tp->arg = arg;
    pthread_mutex_init(&tp->lock, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    for (size_t i = 0; i < n_workers; i++) {
        if (pthread_create(&tp->workers[i], NULL, worker_main, tp) != 0) {
            tp->n_workers = i; /* 만든 것까지만 정리 */
            tp_shutdown(tp);
            return NULL;
        }
        tp->n_workers = i + 1;
    }
    return tp;
}

int tp_submit(threadpool *tp, int fd, const char *peer)
{
    pthread_mutex_lock(&tp->lock);
    if (tp->stopping || tp->count == tp->cap) {
        pthread_mutex_unlock(&tp->lock);
        return -1;
    }
    task *t = &tp->queue[(tp->head + tp->count) % tp->cap];
    t->fd = fd;
    snprintf(t->peer, sizeof t->peer, "%s", peer ? peer : "?");
    tp->count++;
    pthread_cond_signal(&tp->not_empty); /* 워커 하나를 깨운다 */
    pthread_mutex_unlock(&tp->lock);
    return 0;
}

void tp_shutdown(threadpool *tp)
{
    pthread_mutex_lock(&tp->lock);
    tp->stopping = 1;
    pthread_cond_broadcast(&tp->not_empty); /* 자고 있는 워커를 모두 깨운다 */
    pthread_mutex_unlock(&tp->lock);
    for (size_t i = 0; i < tp->n_workers; i++)
        pthread_join(tp->workers[i], NULL);
    pthread_cond_destroy(&tp->not_empty);
    pthread_mutex_destroy(&tp->lock);
    free(tp->queue);
    free(tp->workers);
    free(tp);
}
