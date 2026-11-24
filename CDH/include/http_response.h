/* http_response.h — HTTP 응답 만들기·보내기의 "목차"(선언부). 구현은 src/http_response.c.
 *
 * 두 층으로 나뉜다:
 *   1. 순수 함수 (http_reason, http_format_date, http_format_head): 문자열만 만든다. 단위 테스트가 쉽다.
 *   2. 송신 함수 (http_send_response, http_send_error): 위 함수로 머리를 만들고 소켓으로 보낸다.
 * 정적 파일·동적 처리·프록시 단계는 http_response 구조체만 채워서 http_send_response 에 넘기면 된다. */

#ifndef HTTP_RESPONSE_H /* 인클루드 가드: 아직 포함된 적 없을 때만 아래를 읽음 */
#define HTTP_RESPONSE_H /* "이미 포함됨" 표시 */

#include <stddef.h>    /* size_t */
#include <sys/types.h> /* off_t (파일 본문 시작 위치) */
#include <time.h>      /* time_t (Date 헤더용 시각) */

#define HTTP_SERVER_NAME "microhttp" /* Server 헤더 값 */
#define HTTP_HEAD_MAX 4096           /* 응답 머리 버퍼 크기. URI 상한(2048)을 담는 Location 헤더도 들어가게 넉넉히 */

/* 응답 한 개의 내용. 문자열·본문·fd 는 호출자가 소유 (이 모듈은 복사도 free/close 도 안 함).
 * 0 으로 초기화하면(= {0} 또는 지정 초기화) "메모리 본문, 추가 헤더 없음" 이 기본값이 된다. */
typedef struct {
    int status;                /* 상태 코드 (200~599. 1xx 는 이 API 로 보내지 않음) */
    const char *content_type;  /* Content-Type 값. 예: "text/html; charset=utf-8". NULL 이면 헤더 생략 */
    size_t body_len;           /* 본문 길이 → Content-Length (204 / 304 는 본문·Content-Length 를 보내지 않음) */
    int keep_alive;            /* 1 이면 "Connection: keep-alive", 0 이면 "Connection: close" */
    const char *extra_headers; /* 추가 헤더들. "이름: 값\r\n" 줄의 연속 (예: "Allow: GET, POST\r\n"). 없으면 NULL */

    /* 본문 출처: use_file 이 0 이면 메모리(body), 1 이면 파일(file_fd 의 file_offset 부터 body_len 바이트) */
    int use_file;              /* 0 = 메모리 본문 (기본), 1 = 파일 본문 (정적 파일용, sendfile 사용) */
    const char *body;          /* 메모리 본문 시작 (body_len 이 0 이면 NULL 가능) */
    int file_fd;               /* 파일 본문의 fd (읽기 전용으로 연 일반 파일) */
    off_t file_offset;         /* 파일 본문 시작 위치 (보통 0. Range 요청이면 그 시작) */
} http_response;

/* 상태 코드 → 이유 문구. 예: 404 → "Not Found". 모르는 코드면 "Unknown" */
const char *http_reason(int status); /* 반환 문자열은 정적 상수라 free 하면 안 됨 */

/* 이 상태 코드의 응답이 본문(과 Content-Length)을 가질 수 있는지. 204 / 304 는 0 (RFC 7230 3.3) */
int http_status_has_body(int status);

/* 시각 t 를 HTTP 날짜 형식(IMF-fixdate, 항상 GMT)으로 쓴다. 예: "Sun, 06 Nov 1994 08:49:37 GMT"
 * (RFC 7231 7.1.1.1). 반환: 쓴 바이트 수('\0' 제외), out 이 모자라거나 변환 실패면 -1 */
int http_format_date(char *out, size_t cap, time_t t);

/* 응답 머리를 out 에 쓴다:
 *   HTTP/1.1 <status> <reason>\r\n
 *   Date: <now>\r\n
 *   Server: microhttp\r\n
 *   Content-Type: <content_type>\r\n     ← content_type 이 NULL 이면 생략
 *   Content-Length: <body_len>\r\n       ← 204 / 304 는 생략
 *   Connection: keep-alive | close\r\n
 *   <extra_headers>
 *   \r\n
 * 반환: 쓴 바이트 수('\0' 제외).
 * -1: out 이 모자람 / status 가 200~599 밖 / content_type 에 제어 문자(CR·LF 등) /
 *     extra_headers 가 "이름: 값\r\n" 줄들이 아님 (빈 줄·단독 LF·제어 문자 → 응답 분할 방지) */
int http_format_head(char *out, size_t cap, const http_response *res, time_t now);

/* 머리 + 본문을 마감 시간(timeout_ms) 안에 보낸다.
 * 메모리 본문: sendmsg 한 번 (복사 없음). 파일 본문: 머리 전송 후 sendfile (zero-copy, 리눅스).
 * 204 / 304 는 본문을 보내지 않는다.
 * 성공 0, 실패 -1 (errno: 머리 만들기 실패면 EINVAL, 송신 실패면 EPIPE / ETIMEDOUT / EIO 등) */
int http_send_response(int fd, const http_response *res, int timeout_ms);

/* 오류 응답: 본문 "<코드> <이유>\n" (text/plain). 오류 뒤에는 요청 스트림 경계를 믿을 수 없으므로
 * 항상 "Connection: close". extra_headers 는 http_response 와 같은 규칙 (405 의 "Allow: ..." 등), 없으면 NULL.
 * 반환은 http_send_response 와 같음 */
int http_send_error(int fd, int status, const char *extra_headers, int timeout_ms);

#endif /* HTTP_RESPONSE_H — 인클루드 가드 끝 */
