/* idle_poller.h — keep-alive 로 "쉬는" 연결을 모아 감시하는 스레드 하나의 "목차"(선언부). 구현은 src/idle_poller.c.
 *
 *   워커: 응답 끝 + keep-alive + 남은 바이트 없음 ──idle_park──▶ [감시 스레드: epoll(리눅스) / poll(그 밖)]
 *        읽기 가능(다음 요청 도착, 또는 상대가 닫음/오류) ──▶ ready_fn(job, ctx)  (보통 tp_submit 으로 워커에게 다시)
 *        idle_timeout_ms 동안 아무것도 안 옴 ──▶ close
 *
 * 워커가 쉬는 연결 때문에 붙잡히지 않게 하는 것이 목적 (연결 1만 개가 쉬어도 감시 스레드 1개 + fd 1만 개).
 * fd 소유권: idle_park 성공 → poller 가 가짐. ready_fn 호출 → ready_fn 이 가짐 (close 하거나 넘길 것). */

#ifndef IDLE_POLLER_H /* 인클루드 가드 */
#define IDLE_POLLER_H

#include "conn_job.h" /* conn_job */
#include <stddef.h>   /* size_t */

typedef struct idle_poller idle_poller; /* 불투명 타입 */

/* 감시 중인 연결이 읽기 가능해졌을 때(또는 HUP/ERR) 감시 스레드에서 호출된다.
 * 이 시점에 fd 는 이미 감시 목록에서 빠져 있고 소유권은 ready_fn 에 있다. 오래 걸리는 일을 하면 안 된다. */
typedef void (*idle_ready_fn)(const conn_job *job, void *ctx);

/* 감시 스레드 생성. idle_timeout_ms > 0: 쉬는 연결을 닫기까지의 시간. max_conns: 동시에 감시할 최대 연결 수.
 * 감시 스레드는 SIGINT/SIGTERM 을 막은 채 만들어진다. 실패 시 NULL (errno) */
idle_poller *idle_create(int idle_timeout_ms, size_t max_conns, idle_ready_fn fn, void *ctx);

/* 연결을 감시 목록에 넣는다 (job 복사). 어느 스레드에서 불러도 안전. 기다리지 않는다.
 * 반환: 0 성공 (fd 소유권이 poller 로)
 *      -1 실패, fd 소유권은 호출자에게 그대로 — errno = EAGAIN (max_conns 가득) / ECANCELED (종료 중) / 그 밖 */
int idle_park(idle_poller *ip, const conn_job *job);

size_t idle_count(idle_poller *ip); /* 지금 감시 중인 연결 수 (근사값) */

/* 종료: 감시 스레드를 멈추고 join → 감시 중이던 모든 fd close → free. 이후 idle_park 는 호출하면 안 됨.
 * (종료와 동시에 다른 스레드가 idle_park 를 부르는 경우는 -1/ECANCELED 로 거절되어야 하고 fd 가 새지 않아야 한다
 *  — 단, idle_stop 이 free 까지 끝낸 뒤의 호출은 호출자 책임) */
void idle_stop(idle_poller *ip);

#endif /* IDLE_POLLER_H */
