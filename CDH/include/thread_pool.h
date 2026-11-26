/* thread_pool.h — 고정 크기 워커 스레드 풀 + 상한 있는 작업 큐의 "목차"(선언부). 구현은 src/thread_pool.c.
 *
 *   accept 스레드 ──tp_submit──▶ [고리 버퍼 큐] ──▶ 워커 N개 ──fn(job, ctx)
 *
 * - 큐가 가득 차면 tp_submit 은 기다리지 않고 바로 실패한다 (accept 스레드가 막히면 안 되므로).
 * - fn 은 job->fd 의 소유권을 받는다: 반드시 close 하거나 다른 곳(idle poller 등)에 넘겨야 한다.
 * - 워커는 SIGINT/SIGTERM 을 막은 채(pthread_sigmask) 만들어진다 → 종료 시그널은 메인 스레드만 받는다.
 * - 종료 알림: tp_wake_fd() 는 tp_shutdown 이 시작되면 영원히 "읽기 가능" 이 되는 pipe 의 읽는 쪽.
 *   워커가 poll 로 기다릴 때 함께 넣어 두면 종료 시 즉시 깨어난다. */

#ifndef THREAD_POOL_H /* 인클루드 가드 */
#define THREAD_POOL_H

#include "conn_job.h" /* conn_job */
#include <stddef.h>   /* size_t */

typedef struct thread_pool thread_pool; /* 내부 구조는 thread_pool.c 에만 (불투명 타입) */

/* 워커가 작업 하나마다 호출하는 함수. job 은 워커 스택의 복사본 (함수가 끝나면 사라짐) */
typedef void (*tp_job_fn)(conn_job *job, void *ctx);

/* 풀 생성. nthreads >= 1, qcap >= 1. stack_size 가 0 이면 기본값(256KB), 아니면 그 크기로
 * (PTHREAD_STACK_MIN 보다 작으면 올림). 실패 시 NULL (errno). 만든 스레드는 실패 시 모두 정리한다. */
thread_pool *tp_create(int nthreads, size_t qcap, size_t stack_size, tp_job_fn fn, void *ctx);

/* 작업을 큐에 넣는다 (job 을 복사). 기다리지 않는다.
 * 반환: 0 성공 (fd 소유권이 풀로 넘어감)
 *      -1 실패, fd 소유권은 호출자에게 그대로 — errno = EAGAIN (큐 가득) / ECANCELED (종료 중) */
int tp_submit(thread_pool *tp, const conn_job *job);

size_t tp_queued(thread_pool *tp);   /* 지금 큐에 대기 중인 작업 수 (부하 판단용, 근사값) */
size_t tp_capacity(thread_pool *tp); /* 큐 용량 (qcap) */
int tp_stopping(thread_pool *tp);    /* tp_shutdown 이 시작됐으면 1 (어느 스레드에서 불러도 안전) */
int tp_wake_fd(thread_pool *tp);     /* 종료 알림 pipe 의 읽는 쪽 fd (읽지 말고 poll 에만 넣을 것) */

/* 종료: stopping = 1 → 종료 알림 pipe 에 1바이트 → 대기 중 워커 모두 깨움 → 처리 중인 작업이 끝날 때까지 join
 * → 큐에 남은 작업의 fd 는 close. 메모리는 아직 해제하지 않는다 → 다른 스레드(idle poller)가 이후에
 * tp_submit 을 불러도 안전하게 -1/ECANCELED 를 받는다. 한 번만 부른다. */
void tp_shutdown(thread_pool *tp);

/* tp_shutdown 뒤, 더 이상 아무 스레드도 tp 를 쓰지 않을 때 메모리·pipe 를 해제한다.
 * 종료 순서: tp_shutdown(pool) → idle_stop(poller) → tp_destroy(pool) */
void tp_destroy(thread_pool *tp);

#endif /* THREAD_POOL_H */
