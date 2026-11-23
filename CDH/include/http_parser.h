/* http_parser.h — HTTP/1.x 요청 헤더 파서의 "목차"(선언부).
 * 구현은 src/http_parser.c. 소켓과 무관한 순수 함수라 단위 테스트가 쉽다.
 *
 * 방식: "제자리(in-place) 파싱".
 *   요청 버퍼를 복사하지 않고, 구분자(공백, ':', CR)를 '\0' 으로 바꿔 끊은 뒤
 *   구조체에는 버퍼 안을 가리키는 포인터만 저장한다.
 *   → malloc/free 가 없음. 대신 req 의 문자열은 buf 가 살아 있는 동안만 유효. */

#ifndef HTTP_PARSER_H /* 인클루드 가드: 이 헤더가 아직 포함된 적 없을 때만 아래 내용을 읽는다 */
#define HTTP_PARSER_H /* "이미 포함됨" 표시. 두 번 include 해도 선언이 중복되지 않음 */

#include <stddef.h> /* size_t (바이트 수·길이를 나타내는 부호 없는 정수 타입) */

#define HTTP_MAX_HEADERS 64   /* 헤더 줄 최대 개수. 넘으면 431 (헤더 폭탄 방지 + 고정 크기 배열 크기) */
#define HTTP_MAX_URI_LEN 2048 /* URI 최대 길이(바이트). 넘으면 414 (브라우저·서버들의 흔한 상한) */

/* 요청 메서드. GET/POST 만 지원, 그 외 문법상 올바른 메서드는 OTHER(→ 501) */
typedef enum {          /* enum: 이름 붙은 정수 상수 묶음. 문자열 비교를 한 번만 하고 이후엔 정수로 분기 */
    HTTP_METHOD_GET,    /* "GET"  (= 0) : 리소스 조회 */
    HTTP_METHOD_POST,   /* "POST" (= 1) : 본문을 서버로 전송 */
    HTTP_METHOD_OTHER   /* 그 외 ("PUT", "DELETE" 등) : 문법은 맞지만 구현 안 함 → 501 */
} http_method;          /* 타입 이름 */

/* 헤더 한 줄. name, value 모두 '\0' 종료 문자열 (value 는 앞뒤 공백 제거됨) */
typedef struct {          /* 헤더 하나 = 이름 + 값 쌍 */
    const char *name;     /* 헤더 이름 ("Host"). buf 안을 가리킴, 원문 대소문자 그대로 */
    const char *value;    /* 헤더 값 ("example.com"). buf 안을 가리킴 */
} http_header;            /* 타입 이름 */

/* 파싱 결과 */
typedef struct {
    http_method method;     /* 메서드 종류 (GET / POST / OTHER) */
    const char *method_str; /* 메서드 원문 ("GET", "PUT" 등). 로그·501 응답용 */
    const char *uri;        /* 요청 대상 ("/index.html?x=1" 또는 "http://host/path") */
    int version_minor;      /* HTTP/1.0 → 0, HTTP/1.1 → 1 (major 는 항상 1 이라 저장 안 함) */

    http_header headers[HTTP_MAX_HEADERS]; /* 받은 순서대로의 헤더 목록 (구조체 안 고정 배열 → malloc 불필요) */
    size_t header_count;                   /* headers 중 실제로 채워진 개수 */

    const char *host;        /* Host 헤더 값. 없으면 NULL (자주 쓰니 따로 뽑아 둠) */
    int has_content_length;  /* Content-Length 헤더가 있었으면 1, 없으면 0 */
    size_t content_length;   /* 본문 길이 (헤더 없으면 0) */
    int keep_alive;          /* 응답 후 연결 유지 여부 1/0 (버전 + Connection 헤더로 결정) */

    const char *body;        /* 본문 시작 위치 — 다음 단계(본문 수신)에서 채움 */
    size_t body_len;         /* 본문 길이 — 다음 단계에서 채움 */
} http_request;              /* 타입 이름 */

/* 요청 헤더(요청 라인 + 헤더 줄들 + 빈 줄)를 파싱한다.
 * buf        : 요청 바이트. 파싱하면서 구분자 자리에 '\0' 을 써 넣으므로 수정 가능해야 함
 * header_len : 빈 줄("\r\n\r\n")까지 포함한 헤더 길이 (tcp_recv_request 가 준 값)
 * 반환: 0 = 성공, 그 외 = 클라이언트에게 보낼 HTTP 상태 코드
 *   400 문법 오류 / 414 URI 너무 김 / 431 헤더 개수 초과
 *   501 지원 안 하는 메서드·Transfer-Encoding / 505 지원 안 하는 HTTP 버전 */
int http_parse_request(char *buf, size_t header_len, http_request *req); /* req: 결과를 채울 구조체 */

/* 이름으로 헤더 값 찾기 (대소문자 무시). 여러 개면 첫 번째, 없으면 NULL */
const char *http_get_header(const http_request *req, const char *name); /* 예: http_get_header(&req, "user-agent") */

#endif /* HTTP_PARSER_H — 인클루드 가드 끝 */
