/* http_response.h — HTTP 응답 머리(상태 줄 + 헤더) 만들기의 "목차"(선언부).
 * 구현은 src/http_response.c. 소켓과 무관한 순수 함수 (문자열만 만든다). */

#ifndef HTTP_RESPONSE_H /* 인클루드 가드: 아직 포함된 적 없을 때만 아래를 읽음 */
#define HTTP_RESPONSE_H /* "이미 포함됨" 표시 */

#include <stddef.h> /* size_t */

/* 상태 코드 → 이유 문구. 예: 404 → "Not Found". 모르는 코드면 "Unknown" */
const char *http_reason(int status); /* 반환 문자열은 정적 상수라 free 하면 안 됨 */

/* 응답 머리를 out 에 쓴다:
 *   HTTP/1.1 <status> <reason>\r\n
 *   Content-Type: <content_type>\r\n
 *   Content-Length: <content_length>\r\n
 *   Connection: keep-alive | close\r\n
 *   \r\n
 * 반환: 쓴 바이트 수('\0' 제외), out 이 모자라면 -1 */
int http_format_head(char *out, size_t cap, int status, const char *content_type,
                     size_t content_length, int keep_alive); /* 본문은 호출자가 이어서 보냄 */

#endif /* HTTP_RESPONSE_H — 인클루드 가드 끝 */
