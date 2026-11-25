/* server.h — 연결 하나를 처리하는 HTTP 서버 본체의 "목차"(선언부). 구현은 src/server.c.
 *
 *   accept(main) ── server_dispatch_new ──(데이터 있음: tp_submit)──▶ 워커: serve_connection ──(keep-alive 반복)──┐
 *                          │ 아직 데이터 없음                          ▲                                       │ 남은 바이트 없음
 *                          └──idle_park──▶ idle poller ──readable──▶ server_on_idle_ready ◀──idle_park─────────┘
 *
 * 아직 아무것도 안 보낸 새 연결과 쉬는 keep-alive 연결은 워커를 붙잡지 않고 idle poller 가 기다린다
 * → 워커 수보다 훨씬 많은 연결을 유지할 수 있고, 빈 연결 몇 개로 워커가 바닥나지 않는다. */

#ifndef SERVER_H /* 인클루드 가드 */
#define SERVER_H

#include "conn_job.h"    /* conn_job */
#include "idle_poller.h" /* idle_poller */
#include "thread_pool.h" /* thread_pool */

#define SERVER_KEEPALIVE_MAX 100 /* 한 연결에서 처리할 최대 요청 수 기본값 (넘으면 Connection: close) */

/* 모든 워커와 poller 가 함께 읽는 설정 (시작 후에는 바뀌지 않으므로 잠금 없이 읽는다) */
typedef struct {
    int root_fd;            /* 문서 루트 fd (static_lookup 의 openat 기준) */
    int quiet;              /* -q: 요청마다 찍는 로그 끔 */
    unsigned keepalive_max; /* 한 연결의 최대 요청 수 (기본 SERVER_KEEPALIVE_MAX) */
    thread_pool *pool;      /* 워커 풀 (부하 판단·poller 에서 되돌려 보낼 곳) */
    idle_poller *idle;      /* 쉬는 연결을 맡길 곳 */
} server_ctx;

/* tp_job_fn: 워커에서 호출. job->fd 의 소유권을 받는다 → 끝날 때 close 하거나 idle poller 에 넘긴다.
 * 요청 → 응답을 keep-alive 가 허락하는 동안 반복한다. ctx 는 server_ctx * */
void serve_connection(conn_job *job, void *ctx);

/* idle_ready_fn: 쉬던 연결이 읽기 가능해지면 poller 스레드에서 호출. 워커 풀에 다시 넣고,
 * 실패하면(큐 가득 / 종료 중) server_reject_busy 로 503 후 닫는다 (quiet 가 아니면 로그). ctx 는 server_ctx * */
void server_on_idle_ready(const conn_job *job, void *ctx);

/* accept 스레드에서 새 연결마다 호출 (job->fd 소유권을 받음, 기다리지 않음).
 * 요청 바이트가 이미 와 있으면 워커 풀로, 아직 없으면 idle poller 로 보낸다 (빈 연결이 워커를 붙잡지 않게).
 * 둘 다 가득이면 503. ctx 는 server_ctx * */
void server_dispatch_new(const conn_job *job, void *ctx);

/* 바빠서 못 받는 연결을 닫는다: 이미 와 있는 수신 데이터를 비우고(RST 방지) 503 을 보낸 뒤 close.
 * 상대가 아무것도 안 보내고 닫았으면 503 없이 close. 논블로킹 — 어느 스레드에서든 기다리지 않는다 */
void server_reject_busy(int fd);

#endif /* SERVER_H */
