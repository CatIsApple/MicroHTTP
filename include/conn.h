/* conn.h — 연결 하나를 처음부터 끝까지 처리한다 (받기 → 응답 → 닫기).
 * 호출 뒤 fd 는 닫혀 있다. */
#ifndef MICROHTTP_CONN_H
#define MICROHTTP_CONN_H

#include "handler.h"

void conn_serve(int fd, const char *peer, const server_config *cfg, int quiet);

#endif
