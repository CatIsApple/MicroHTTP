/* test_rproxy.c — Reverse Proxy(rproxy) + 공통 부품(proxy_core) 순수 함수 단위 테스트:
 * 라우트 규칙 해석, 경로 매칭(가장 긴 prefix·경계·dot-segment), 백엔드로 보낼 요청 머리 다시 쓰기,
 * 백엔드 응답 머리 검사·다시 쓰기. 소켓 I/O(rproxy_handle · pxy_exchange)는 test_functional 이 실제 백엔드로 검사한다. */

#define _POSIX_C_SOURCE 200809L

#include "http_parser.h" /* http_parse_request (요청 머리 다시 쓰기 입력 만들기) */
#include "proxy_core.h"  /* 테스트 대상: 공통 부품 */
#include "rproxy.h"      /* 테스트 대상: 리버스 프록시 */
#include "test.h"        /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <arpa/inet.h>   /* ntohs, inet_ntop */
#include <string.h>      /* strstr, strlen, strcpy */

static rproxy_table g_t; /* 큼 → 전역 */

/* ---- 규칙 해석 ---- */
static void test_add_route(void)
{
    const char *why = NULL;
    rproxy_init(&g_t);
    CHECK_INT(g_t.timeout_ms, RPROXY_DEFAULT_TIMEOUT_MS);

    CHECK_INT(rproxy_add_route(&g_t, "/api/=127.0.0.1:8001,localhost:8002", &why), 0);
    CHECK_INT(g_t.nroutes, 1);
    CHECK_STR(g_t.routes[0].prefix, "/api/");
    CHECK_INT(g_t.routes[0].nbackends, 2);
    CHECK_STR(g_t.routes[0].backends[0].name, "127.0.0.1:8001");
    CHECK_INT(ntohs(g_t.routes[0].backends[0].addr.sin_port), 8001);
    CHECK_STR(g_t.routes[0].backends[1].name, "localhost:8002");      /* 이름도 시작할 때 해석 */
    char ip[32];
    inet_ntop(AF_INET, &g_t.routes[0].backends[1].addr.sin_addr, ip, sizeof(ip));
    CHECK_STR(ip, "127.0.0.1");

    /* 잘못된 규칙: 표는 그대로 */
    const char *bad[] = {
        "api=127.0.0.1:1",          /* '/' 로 시작 안 함 */
        "/api",                     /* '=' 없음 */
        "/api=",                    /* 백엔드 없음 */
        "=127.0.0.1:1",             /* prefix 없음 */
        "/a b=127.0.0.1:1",         /* prefix 에 공백 */
        "/x=127.0.0.1",             /* 포트 없음 */
        "/x=127.0.0.1:0",           /* 포트 0 */
        "/x=127.0.0.1:65536",       /* 포트 범위 밖 */
        "/x=127.0.0.1:80a",         /* 포트에 글자 */
        "/x=:80",                   /* 호스트 없음 */
        "/x=127.0.0.1:1,",          /* 빈 백엔드 */
        "/x=no-such-host.invalid:80", /* 해석 불가 (.invalid 는 절대 해석되지 않는 TLD) */
        "/api/=127.0.0.1:9",        /* 중복 prefix */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why = NULL;
        CHECK_INT(rproxy_add_route(&g_t, bad[i], &why), -1);
        CHECK(why != NULL);
        if (rproxy_add_route(&g_t, bad[i], NULL) != -1)                  /* why 가 NULL 이어도 안전 */
            fprintf(stderr, "accepted bad spec: %s\n", bad[i]);
    }
    CHECK_INT(g_t.nroutes, 1);

    /* 상한 */
    rproxy_init(&g_t);
    char spec[64];
    for (int i = 0; i < RPROXY_MAX_ROUTES; i++) {
        snprintf(spec, sizeof(spec), "/r%d=127.0.0.1:1", i);
        CHECK_INT(rproxy_add_route(&g_t, spec, NULL), 0);
    }
    CHECK_INT(rproxy_add_route(&g_t, "/more=127.0.0.1:1", NULL), -1);
}

/* ---- 매칭 ---- */
static void test_match(void)
{
    rproxy_init(&g_t);
    rproxy_add_route(&g_t, "/api/=127.0.0.1:1", NULL);
    rproxy_add_route(&g_t, "/api/v2/=127.0.0.1:2", NULL);
    rproxy_add_route(&g_t, "/static=127.0.0.1:3", NULL);
    rproxy_route *api = &g_t.routes[0], *v2 = &g_t.routes[1], *st = &g_t.routes[2];

    CHECK(rproxy_match(&g_t, "/api/users") == api);
    CHECK(rproxy_match(&g_t, "/api/") == api);
    CHECK(rproxy_match(&g_t, "/api") == NULL);                        /* "/api/" 규칙은 슬래시까지 필요 */
    CHECK(rproxy_match(&g_t, "/api/v2/x") == v2);                     /* 더 긴 prefix 가 이김 */
    CHECK(rproxy_match(&g_t, "/api/v2") == api);
    CHECK(rproxy_match(&g_t, "/static") == st);                       /* 슬래시 없는 prefix: 경계 검사 */
    CHECK(rproxy_match(&g_t, "/static/a.css") == st);
    CHECK(rproxy_match(&g_t, "/static?x=1") == st);
    CHECK(rproxy_match(&g_t, "/staticx") == NULL);
    CHECK(rproxy_match(&g_t, "/") == NULL);
    CHECK(rproxy_match(&g_t, "/API/users") == NULL);                  /* 경로는 대소문자 구분 */
    CHECK(rproxy_match(&g_t, "http://h/api/x") == NULL);              /* absolute-form 은 대상 아님 */
    CHECK(rproxy_match(NULL, "/api/x") == NULL);

    /* dot-segment 로 prefix 경계를 넘으려는 요청은 프록시로 보내지 않음 */
    CHECK(rproxy_match(&g_t, "/api/../secret") == NULL);
    CHECK(rproxy_match(&g_t, "/api/./x") == NULL);
    CHECK(rproxy_match(&g_t, "/api/%2e%2e/secret") == NULL);
    CHECK(rproxy_match(&g_t, "/api/.%2E/secret") == NULL);
    CHECK(rproxy_match(&g_t, "/api/..") == NULL);
    CHECK(rproxy_match(&g_t, "/api/x?path=../y") == api);             /* 쿼리 안의 ".." 는 경로가 아님 */
    CHECK(rproxy_match(&g_t, "/api/.hidden") == api);                 /* 점으로 시작할 뿐인 이름은 허용 */
    CHECK(rproxy_match(&g_t, "/api/a..b") == api);

    rproxy_table empty;
    rproxy_init(&empty);
    CHECK(rproxy_match(&empty, "/api/x") == NULL);
}

/* ---- 요청 머리 ---- */
static int build(const char *raw, const char *ip, char *out, size_t cap)
{
    static char buf[4096];
    strcpy(buf, raw);
    http_request req;
    if (http_parse_request(buf, strlen(buf), &req) != 0)
        return -2;
    pxy_req_opts o = {.target = req.uri, .client_ip = ip, .host = "backend:8001", .replace_host = 0};
    return pxy_build_request(out, cap, &req, &o);
}

static void test_build_request(void)
{
    char out[2048];
    int n = build("GET /api/x?q=1 HTTP/1.1\r\n"
                  "Host: example.com\r\n"
                  "User-Agent: t\r\n"
                  "Connection: keep-alive, X-Hop\r\n"
                  "Keep-Alive: timeout=5\r\n"
                  "X-Hop: secret\r\n"
                  "Proxy-Connection: keep-alive\r\n"
                  "Upgrade: websocket\r\n"
                  "TE: trailers\r\n"
                  "X-Forwarded-Proto: https\r\n"
                  "\r\n",
                  "10.0.0.9", out, sizeof(out));
    CHECK(n > 0);
    CHECK_STR(out, "GET /api/x?q=1 HTTP/1.1\r\n"
                   "Host: example.com\r\n"                         /* Host 는 그대로 (가상 호스트 백엔드용) */
                   "User-Agent: t\r\n"
                   "X-Forwarded-For: 10.0.0.9\r\n"
                   "X-Forwarded-Proto: http\r\n"                   /* 클라이언트가 보낸 값은 버리고 우리가 씀 */
                   "Connection: close\r\n"
                   "\r\n");
    CHECK_INT(n, (int)strlen(out));

    /* 기존 X-Forwarded-For 뒤에 덧붙임, POST 의 Content-Length 유지, Expect 제거 */
    n = build("POST /p HTTP/1.1\r\nHost: h\r\nX-Forwarded-For: 1.1.1.1\r\nX-Forwarded-For: 2.2.2.2\r\n"
              "Content-Length: 3\r\nExpect: 100-continue\r\n\r\n",
              "3.3.3.3", out, sizeof(out));
    CHECK(n > 0);
    CHECK(strstr(out, "X-Forwarded-For: 1.1.1.1, 2.2.2.2, 3.3.3.3\r\n") != NULL);
    CHECK(strstr(out, "Content-Length: 3\r\n") != NULL);
    CHECK(strstr(out, "Expect") == NULL);
    CHECK(strncmp(out, "POST /p HTTP/1.1\r\n", 18) == 0);

    /* HTTP/1.0, Host 없음 → 버전 유지 + Host 를 백엔드 주소로 */
    n = build("GET /a HTTP/1.0\r\n\r\n", "1.2.3.4", out, sizeof(out));
    CHECK(n > 0);
    CHECK(strncmp(out, "GET /a HTTP/1.0\r\n", 17) == 0);
    CHECK(strstr(out, "\r\nHost: backend:8001\r\n") != NULL);

    /* 모르는 메서드도 원문 그대로 */
    n = build("DELETE /api/1 HTTP/1.1\r\nHost: h\r\n\r\n", "1.2.3.4", out, sizeof(out));
    CHECK(n > 0 && strncmp(out, "DELETE /api/1 HTTP/1.1\r\n", 24) == 0);

    /* 버퍼 부족 → -1 */
    CHECK_INT(build("GET / HTTP/1.1\r\nHost: h\r\n\r\n", "1.2.3.4", out, 20), -1);
    CHECK_INT(build("GET / HTTP/1.1\r\nHost: h\r\n\r\n", "1.2.3.4", out, 0), -1);
}

/* ---- 응답 머리 ---- */
static int presp(const char *h, pxy_resp_info *i) { return pxy_parse_response_head(h, strlen(h), i); }

static void test_response_head(void)
{
    pxy_resp_info i;
    CHECK_INT(presp("HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\n", &i), 0);
    CHECK_INT(i.status, 200);
    CHECK_INT(i.has_length, 1);
    CHECK_INT(i.length, 12);
    CHECK_INT(i.chunked, 0);

    CHECK_INT(presp("HTTP/1.0 404 Not Found\r\n\r\n", &i), 0);   /* 길이 없음 → 연결 끝까지 */
    CHECK_INT(i.status, 404);
    CHECK_INT(i.has_length, 0);

    CHECK_INT(presp("HTTP/1.1 204\r\n\r\n", &i), 0);              /* 이유 문구 없음도 허용 */
    CHECK_INT(i.status, 204);

    CHECK_INT(presp("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, Chunked\r\nContent-Length: 5\r\n\r\n", &i), 0);
    CHECK_INT(i.chunked, 1);
    CHECK_INT(i.has_te, 1);
    CHECK_INT(i.has_length, 0);                                     /* TE 가 있으면 CL 무시 */

    CHECK_INT(presp("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", &i), 0);
    CHECK_INT(i.chunked, 0);                                        /* chunked 가 마지막이 아님 → 연결 끝까지 */

    CHECK_INT(presp("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &i), 0); /* 같은 값 중복은 OK */
    CHECK_INT(presp("HTTP/1.1 100 Continue\r\n\r\n", &i), 0);
    CHECK_INT(i.status, 100);

    const char *bad[] = {
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1 099 X\r\n\r\n",
        "HTTP/1.1 600 X\r\n\r\n",
        "ICY 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nNoColon\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: v\r\n\r\n",
        "HTTP/1.1 200 OK\r\nBad Name: v\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: b\r\n folded\r\n\r\n",            /* obs-fold */
        "HTTP/1.1 200 OK\r\nA: b\nInjected: 1\r\n\r\n",          /* 단독 LF → 응답 분할 */
        "HTTP/1.1 200 OK\r\nA: b\rc\r\n\r\n",                    /* 단독 CR */
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n",
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        int r = presp(bad[k], &i);
        CHECK_INT(r, -1);
        if (r != -1)
            fprintf(stderr, "accepted bad response head #%zu\n", k);
    }
    /* NUL 이 섞인 머리 (strlen 으로는 못 만드니 길이를 직접) */
    const char nul[] = "HTTP/1.1 200 OK\r\nA: x\0y\r\n\r\n";
    CHECK_INT(pxy_parse_response_head(nul, sizeof(nul) - 1, &i), -1);

    /* 다시 쓰기: 버전 1.1, hop-by-hop·Connection 에 적힌 헤더 제거, 우리 Connection 추가 */
    const char *h = "HTTP/1.0 201 Created\r\n"
                    "Content-Length: 2\r\n"
                    "Connection: close, X-Internal\r\n"
                    "Keep-Alive: timeout=1\r\n"
                    "X-Internal: 1\r\n"
                    "Set-Cookie: a=b\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n";
    char out[1024];
    int n = pxy_rewrite_response_head(out, sizeof(out), h, strlen(h), 1);
    CHECK(n > 0);
    CHECK_STR(out, "HTTP/1.1 201 Created\r\n"
                   "Content-Length: 2\r\n"
                   "Set-Cookie: a=b\r\n"
                   "Transfer-Encoding: chunked\r\n"                /* TE 는 중계할 본문의 형식이라 유지 */
                   "Connection: keep-alive\r\n"
                   "\r\n");
    CHECK_INT(n, (int)strlen(out));
    n = pxy_rewrite_response_head(out, sizeof(out), "HTTP/1.1 204\r\n\r\n", 16, 0);
    CHECK_STR(out, "HTTP/1.1 204\r\nConnection: close\r\n\r\n");
    CHECK_INT(pxy_rewrite_response_head(out, 20, h, strlen(h), 1), -1); /* 버퍼 부족 */
}

int main(void)
{
    test_add_route();
    test_match();
    test_build_request();
    test_response_head();
    return TEST_REPORT();
}
