/*
 * test_http.c — HTTP 파서와 응답 헤더 생성기 단위 테스트 (네트워크 없음)
 */
#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static HttpRequest request;
static size_t consumed;
static int status;

/* 문자열 전체를 한 번에 파싱합니다. */
static HttpParseResult parse(const char *text)
{
    return http_parse_request((const unsigned char *)text, strlen(text), &request, &consumed, &status);
}

/* 오류가 나야 하는 요청: 기대한 상태 코드인지 확인 */
static void expect_error(const char *text, int expected)
{
    HttpParseResult result = parse(text);
    if (result != HTTP_ERROR || status != expected) {
        fprintf(stderr, "expected %d, got result=%d status=%d for:\n%s\n", expected, result, status, text);
        assert(0);
    }
}

static void test_basic_get(void)
{
    const char *text = "GET /index.html?x=1 HTTP/1.1\r\nHost: localhost:8080\r\nUser-Agent: t\r\n\r\n";
    assert(parse(text) == HTTP_COMPLETE);
    assert(!strcmp(request.method, "GET"));
    assert(!strcmp(request.target, "/index.html?x=1"));
    assert(!strcmp(request.version, "HTTP/1.1"));
    assert(!strcmp(request.host, "localhost:8080"));
    assert(request.keep_alive == 1);            /* HTTP/1.1 기본은 keep-alive */
    assert(consumed == strlen(text));
    assert(request.body_length == 0);
    /* 헤더 원문은 요청 줄 다음부터 빈 줄 전까지 */
    assert(request.headers_length == strlen("Host: localhost:8080\r\nUser-Agent: t\r\n"));
    assert(!memcmp(request.headers, "Host: localhost:8080\r\n", 22));
}

static void test_post_body_and_split_delivery(void)
{
    /* TCP는 요청을 아무 곳에서나 쪼개 보낼 수 있습니다. 모든 분할 지점에서 결과가 같아야 합니다. */
    const char *text = "POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    size_t total = strlen(text);
    for (size_t cut = 0; cut < total; ++cut)
        assert(http_parse_request((const unsigned char *)text, cut, &request, &consumed, &status) == HTTP_NEED_MORE);
    assert(parse(text) == HTTP_COMPLETE);
    assert(request.body_length == 5 && !memcmp(request.body, "hello", 5));
    assert(consumed == total);
}

static void test_binary_body_and_pipelining(void)
{
    /* 본문에 NUL이 있어도 strlen으로 자르지 않고 Content-Length만큼 읽습니다. */
    const unsigned char text[] = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\nA\0BGET / HTTP/1.1\r\n";
    assert(http_parse_request(text, sizeof(text) - 1, &request, &consumed, &status) == HTTP_COMPLETE);
    assert(request.body_length == 3 && request.body[1] == 0);
    /* consumed는 첫 요청까지만. 뒤에 붙은 다음 요청은 서버가 다시 파싱합니다(파이프라이닝). */
    assert(consumed == sizeof(text) - 1 - strlen("GET / HTTP/1.1\r\n"));
}

static void test_connection_rules(void)
{
    assert(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n") == HTTP_COMPLETE);
    assert(request.keep_alive == 0);
    assert(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: Keep-Alive, CLOSE\r\n\r\n") == HTTP_COMPLETE);
    assert(request.keep_alive == 0);            /* close가 이김 */
    /* HTTP/1.0: Host 없어도 됨, 기본은 close, keep-alive 토큰이 있으면 유지 */
    assert(parse("GET / HTTP/1.0\r\n\r\n") == HTTP_COMPLETE);
    assert(request.keep_alive == 0 && request.host[0] == '\0');
    assert(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n") == HTTP_COMPLETE);
    assert(request.keep_alive == 1);
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nConnection: close;x\r\n\r\n", 400);
}

static void test_invalid_requests(void)
{
    expect_error("GET / HTTP/1.1\r\n\r\n", 400);                         /* 1.1인데 Host 없음 */
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);   /* Host 중복 */
    expect_error("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505);              /* 지원하지 않는 버전 */
    expect_error("GET / HTTX/1.1\r\nHost: a\r\n\r\n", 400);              /* 버전 형식 오류 */
    expect_error("GE(T / HTTP/1.1\r\nHost: a\r\n\r\n", 400);             /* 메서드에 금지 문자 */
    expect_error("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400);             /* 공백 두 칸 */
    expect_error("GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n", 400);      /* 절대 URL 미지원 */
    expect_error("GET / HTTP/1.1\nHost: a\r\n\r\n", 400);                /* CR 없는 LF */
    expect_error("GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400);             /* 이름 뒤 공백 */
    expect_error("GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", 400);   /* 콜론 없음 */
    expect_error("POST / HTTP/1.1\r\nHost: a\r\n\r\n", 411);             /* POST인데 길이 없음 */
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", 400);
    expect_error("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n", 417);
}

static void test_size_limits(void)
{
    static char text[HTTP_MAX_HEADER + 4096];
    /* 본문 상한 + 1 → 413 (숫자를 읽는 도중 바로 판단) */
    snprintf(text, sizeof(text), "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: %d\r\n\r\n", HTTP_MAX_BODY + 1);
    expect_error(text, 413);
    snprintf(text, sizeof(text), "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n");
    expect_error(text, 413);
    /* 경로 상한 + 1 → 414 */
    int n = snprintf(text, sizeof(text), "GET /");
    memset(text + n, 'a', HTTP_MAX_TARGET);
    snprintf(text + n + HTTP_MAX_TARGET, sizeof(text) - (size_t)n - HTTP_MAX_TARGET, " HTTP/1.1\r\nHost: a\r\n\r\n");
    expect_error(text, 414);
    /* 헤더 상한(16KB)보다 긴 URI: 요청 줄이 끝나기 전에 상한 도달 → 431이 아니라 414 */
    n = snprintf(text, sizeof(text), "GET /");
    memset(text + n, 'a', HTTP_MAX_HEADER);
    text[n + HTTP_MAX_HEADER] = '\0';
    expect_error(text, 414);
    /* 헤더가 상한까지 끝나지 않음 → 431 */
    n = snprintf(text, sizeof(text), "GET / HTTP/1.1\r\nHost: a\r\nX: ");
    memset(text + n, 'b', HTTP_MAX_HEADER);
    text[n + HTTP_MAX_HEADER] = '\0';
    expect_error(text, 431);
}

static void test_response_header(void)
{
    char out[256];
    int length = http_response_header(out, sizeof(out), 200, "text/html; charset=utf-8", 12, 1, NULL, NULL);
    const char *expected =
        "HTTP/1.1 200 OK\r\nServer: MicroHTTP\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 12\r\nConnection: keep-alive\r\n\r\n";
    assert(length == (int)strlen(expected) && !strcmp(out, expected));
    length = http_response_header(out, sizeof(out), 405, "text/plain", 0, 0, "GET, HEAD", NULL);
    assert(length > 0 && strstr(out, "Connection: close\r\n") && strstr(out, "Allow: GET, HEAD\r\n"));
    assert(http_response_header(out, sizeof(out), 299, "text/plain", 0, 0, NULL, NULL) == -1);       /* 모르는 코드 */
    assert(http_response_header(out, sizeof(out), 200, "a\r\nX: y", 0, 0, NULL, NULL) == -1);       /* 헤더 주입 */
    assert(http_response_header(out, 10, 200, "text/plain", 0, 0, NULL, NULL) == -1);               /* 공간 부족 */
}

static void test_extra_headers_and_find_header(void)
{
    char out[512];
    int length = http_response_header(out, sizeof(out), 303, "text/plain", 0, 1, NULL,
                                      "Location: /me\r\nSet-Cookie: sid=abc; HttpOnly\r\n");
    assert(length > 0 && strstr(out, "HTTP/1.1 303 See Other\r\n"));
    assert(strstr(out, "Set-Cookie: sid=abc; HttpOnly\r\n\r\n"));      /* 추가 헤더 뒤에 빈 줄 */
    assert(http_response_header(out, sizeof(out), 401, "text/plain", 0, 0, NULL, NULL) > 0);

    const unsigned char *value;
    size_t value_length;
    assert(parse("GET / HTTP/1.1\r\nHost: a\r\ncookie:  theme=dark; sid=xyz \r\nCookie: second=1\r\n\r\n") == HTTP_COMPLETE);
    assert(http_find_header(&request, "Cookie", &value, &value_length) == 1);   /* 대소문자 무시, 첫 번째 */
    assert(value_length == strlen("theme=dark; sid=xyz") && !memcmp(value, "theme=dark; sid=xyz", value_length));
    assert(http_find_header(&request, "Cook", &value, &value_length) == 0);      /* 접두어만 같으면 아님 */
    assert(http_find_header(&request, "Authorization", &value, &value_length) == 0);
}

int main(void)
{
    test_basic_get();
    test_post_body_and_split_delivery();
    test_binary_body_and_pipelining();
    test_connection_rules();
    test_invalid_requests();
    test_size_limits();
    test_response_header();
    test_extra_headers_and_find_header();
    puts("PASS test_http: request line, headers, body, split delivery, keep-alive, errors, limits, response header, extra headers, find header");
    return 0;
}
