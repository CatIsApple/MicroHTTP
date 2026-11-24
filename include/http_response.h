/* http_response.h — L3: HTTP 응답 생성 레이어
 *
 * 상태 코드 + 헤더 + 본문을 구조체에 모았다가, http_response_send() 가
 * "HTTP/1.1 200 OK\r\n헤더...\r\n\r\n본문" 형태로 직렬화해서 L1(tcp_send_all) 로 보낸다.
 *
 * 본문은 세 종류
 *   BODY_NONE : 없음
 *   BODY_MEM  : 메모리 (문자열, CGI 출력)
 *   BODY_FILE : 열린 파일 (정적 파일). 보낸 뒤 http_response_free() 가 닫는다.
 *
 * Content-Length, Connection, Date, Server 헤더는 send 가 자동으로 붙인다.
 * 핸들러가 깜빡해서 잘못된 응답이 나가는 일이 없게 하기 위해서다.
 */
#ifndef MICROHTTP_HTTP_RESPONSE_H
#define MICROHTTP_HTTP_RESPONSE_H

#include "http_request.h" /* http_header 타입만 씀 */

#include <stddef.h>
#include <sys/types.h>

#define HTTP_RESPONSE_MAX_HEADERS 16

typedef enum { BODY_NONE, BODY_MEM, BODY_FILE } body_kind;

typedef struct {
    int         status;
    http_header headers[HTTP_RESPONSE_MAX_HEADERS]; /* 핸들러가 추가한 헤더 */
    size_t      n_headers;
    body_kind   kind;
    const void *mem;      /* BODY_MEM: 본문 */
    size_t      mem_len;
    void       *owned;    /* free 해야 하는 메모리 (http_response_body_owned 로 넘긴 것) */
    int         fd;       /* BODY_FILE: 보낼 파일 */
    off_t       file_len;
    int         keep_alive;   /* 응답 후 연결 유지 (Connection 헤더 값을 정함) */
    char        scratch[512]; /* http_response_header_copy 가 값을 복사해 두는 곳 */
    size_t      scratch_used;
    char        error_page[256]; /* http_response_error 가 만든 HTML */
} http_response;

/* 상태 코드만 정한 빈 응답. keep_alive = 1, 본문 없음 */
void http_response_init(http_response *res, int status);

/* 헤더 추가. 값은 send 할 때까지 살아 있어야 한다 (문자열 상수 등).
 * 헤더가 가득 찼거나 이름/값에 CR, LF 가 있으면 -1 (응답 분할 공격 방지) */
int http_response_header(http_response *res, const char *name, const char *value);

/* 값을 res 안에 복사해 두는 버전 (지역 변수로 만든 문자열을 넣을 때) */
int http_response_header_copy(http_response *res, const char *name, const char *value);

/* 본문 지정. ctype 이 NULL 이 아니면 Content-Type 헤더도 붙인다 */
void http_response_body_mem(http_response *res, const void *p, size_t n, const char *ctype);
/* base 는 malloc 한 메모리, 본문은 그 안의 [p, p+n). free 는 http_response_free 가 한다 */
void http_response_body_owned(http_response *res, void *base, const void *p, size_t n, const char *ctype);
/* 파일 fd 의 처음부터 len 바이트. fd 는 http_response_free 가 닫는다 */
void http_response_body_file(http_response *res, int fd, off_t len, const char *ctype);

/* status 로 바꾸고 기본 HTML 오류 페이지를 본문으로 넣는다 (기존 본문은 정리) */
void http_response_error(http_response *res, int status);

/* 200 → "OK". 모르는 코드는 "Unknown" */
const char *http_status_text(int status);

/* 직렬화해서 보낸다. head_only 면 헤더만 (HEAD 요청). 성공 0, 실패 -1 (연결을 닫아야 함) */
int http_response_send(int fd, http_response *res, int head_only, long long deadline_ms);

/* 파일 fd 닫기, owned 메모리 해제. 여러 번 불러도 안전 */
void http_response_free(http_response *res);

#endif
