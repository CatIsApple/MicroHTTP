/* http_response.h — HTTP 응답 만들기·보내기의 "목차"(선언부). 구현은 src/http_response.c.
 *
 * 두 층으로 나뉜다:
 *   1. 순수 함수 (http_reason, http_format_date, http_format_head): 문자열만 만든다. 단위 테스트가 쉽다.
 *   2. 송신 함수 (http_send_response, http_send_error): 위 함수로 머리를 만들고 소켓으로 보낸다.
 * 정적 파일·동적 처리·프록시 단계는 http_response 구조체만 채워서 http_send_response 에 넘기면 된다. */

#ifndef HTTP_RESPONSE_H /* 인클루드 가드: 아직 포함된 적 없을 때만 아래를 읽음 */
#define HTTP_RESPONSE_H /* "이미 포함됨" 표시 */

#include <stddef.h> /* size_t */
#include <time.h>   /* time_t (Date 헤더용 시각) */

#define HTTP_SERVER_NAME "microhttp" /* Server 헤더 값 */
#define HTTP_HEAD_MAX 1024           /* 응답 머리 버퍼 크기. 내용이 이보다 크면 http_format_head 가 -1 */

/* 응답 한 개의 내용. 문자열·본문은 호출자가 소유 (이 모듈은 복사도 free 도 안 함) */
typedef struct {
    int status;                /* 상태 코드 (100~599) */
    const char *content_type;  /* Content-Type 값. 예: "text/html; charset=utf-8" */
    const char *body;          /* 본문 시작 (body_len 이 0 이면 NULL 가능) */
    size_t body_len;           /* 본문 길이 → Content-Length 로 나감 */
    int keep_alive;            /* 1 이면 "Connection: keep-alive", 0 이면 "Connection: close" */
    const char *extra_headers; /* 추가 헤더들. 각 줄이 "\r\n" 으로 끝나야 함 (예: "Allow: GET, POST\r\n"). 없으면 NULL */
} http_response;

/* 상태 코드 → 이유 문구. 예: 404 → "Not Found". 모르는 코드면 "Unknown" */
const char *http_reason(int status); /* 반환 문자열은 정적 상수라 free 하면 안 됨 */

/* 시각 t 를 HTTP 날짜 형식(IMF-fixdate, 항상 GMT)으로 쓴다. 예: "Sun, 06 Nov 1994 08:49:37 GMT"
 * (RFC 7231 7.1.1.1). 반환: 쓴 바이트 수('\0' 제외), out 이 모자라거나 변환 실패면 -1 */
int http_format_date(char *out, size_t cap, time_t t);

/* 응답 머리를 out 에 쓴다:
 *   HTTP/1.1 <status> <reason>\r\n
 *   Date: <now>\r\n
 *   Server: microhttp\r\n
 *   Content-Type: <content_type>\r\n
 *   Content-Length: <body_len>\r\n
 *   Connection: keep-alive | close\r\n
 *   <extra_headers>
 *   \r\n
 * 반환: 쓴 바이트 수('\0' 제외).
 * -1: out 이 모자람 / status 가 100~599 밖 / content_type 에 CR·LF 포함 (헤더 주입 방지) /
 *     extra_headers 가 비어 있지 않은데 "\r\n" 으로 끝나지 않음 */
int http_format_head(char *out, size_t cap, const http_response *res, time_t now);

/* 머리 + 본문을 마감 시간(timeout_ms) 안에 한 번에 보낸다 (본문은 복사하지 않음).
 * 성공 0, 실패 -1 (errno: 머리 만들기 실패면 EINVAL, 송신 실패면 EPIPE / ETIMEDOUT 등) */
int http_send_response(int fd, const http_response *res, int timeout_ms);

/* 오류 응답: 본문 "<코드> <이유>\n" (text/plain). 오류 뒤에는 요청 스트림 경계를 믿을 수 없으므로
 * 항상 "Connection: close". extra_headers 는 http_response 와 같은 규칙 (405 의 "Allow: ..." 등), 없으면 NULL.
 * 반환은 http_send_response 와 같음 */
int http_send_error(int fd, int status, const char *extra_headers, int timeout_ms);

#endif /* HTTP_RESPONSE_H — 인클루드 가드 끝 */
