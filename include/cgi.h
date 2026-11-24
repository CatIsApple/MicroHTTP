/* cgi.h — L5: CGI/1.1 (RFC 3875) 동적 처리
 *
 * /cgi-bin/<스크립트>[/추가 경로][?쿼리] 요청이 오면 cfg->cgi_dir/<스크립트> 를 실행하고,
 * 그 표준 출력("헤더\n\n본문")을 HTTP 응답으로 바꾼다.
 *   - 요청 정보는 환경 변수로 (REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH ...)
 *   - POST 본문은 스크립트의 표준 입력으로
 *   - 출력 헤더 중 Status, Content-Type, Location 등은 응답 헤더로
 * 오류: 스크립트 없음 404, 실행 권한 없음/폴더 밖 403, 실행 실패·잘못된 출력 502, 시간 초과 504
 */
#ifndef MICROHTTP_CGI_H
#define MICROHTTP_CGI_H

#include "handler.h"

void cgi_handle(const server_config *cfg, const http_request *req, http_response *res);

#endif
