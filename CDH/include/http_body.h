/* http_body.h — 요청 본문(body) 수신의 "목차"(선언부).
 * 구현은 src/http_body.c. 파서(http_parser)가 뽑아 둔 Content-Length 를 보고
 * 소켓에서 본문을 마저 받아 req->body / req->body_len 을 채운다. */

#ifndef HTTP_BODY_H /* 인클루드 가드: 아직 포함된 적 없을 때만 아래를 읽음 */
#define HTTP_BODY_H /* "이미 포함됨" 표시 */

#include "http_parser.h" /* http_request (파싱 결과 구조체) */
#include <stddef.h>      /* size_t */

#define HTTP_MAX_BODY_SIZE (1024 * 1024) /* 본문 최대 크기 1MB. 넘으면 413 (메모리 폭탄 방지) */

/* 헤더 파싱이 끝난 요청의 본문을 받는다.
 * fd         : 클라이언트 연결 소켓
 * req        : http_parse_request 가 성공한 결과. body, body_len 을 채움
 * buf, cap   : 헤더를 받은 버퍼와 그 크기. 본문이 남은 자리에 들어가면 여기에 이어서 받음
 * received   : tcp_recv_request 가 받은 총 바이트 수 (헤더 + 본문 일부)
 * header_len : 빈 줄까지 포함한 헤더 길이
 * timeout_ms : 본문 수신 전체의 마감 시간
 * heap_out   : 본문이 버퍼에 안 들어가 malloc 했으면 그 포인터, 아니면 NULL.
 *              호출자가 응답을 보낸 뒤 free(*heap_out) 해야 함 (NULL 이어도 free 는 안전)
 * 반환: 0 = 성공
 *      -1 = 수신 실패(타임아웃·끊김, errno 참고) → 응답 없이 연결을 닫으면 됨
 *      그 외 = 보낼 HTTP 상태 코드 (411 길이 필요 / 413 본문 너무 큼 / 417 Expect 미지원 / 500 메모리 부족) */
int http_read_body(int fd, http_request *req, char *buf, size_t cap,
                   size_t received, size_t header_len, int timeout_ms, char **heap_out);

#endif /* HTTP_BODY_H — 인클루드 가드 끝 */
