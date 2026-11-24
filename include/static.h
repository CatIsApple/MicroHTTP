/* static.h — L4: 정적 파일 서빙 */
#ifndef MICROHTTP_STATIC_H
#define MICROHTTP_STATIC_H

#include "handler.h"

/* cfg->doc_root 아래에서 req->path 파일을 찾아 res 를 채운다.
 * GET/HEAD 만 (그 외 405), 루트 밖 403, 없음 404, 권한 없음 403, 그 밖 500.
 * 디렉터리면 index.html, 끝에 '/' 가 없는 디렉터리 주소는 '/' 를 붙인 곳으로 301. */
void static_handle(const server_config *cfg, const http_request *req, http_response *res);

/* resolved 가 root 자신이거나 root 아래에 있는가 (둘 다 realpath 결과) */
int path_is_under(const char *root, const char *resolved);

#endif
