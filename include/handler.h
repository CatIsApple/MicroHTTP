/* handler.h — L4/L5 공통: 서버 설정과 핸들러 시그니처
 *
 * 핸들러는 요청(L2)을 읽고 응답(L3)을 채운다. 소켓은 모른다.
 * 실패하더라도 반드시 res 를 채워야 한다 (http_response_error).
 */
#ifndef MICROHTTP_HANDLER_H
#define MICROHTTP_HANDLER_H

#include "http_request.h"
#include "http_response.h"

typedef struct {
    const char *doc_root;   /* 정적 파일 루트. realpath 로 정규화된 절대 경로 */
    const char *cgi_prefix; /* 이 경로로 시작하면 CGI 로 보냄, 예: "/cgi-bin/" (NULL 이면 CGI 끔) */
    const char *cgi_dir;    /* CGI 스크립트 폴더. realpath 로 정규화된 절대 경로 */
} server_config;

typedef void (*http_handler)(const server_config *cfg, const http_request *req, http_response *res);

#endif
