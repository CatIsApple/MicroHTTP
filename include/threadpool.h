/* threadpool.h — L6: 고정 크기 워커 스레드 + 크기가 정해진 연결 대기열
 *
 * accept 한 연결(fd)을 대기열에 넣으면 놀고 있는 워커가 꺼내 job(fd, peer, arg) 을 부른다.
 * 대기열이 가득 차면 tp_submit 이 -1 → 호출자가 503 을 보내고 닫는다(백프레셔).
 */
#ifndef MICROHTTP_THREADPOOL_H
#define MICROHTTP_THREADPOOL_H

#include <stddef.h>

typedef void (*tp_job)(int fd, const char *peer, void *arg);

typedef struct threadpool threadpool;

/* 실패하면 NULL */
threadpool *tp_create(size_t n_workers, size_t queue_cap, tp_job job, void *arg);

/* 연결 하나를 맡긴다. 대기열이 가득 찼거나 종료 중이면 -1 (fd 는 호출자가 처리) */
int tp_submit(threadpool *tp, int fd, const char *peer);

/* 새 작업을 더 받지 않고, 대기열에 남은 연결까지 처리한 뒤 워커를 모두 join 하고 해제한다 */
void tp_shutdown(threadpool *tp);

#endif
