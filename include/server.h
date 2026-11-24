/* server.h — 서버 수명 관리: 시그널, 리스닝 소켓, accept 루프.
 * 지금은 연결을 하나씩 순서대로 처리한다(반복 서버). 동시성은 L6 에서 여기에 들어온다. */
#ifndef MICROHTTP_SERVER_H
#define MICROHTTP_SERVER_H

#include "handler.h"

#include <stdint.h>

/* Ctrl+C / SIGTERM 을 받을 때까지 실행. 정상 종료 0, 시작 실패 -1 */
int server_run(const server_config *cfg, uint16_t port, int quiet);

#endif
