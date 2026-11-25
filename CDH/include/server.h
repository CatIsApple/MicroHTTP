/* server.h — 연결 하나를 처리하는 HTTP 서버 본체의 "목차"(선언부). 구현은 src/server.c.
 *
 *   accept(main) ──tp_submit──▶ 워커: serve_connection ──(keep-alive 반복: 요청 → 응답)──┐
 *                                  ▲                                                     │ 남은 바이트 없음 + 쉬는 중
 *                                  └──tp_submit── server_on_idle_ready ◀── idle poller ◀─idle_park
 *
 * 쉬는 keep-alive 연결은 워커를 붙잡지 않고 idle poller 로 넘긴다 → 워커 수보다 훨씬 많은 연결을 유지할 수 있다. */

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
 * 실패하면(큐 가득 / 종료 중) 연결을 닫는다 (quiet 가 아니면 로그). ctx 는 server_ctx * */
void server_on_idle_ready(const conn_job *job, void *ctx);

#endif /* SERVER_H */
