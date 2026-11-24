/* server.h — 서버 수명 관리: 시그널, 리스닝 소켓, accept 루프.
 * L6: accept 한 연결을 스레드 풀에 넘기고, 대기열이 차면 503 으로 거절한다. */
#ifndef MICROHTTP_SERVER_H
#define MICROHTTP_SERVER_H

#include "handler.h"

#include <stdint.h>

/* Ctrl+C / SIGTERM 을 받을 때까지 실행. 정상 종료 0, 시작 실패 -1 */
#include <stddef.h>

/* n_workers 개의 워커 스레드로 연결을 동시에 처리한다 */
int server_run(const server_config *cfg, uint16_t port, size_t n_workers, int quiet);

#endif
