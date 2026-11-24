/* router.h — L5: 요청을 어느 핸들러로 보낼지 정한다 */
#ifndef MICROHTTP_ROUTER_H
#define MICROHTTP_ROUTER_H

#include "handler.h"

/* 경로 앞부분이 맞는 첫 핸들러를 부른다. 맞는 것이 없으면 정적 파일(L4) */
void router_dispatch(const server_config *cfg, const http_request *req, http_response *res);

#endif
