/* test_http_request.c — L2 파서 단위 테스트. 소켓 없이 문자열만으로 http_parse() 를 확인한다.
 *
 * 구성: 정상 요청 → 덜 온 요청(INCOMPLETE) → 400 → 413/431/501/505 순서로 묶여 있다.
 * parse()/parse_len() 이 리터럴을 쓰기 가능한 전역 buf 에 복사해 파싱하고, 결과는 전역 req, used 에 남는다. */
#include "http_request.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define EXPECT(c)                                                        \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

#define MAX_BODY 1000
static char buf[HTTP_MAX_HEADER_BYTES * 2 + MAX_BODY];
static http_request req;
static size_t used;

/* 파서가 버퍼를 고치므로 리터럴을 쓰기 가능한 버퍼에 복사해서 넘긴다 */
static http_parse_result parse_len(const char *s, size_t n)
{
    memcpy(buf, s, n);
    return http_parse(buf, n, MAX_BODY, &req, &used);
}
/* NUL 로 끝나는 문자열 요청을 파싱한다 (parse_len 의 줄임) */
static http_parse_result parse(const char *s) { return parse_len(s, strlen(s)); }

/* ---- 정상 요청 ---- */

/* 기본 GET: 메서드, 대상, 디코드한 경로, 쿼리, 버전, Host, keep-alive, consumed 확인 */
static void get_request(void)
{
    const char *s = "GET /docs/index.html?lang=ko&x=1 HTTP/1.1\r\n"
                    "Host: localhost:8080\r\n"
                    "User-Agent:   curl/8.5   \r\n"
                    "Accept: */*\r\n"
                    "\r\n";
    EXPECT(parse(s) == HTTP_PARSE_OK);
    EXPECT(used == strlen(s));
    EXPECT(req.method == HTTP_GET);
    EXPECT(strcmp(req.method_str, "GET") == 0);
    EXPECT(strcmp(req.target, "/docs/index.html?lang=ko&x=1") == 0);
    EXPECT(strcmp(req.path, "/docs/index.html") == 0);
    EXPECT(req.query && strcmp(req.query, "lang=ko&x=1") == 0);
    EXPECT(req.version_minor == 1);
    EXPECT(req.host && strcmp(req.host, "localhost:8080") == 0);
    EXPECT(req.n_headers == 3);
    EXPECT(strcmp(req.headers[1].name, "User-Agent") == 0);
    EXPECT(strcmp(req.headers[1].value, "curl/8.5") == 0); /* 앞뒤 공백 제거 */
    EXPECT(req.keep_alive == 1);                          /* 1.1 기본값 */
    EXPECT(req.content_length == 0 && req.body == NULL && req.body_len == 0);
}

/* POST 본문이 Content-Length 만큼 잡히고 consumed 에 본문까지 포함되는지 */
static void post_request(void)
{
    const char *s = "POST /form HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "Content-Length: 11\r\n"
                    "\r\n"
                    "name=da0nn!";
    EXPECT(parse(s) == HTTP_PARSE_OK);
    EXPECT(req.method == HTTP_POST);
    EXPECT(req.content_length == 11 && req.body_len == 11);
    EXPECT(req.body && memcmp(req.body, "name=da0nn!", 11) == 0);
    EXPECT(used == strlen(s));
    EXPECT(req.query == NULL);
}

/* 본문은 바이너리일 수 있다 (NUL, CRLF 포함) */
static void binary_body(void)
{
    const char s[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\na\0\r\n\r\nb";
    EXPECT(parse_len(s, sizeof s - 1) == HTTP_PARSE_OK);
    EXPECT(req.body_len == 6 && memcmp(req.body, "a\0\r\n\r\n", 6) == 0);
}

/* HEAD 메서드, 그리고 HTTP/1.0 의 Host 생략·기본 연결 닫기 */
static void head_and_http10(void)
{
    EXPECT(parse("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_OK && req.method == HTTP_HEAD);
    /* HTTP/1.0 은 Host 가 없어도 되고, 기본으로 연결을 닫는다 */
    EXPECT(parse("GET / HTTP/1.0\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(req.version_minor == 0 && req.host == NULL && req.keep_alive == 0);
}

/* http_header_get: 대소문자 무시, 빈 값, 같은 이름이 여러 개면 첫 값 */
static void header_lookup(void)
{
    EXPECT(parse("GET / HTTP/1.1\r\nhost: a\r\nX-Empty:\r\nX-Dup: 1\r\nx-dup: 2\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(req.host && strcmp(req.host, "a") == 0); /* 헤더 이름은 대소문자 무시 */
    EXPECT(strcmp(http_header_get(&req, "HOST"), "a") == 0);
    EXPECT(strcmp(http_header_get(&req, "x-empty"), "") == 0);
    EXPECT(strcmp(http_header_get(&req, "X-DUP"), "1") == 0); /* 같은 이름이면 첫 번째 */
    EXPECT(http_header_get(&req, "Cookie") == NULL);
}

/* Connection 헤더(close, keep-alive, 쉼표 목록)에 따른 keep_alive 값 */
static void connection_header(void)
{
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(req.keep_alive == 0);
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, Close\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(req.keep_alive == 0);
    EXPECT(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(req.keep_alive == 1);
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(req.keep_alive == 1);
}

/* %XX 디코드 결과가 path 에 들어가고 쿼리·target 은 그대로인지. "a..b" 같은 이름은 허용 */
static void path_decoding(void)
{
    EXPECT(parse("GET /hello%20world/%ED%95%9C?q=%20 HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_OK);
    EXPECT(strcmp(req.path, "/hello world/\xED\x95\x9C") == 0); /* "한" (UTF-8) */
    EXPECT(strcmp(req.query, "q=%20") == 0);                    /* 쿼리는 그대로 */
    EXPECT(strcmp(req.target, "/hello%20world/%ED%95%9C?q=%20") == 0);
    EXPECT(parse("GET /a..b/.hidden/... HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_OK);
}

/* 앞에 붙은 빈 줄은 무시 */
static void leading_blank_lines(void)
{
    const char *s = "\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n";
    EXPECT(parse(s) == HTTP_PARSE_OK && used == strlen(s));
}

/* ---- 덜 온 요청: INCOMPLETE, 그리고 버퍼를 건드리지 않음 ---- */

/* 요청의 모든 앞부분(0..n-1 바이트)이 INCOMPLETE 이고 버퍼를 바꾸지 않는지, 전체는 OK 인지 */
static void every_prefix_is_incomplete(void)
{
    const char *s = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nabcd";
    size_t n = strlen(s);
    memcpy(buf, s, n);
    for (size_t k = 0; k < n; k++) {
        http_parse_result r = http_parse(buf, k, MAX_BODY, &req, &used);
        if (r != HTTP_PARSE_INCOMPLETE || used != 0) {
            fprintf(stderr, "FAIL prefix %zu: result %d\n", k, (int)r);
            failures++;
        }
        EXPECT(memcmp(buf, s, n) == 0); /* 같은 버퍼로 다시 불러도 되도록 그대로여야 함 */
    }
    EXPECT(http_parse(buf, n, MAX_BODY, &req, &used) == HTTP_PARSE_OK && used == n);
    EXPECT(memcmp(req.body, "abcd", 4) == 0);
}

/* 버퍼에 다음 요청이 이어 붙어 있어도 consumed 로 경계를 알려 준다 */
static void two_requests_in_buffer(void)
{
    const char *first = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi";
    const char *second = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    char s[256];
    snprintf(s, sizeof s, "%s%s", first, second);
    size_t n = strlen(s);
    EXPECT(parse(s) == HTTP_PARSE_OK);
    EXPECT(used == strlen(first) && strcmp(req.path, "/a") == 0);
    memmove(buf, buf + used, n - used);
    EXPECT(http_parse(buf, n - strlen(first), MAX_BODY, &req, &used) == HTTP_PARSE_OK);
    EXPECT(strcmp(req.path, "/b") == 0 && used == strlen(second));
}

/* ---- 400 ---- */

/* 요청 라인 문법 오류들이 400 이 되는지 */
static void bad_request_line(void)
{
    EXPECT(parse("HELLO\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET  / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);  /* 공백 두 칸 */
    EXPECT(parse("GET / HTTP/1.1 \r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);  /* 끝에 공백 */
    EXPECT(parse("GET page HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST); /* '/' 로 시작 안 함 */
    EXPECT(parse("GET / http/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET / HTTP/1.11\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("G@T / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /\x01 HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
}

/* CRLF 가 아닌 줄바꿈(LF 만, 줄 중간 CR)이 400 이 되는지 */
static void bad_line_endings(void)
{
    EXPECT(parse("GET / HTTP/1.1\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);   /* LF 만 */
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\ry\r\n\r\n") == HTTP_PARSE_BAD_REQUEST); /* 줄 중간 CR */
    EXPECT(parse("GET / HTTP/1.1\nHost: x\n\n") == HTTP_PARSE_INCOMPLETE); /* CRLF 빈 줄이 없으면 아직 안 끝남 */
}

/* Host 누락·중복, 잘못된 헤더 이름/값이 400 이 되는지 */
static void bad_headers(void)
{
    EXPECT(parse("GET / HTTP/1.1\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);                       /* Host 없음 */
    EXPECT(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n") == HTTP_PARSE_BAD_REQUEST); /* Host 두 개 */
    EXPECT(parse("GET / HTTP/1.1\r\nHost : x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);           /* 콜론 앞 공백 */
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\r\n  more\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);  /* 이어쓰기 줄 */
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\r\nNoColon\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET / HTTP/1.1\r\nHost: x\r\n: novalue\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    const char nul[] = "GET / HTTP/1.1\r\nHost: x\r\nX: a\0b\r\n\r\n";
    EXPECT(parse_len(nul, sizeof nul - 1) == HTTP_PARSE_BAD_REQUEST);
}

/* 숫자가 아니거나, 넘치거나, 서로 다른 Content-Length 가 400 이 되는지 */
static void bad_content_length(void)
{
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: ten\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\nx") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1 1\r\n\r\nxx") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n") ==
           HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx") ==
           HTTP_PARSE_BAD_REQUEST);
    /* 같은 값이 두 번이면 괜찮다 */
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nxx") ==
           HTTP_PARSE_OK);
}

/* ".." 세그먼트(인코딩한 것 포함), 잘못된 %XX, %00 이 400 이 되는지 */
static void bad_paths(void)
{
    EXPECT(parse("GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /img/.. HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /%2E%2e/secret HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /a%2F..%2Fb HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /a%00.html HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /a% HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /a%2 HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
    EXPECT(parse("GET /a%g0 HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_REQUEST);
}

/* ---- 413 / 431 / 501 / 505 ---- */

/* 본문 크기 초과는 413, 헤더 크기·개수 초과는 431 */
static void too_large(void)
{
    static char s[HTTP_MAX_HEADER_BYTES * 2];
    size_t n;

    /* 본문이 max_body 초과 → 본문을 기다리지 않고 바로 413 */
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1001\r\n\r\n") == HTTP_PARSE_BODY_TOO_LARGE);
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\n") == HTTP_PARSE_INCOMPLETE);

    /* 헤더 33개 → 431, 32개 → OK */
    for (int count = HTTP_MAX_HEADERS; count <= HTTP_MAX_HEADERS + 1; count++) {
        n = (size_t)sprintf(s, "GET / HTTP/1.1\r\nHost: x\r\n");
        for (int i = 1; i < count; i++)
            n += (size_t)sprintf(s + n, "X-%d: v\r\n", i);
        n += (size_t)sprintf(s + n, "\r\n");
        http_parse_result want = count == HTTP_MAX_HEADERS ? HTTP_PARSE_OK : HTTP_PARSE_HEADER_TOO_LARGE;
        EXPECT(parse_len(s, n) == want);
    }

    /* 빈 줄 없이 8KB 가 차면 더 기다리지 않고 431 */
    n = (size_t)sprintf(s, "GET / HTTP/1.1\r\nX: ");
    memset(s + n, 'a', sizeof s - n);
    EXPECT(parse_len(s, HTTP_MAX_HEADER_BYTES - 1) == HTTP_PARSE_INCOMPLETE);
    EXPECT(parse_len(s, HTTP_MAX_HEADER_BYTES) == HTTP_PARSE_HEADER_TOO_LARGE);

    /* 빈 줄이 8KB 를 넘는 위치에 있어도 431 */
    n = (size_t)sprintf(s, "GET / HTTP/1.1\r\nHost: x\r\nX: ");
    memset(s + n, 'a', HTTP_MAX_HEADER_BYTES);
    n += HTTP_MAX_HEADER_BYTES;
    n += (size_t)sprintf(s + n, "\r\n\r\n");
    EXPECT(parse_len(s, n) == HTTP_PARSE_HEADER_TOO_LARGE);
}

/* 지원하지 않는 메서드·Transfer-Encoding 은 501, HTTP/1.x 가 아니면 505 */
static void not_supported(void)
{
    EXPECT(parse("PUT / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_NOT_IMPLEMENTED);
    EXPECT(parse("DELETE / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_NOT_IMPLEMENTED);
    EXPECT(parse("get / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_PARSE_NOT_IMPLEMENTED); /* 대소문자 구분 */
    EXPECT(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n") == HTTP_PARSE_NOT_IMPLEMENTED);
    EXPECT(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n") == HTTP_PARSE_BAD_VERSION);
    EXPECT(parse("GET / HTTP/0.9\r\n\r\n") == HTTP_PARSE_BAD_VERSION);
}

/* http_parse_status 가 결과마다 맞는 상태 코드를 돌려주는지 */
static void status_codes(void)
{
    EXPECT(http_parse_status(HTTP_PARSE_OK) == 0);
    EXPECT(http_parse_status(HTTP_PARSE_INCOMPLETE) == 0);
    EXPECT(http_parse_status(HTTP_PARSE_BAD_REQUEST) == 400);
    EXPECT(http_parse_status(HTTP_PARSE_BODY_TOO_LARGE) == 413);
    EXPECT(http_parse_status(HTTP_PARSE_HEADER_TOO_LARGE) == 431);
    EXPECT(http_parse_status(HTTP_PARSE_NOT_IMPLEMENTED) == 501);
    EXPECT(http_parse_status(HTTP_PARSE_BAD_VERSION) == 505);
}

/* 모든 테스트를 실행하고, 실패가 있으면 개수를 찍고 1 을 돌려준다 */
int main(void)
{
    get_request();
    post_request();
    binary_body();
    head_and_http10();
    header_lookup();
    connection_header();
    path_decoding();
    leading_blank_lines();
    every_prefix_is_incomplete();
    two_requests_in_buffer();
    bad_request_line();
    bad_line_endings();
    bad_headers();
    bad_content_length();
    bad_paths();
    too_large();
    not_supported();
    status_codes();
    if (failures) {
        fprintf(stderr, "test_http_request: %d failed\n", failures);
        return 1;
    }
    printf("test_http_request: ok\n");
    return 0;
}
