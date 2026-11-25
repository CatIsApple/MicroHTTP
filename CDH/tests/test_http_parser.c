/* test_http_parser.c — http_parse_request / http_get_header 단위 테스트.
 * 소켓 없이 문자열만 넣어 본다. 정상 요청의 필드 추출, 오류 코드 표의 모든 줄, 경계값. */

#include "http_parser.h" /* 테스트 대상 */
#include "test.h"        /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <stdint.h>      /* SIZE_MAX */
#include <stdio.h>       /* snprintf */
#include <string.h>      /* memcpy, strlen, strcmp, memset */

static char g_buf[32768]; /* 파싱 대상 버퍼. 파서가 '\0' 을 써 넣으므로 매번 복사해서 씀 */

/* data 의 len 바이트를 파싱. 헤더 끝까지를 header_len 으로 넘긴다 (본문이 섞여 있어도 됨) */
static int parse_n(const char *data, size_t len, http_request *req)
{
    memcpy(g_buf, data, len);                        /* 원본을 건드리지 않게 복사 */
    g_buf[len] = '\0';                               /* tcp_recv_request 처럼 끝에 '\0' */
    size_t header_len = len;                         /* 기본: 전체가 헤더 */
    for (size_t i = 0; i + 4 <= len; i++) {          /* 첫 "\r\n\r\n" 위치 찾기 (NUL 이 섞여도 동작하게 직접 검색) */
        if (memcmp(g_buf + i, "\r\n\r\n", 4) == 0) { /* 찾으면 */
            header_len = i + 4;                      /* 빈 줄까지 포함한 길이 */
            break;                                   /* 첫 번째만 */
        }
    }
    return http_parse_request(g_buf, header_len, req); /* 파싱 */
}

static int parse(const char *s, http_request *req) /* NUL 없는 문자열용 줄임 함수 */
{
    return parse_n(s, strlen(s), req);
}

static int code(const char *s) /* 결과 코드만 필요할 때 */
{
    http_request req;
    return parse(s, &req);
}

/* ---- 정상 요청 ---- */
static void test_ok_requests(void)
{
    http_request r;

    CHECK_INT(parse("GET /index.html?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n", &r), 0);
    CHECK_INT(r.method, HTTP_METHOD_GET);
    CHECK_STR(r.method_str, "GET");
    CHECK_STR(r.uri, "/index.html?x=1");
    CHECK_INT(r.version_minor, 1);
    CHECK_STR(r.host, "example.com");
    CHECK_INT(r.header_count, 1);
    CHECK_INT(r.has_content_length, 0);
    CHECK_INT(r.keep_alive, 1);              /* 1.1 기본값 */

    CHECK_INT(parse("POST /submit HTTP/1.1\r\nHost:  a.com \t\r\nContent-Length: 5\r\n"
                    "User-Agent: t\r\n\r\nhello", &r), 0);
    CHECK_INT(r.method, HTTP_METHOD_POST);
    CHECK_STR(r.host, "a.com");              /* 앞뒤 공백·탭 제거 */
    CHECK_INT(r.has_content_length, 1);
    CHECK_INT(r.content_length, 5);
    CHECK_INT(r.header_count, 3);
    CHECK_STR(http_get_header(&r, "user-agent"), "t");   /* 이름 대소문자 무시 */
    CHECK_STR(http_get_header(&r, "CONTENT-LENGTH"), "5");
    CHECK_STR(http_get_header(&r, "X-None"), NULL);      /* 없는 헤더 */

    CHECK_INT(parse("GET / HTTP/1.0\r\n\r\n", &r), 0);   /* 1.0 은 Host 없어도 됨 */
    CHECK_INT(r.version_minor, 0);
    CHECK_STR(r.host, NULL);
    CHECK_INT(r.keep_alive, 0);              /* 1.0 기본값 */

    CHECK_INT(parse("GET http://x.com/a HTTP/1.1\r\nHost: x.com\r\n\r\n", &r), 0); /* absolute-form */
    CHECK_STR(r.uri, "http://x.com/a");

    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nX-Empty:\r\n\r\n", &r), 0); /* 빈 값 허용 */
    CHECK_STR(http_get_header(&r, "x-empty"), "");

    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nX-Kr: \xed\x95\x9c\r\n\r\n", &r), 0); /* UTF-8 (obs-text) 값 허용 */
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nX-Tab: a\tb\r\n\r\n", &r), 0);        /* 값 안의 탭 허용 */
    CHECK_STR(http_get_header(&r, "x-tab"), "a\tb");
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nX-Colon: a:b:c\r\n\r\n", &r), 0);     /* 값 안의 ':' 허용 */
    CHECK_STR(http_get_header(&r, "x-colon"), "a:b:c");
}

/* ---- keep-alive 결정 표 ---- */
static void test_keep_alive(void)
{
    http_request r;
    parse("GET / HTTP/1.1\r\nHost: a\r\n\r\n", &r);                            CHECK_INT(r.keep_alive, 1);
    parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n", &r);  CHECK_INT(r.keep_alive, 1);
    parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", &r);       CHECK_INT(r.keep_alive, 0);
    parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, CLOSE\r\n\r\n", &r); CHECK_INT(r.keep_alive, 0); /* 목록 + 대소문자 */
    parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: x\r\nConnection: close\r\n\r\n", &r); CHECK_INT(r.keep_alive, 0); /* 여러 줄 */
    parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: closed\r\n\r\n", &r);      CHECK_INT(r.keep_alive, 1); /* 부분 일치는 아님 */
    parse("GET / HTTP/1.0\r\n\r\n", &r);                                       CHECK_INT(r.keep_alive, 0);
    parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r);             CHECK_INT(r.keep_alive, 1);
    parse("GET / HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n", &r);      CHECK_INT(r.keep_alive, 0); /* close 우선 */
}

/* ---- 오류 코드 표 ---- */
static void test_errors(void)
{
    /* 줄바꿈·구조 */
    CHECK_INT(code("GET / HTTP/1.1\nHost: a\r\n\r\n"), 400);           /* 단독 LF */
    CHECK_INT(code("GET / HTTP/1.1\rX\r\nHost: a\r\n\r\n"), 400);      /* 단독 CR */
    CHECK_INT(code("GET / HTTP/1.1\r\nHost: a\r\n"), 400);             /* 빈 줄 없음 */
    CHECK_INT(code("\r\n\r\n"), 400);                                  /* 요청 라인 없음 */
    {
        const char nul[] = "GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n";     /* 헤더 안 NUL */
        http_request r;
        CHECK_INT(parse_n(nul, sizeof(nul) - 1, &r), 400);
    }

    /* 요청 라인 */
    CHECK_INT(code("GET  / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);        /* 공백 두 개 */
    CHECK_INT(code("GET / HTTP/1.1 \r\nHost: a\r\n\r\n"), 400);        /* 끝에 공백 */
    CHECK_INT(code("GET /\r\nHost: a\r\n\r\n"), 400);                  /* 버전 없음 (HTTP/0.9 형태) */
    CHECK_INT(code(" GET / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);        /* 앞에 공백 */
    CHECK_INT(code("G(T / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);         /* 메서드에 token 이외 문자 */
    CHECK_INT(code("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n"), 400); /* URI 형태 이상 */
    CHECK_INT(code("GET /a\x01 HTTP/1.1\r\nHost: a\r\n\r\n"), 400);    /* URI 제어 문자 */
    CHECK_INT(code("GET http:// HTTP/1.1\r\nHost: a\r\n\r\n"), 400);   /* 호스트 없는 absolute-form */

    /* 버전 */
    CHECK_INT(code("GET / HTTX/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(code("GET / http/1.1\r\nHost: a\r\n\r\n"), 400);         /* 소문자 */
    CHECK_INT(code("GET / HTTP/1.10\r\nHost: a\r\n\r\n"), 400);        /* 길이 이상 */
    CHECK_INT(code("GET / HTTP/1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(code("GET / HTTP/2.0\r\nHost: a\r\n\r\n"), 505);
    CHECK_INT(code("GET / HTTP/1.2\r\nHost: a\r\n\r\n"), 505);
    CHECK_INT(code("GET / HTTP/0.9\r\nHost: a\r\n\r\n"), 505);

    /* 메서드 */
    /* 모르는 메서드는 파서에서 통과 (OTHER) → 프록시는 전달, 정적 경로는 server.c 가 501 */
    {
        http_request m;
        CHECK_INT(parse("PUT /x HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\n", &m), 0);
        CHECK_INT(m.method, HTTP_METHOD_OTHER);
        CHECK_STR(m.method_str, "PUT");
        CHECK_INT(m.keep_alive, 1);                                    /* keep-alive 판정까지 끝난 상태 */
        CHECK_INT(m.content_length, 2);
        CHECK_INT(parse("DELETE / HTTP/1.1\r\nHost: a\r\n\r\n", &m), 0);
        CHECK_INT(m.method, HTTP_METHOD_OTHER);
        CHECK_INT(parse("get / HTTP/1.1\r\nHost: a\r\n\r\n", &m), 0); /* 대소문자 구분 → GET 아님 */
        CHECK_INT(m.method, HTTP_METHOD_OTHER);
    }
    /* CONNECT: authority-form("host:port") 만 허용 (RFC 7230 5.3.3), 다른 메서드는 authority-form 불가 */
    {
        http_request m;
        CHECK_INT(parse("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n", &m), 0);
        CHECK_INT(m.method, HTTP_METHOD_CONNECT);
        CHECK_STR(m.uri, "example.com:443");
    }
    CHECK_INT(code("CONNECT / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(code("CONNECT http://a/ HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(code("GET example.com:443 HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(code("connect a:443 HTTP/1.1\r\nHost: a\r\n\r\n"), 400); /* 소문자는 CONNECT 아님 → authority-form 불가 */
    CHECK_INT(code("PUT / HTTP/1.1\r\n\r\n"), 400);                    /* 문법 오류(Host 없음)는 여전히 400 */
    CHECK_INT(code("PUT / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"), 501); /* TE 는 메서드와 무관하게 501 */

    /* 헤더 줄 */
    CHECK_INT(code("GET / HTTP/1.1\r\nHost : a\r\n\r\n"), 400);        /* 콜론 앞 공백 */
    CHECK_INT(code("GET / HTTP/1.1\r\n: a\r\nHost: a\r\n\r\n"), 400);  /* 이름 비어 있음 */
    CHECK_INT(code("GET / HTTP/1.1\r\nNoColon\r\nHost: a\r\n\r\n"), 400);
    CHECK_INT(code("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n"), 400); /* obs-fold */
    CHECK_INT(code("GET / HTTP/1.1\r\nHost: a\r\nX: a\x01" "b\r\n\r\n"), 400); /* 값 제어 문자 */
    CHECK_INT(code("GET / HTTP/1.1\r\nHost: a\r\nX: a\x7f\r\n\r\n"), 400);     /* 값 DEL */

    /* Host */
    CHECK_INT(code("GET / HTTP/1.1\r\n\r\n"), 400);                    /* 1.1 Host 필수 */
    CHECK_INT(code("GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n"), 400); /* 중복 */

    /* Content-Length */
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n"), 400);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n"), 400);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5a\r\n\r\n"), 400);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\n\r\n"), 400);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n"), 400);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"), 400);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n"), 0); /* 같은 값 중복은 허용 */

    /* Transfer-Encoding */
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
    CHECK_INT(code("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
}

/* ---- 경계값 ---- */
static void test_limits(void)
{
    static char req[16384]; /* 큰 요청을 만들 공간 */
    char uri[HTTP_MAX_URI_LEN + 2];

    /* URI 2048 → 통과, 2049 → 414 */
    uri[0] = '/';
    memset(uri + 1, 'a', HTTP_MAX_URI_LEN - 1);
    uri[HTTP_MAX_URI_LEN] = '\0';                               /* 길이 정확히 2048 */
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", uri);
    CHECK_INT(code(req), 0);
    uri[HTTP_MAX_URI_LEN] = 'a';
    uri[HTTP_MAX_URI_LEN + 1] = '\0';                           /* 길이 2049 */
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", uri);
    CHECK_INT(code(req), 414);

    /* 헤더 64개 → 통과, 65개 → 431 (Host 포함 개수) */
    size_t off = (size_t)snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 1; i < HTTP_MAX_HEADERS; i++)                  /* Host + 63 = 64개 */
        off += (size_t)snprintf(req + off, sizeof(req) - off, "X-%d: v\r\n", i);
    snprintf(req + off, sizeof(req) - off, "\r\n");
    CHECK_INT(code(req), 0);
    off += (size_t)snprintf(req + off, sizeof(req) - off, "X-last: v\r\n"); /* 65개째 */
    snprintf(req + off, sizeof(req) - off, "\r\n");
    CHECK_INT(code(req), 431);

    /* Content-Length: size_t 최댓값 → 통과 (파서는 상한을 안 봄. 413 은 http_body 몫), 한 자리 더 → 400 */
    http_request r;
    snprintf(req, sizeof(req), "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: %zu\r\n\r\n", (size_t)SIZE_MAX);
    CHECK_INT(parse(req, &r), 0);
    CHECK(r.content_length == SIZE_MAX);
    snprintf(req, sizeof(req), "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: %zu0\r\n\r\n", (size_t)SIZE_MAX);
    CHECK_INT(code(req), 400);                                  /* 오버플로 */
}

int main(void)
{
    test_ok_requests();
    test_keep_alive();
    test_errors();
    test_limits();
    return TEST_REPORT();
}
