/* http_request.h — L2: HTTP 요청 파싱 레이어
 *
 * 바이트 → 구조체. 소켓을 모르는 순수 함수라서(tcp.h 를 include 하지 않음)
 * 문자열만 넣어서 테스트할 수 있다.
 *
 * 파싱하는 것
 *   - 요청 라인: 메서드(GET, POST, HEAD), 요청 대상(URI), 버전(HTTP/1.0, HTTP/1.1)
 *   - 헤더: Host, Content-Length, Connection 은 의미까지 검사, 나머지는 이름/값만 저장
 *   - POST 본문: Content-Length 만큼
 *
 * 사용법 (같은 버퍼로 여러 번 불러도 되는 "재진입" 파서)
 *   받은 바이트를 버퍼 뒤에 덧붙이고 http_parse() 를 부른다.
 *     HTTP_PARSE_INCOMPLETE : 아직 덜 왔다. 더 받아서 다시 부른다. (버퍼는 건드리지 않음)
 *     HTTP_PARSE_OK         : 요청 하나 완성. *consumed 바이트가 이 요청이다.
 *     그 밖                 : 잘못된 요청. http_parse_status() 의 상태 코드로 응답하고 닫는다.
 *
 * 메모리: malloc 을 하지 않는다. OK 일 때 버퍼 안의 구분자 자리에 '\0' 을 써서 문자열을 만들고,
 * 구조체의 포인터들은 그 버퍼를 가리킨다. 그래서 버퍼가 살아 있는 동안만 유효하다.
 */
#ifndef MICROHTTP_HTTP_REQUEST_H
#define MICROHTTP_HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_MAX_HEADER_BYTES 8192 /* 요청 라인 + 헤더 + 빈 줄 최대 크기 */
#define HTTP_MAX_HEADERS 32        /* 헤더 최대 개수 */

typedef enum { HTTP_GET, HTTP_POST, HTTP_HEAD } http_method;

typedef struct {
    const char *name;  /* 보낸 그대로의 대소문자 */
    const char *value; /* 앞뒤 공백 제거됨 */
} http_header;

typedef struct {
    http_method method;
    const char *method_str;     /* "GET" */
    const char *target;         /* 받은 그대로의 요청 대상 "/a%20b?x=1" */
    const char *path;           /* 퍼센트 디코드한 경로 "/a b" */
    const char *query;          /* '?' 뒤 "x=1" (디코드 안 함). 없으면 NULL */
    int         version_minor;  /* HTTP/1.0 → 0, HTTP/1.1 → 1 */
    const char *host;           /* Host 헤더 값. HTTP/1.0 에서 없으면 NULL */
    size_t      content_length; /* 없으면 0 */
    int         keep_alive;     /* 응답 후 연결을 유지할지 (버전 기본값 + Connection 헤더) */
    const char *body;           /* 본문 시작 (NUL 로 끝나지 않음). 없으면 NULL */
    size_t      body_len;
    const char *remote_addr;    /* 클라이언트 "ip:port". 파서는 NULL 로 두고 연결 처리(conn)가 채운다 (CGI 의 REMOTE_ADDR) */
    http_header headers[HTTP_MAX_HEADERS];
    size_t      n_headers;
    char        path_buf[HTTP_MAX_HEADER_BYTES]; /* path 가 가리키는 곳 (디코드 결과는 원본보다 길지 않음) */
} http_request;

typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_INCOMPLETE,
    HTTP_PARSE_BAD_REQUEST,      /* 400 문법 오류, Host 누락, 잘못된 Content-Length, ".." 경로 */
    HTTP_PARSE_BODY_TOO_LARGE,   /* 413 Content-Length > max_body */
    HTTP_PARSE_HEADER_TOO_LARGE, /* 431 헤더가 8KB 또는 32개 초과 */
    HTTP_PARSE_NOT_IMPLEMENTED,  /* 501 GET/POST/HEAD 이외 메서드, Transfer-Encoding */
    HTTP_PARSE_BAD_VERSION       /* 505 HTTP/1.x 가 아님 */
} http_parse_result;

/* buf[0..len) 에서 요청 하나를 파싱한다. max_body: 받아 줄 최대 본문 크기 */
http_parse_result http_parse(char *buf, size_t len, size_t max_body, http_request *req, size_t *consumed);

/* 이름이 같은 첫 헤더의 값 (대소문자 무시). 없으면 NULL */
const char *http_header_get(const http_request *req, const char *name);

/* 파싱 결과에 맞는 HTTP 상태 코드. OK / INCOMPLETE 는 0 */
int http_parse_status(http_parse_result r);

#endif
