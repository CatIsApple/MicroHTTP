/*
 * test_proxy.c — 라우팅 규칙, Round-Robin, 백엔드 요청 생성 단위 테스트 (네트워크 없음)
 */
#include "proxy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_rule_parsing(void)
{
    ProxyTable table = { .lock = PTHREAD_MUTEX_INITIALIZER };
    assert(proxy_add_route(&table, "/api/=127.0.0.1:9001,localhost:9002") == 0);
    assert(table.route_count == 1);
    ProxyRoute *route = &table.routes[0];
    assert(!strcmp(route->prefix, "/api/") && route->backend_count == 2);
    assert(!strcmp(route->backends[0].host, "127.0.0.1") && !strcmp(route->backends[0].port, "9001"));
    assert(!strcmp(route->backends[1].host, "localhost") && !strcmp(route->backends[1].port, "9002"));

    /* 형식 오류는 거절하고 표에 추가하지 않음 */
    const char *invalid[] = {
        "api/=127.0.0.1:9001",      /* prefix가 '/'로 시작하지 않음 */
        "/api/",                    /* '=' 없음 */
        "/api/=",                   /* 백엔드 없음 */
        "/api/=127.0.0.1",          /* 포트 없음 */
        "/api/=127.0.0.1:0",        /* 포트 범위 */
        "/api/=127.0.0.1:70000",
        "/api/=127.0.0.1:90a1",
        "/api/=:9001",              /* 호스트 없음 */
        "/api/=127.0.0.1:9001,",    /* 빈 항목 */
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(proxy_add_route(&table, invalid[i]) == -1);
    assert(table.route_count == 1);
}

static void test_longest_prefix_routing(void)
{
    ProxyTable table = { .lock = PTHREAD_MUTEX_INITIALIZER };
    assert(proxy_add_route(&table, "/api/=127.0.0.1:8001") == 0);
    assert(proxy_add_route(&table, "/api/admin/=127.0.0.1:8003") == 0);
    assert(proxy_add_route(&table, "/static/=127.0.0.1:8002") == 0);
    assert(proxy_find_route(&table, "/api/users") == &table.routes[0]);
    assert(proxy_find_route(&table, "/api/users?id=1") == &table.routes[0]);
    assert(proxy_find_route(&table, "/api/admin/x") == &table.routes[1]);     /* 더 긴 규칙 우선 */
    assert(proxy_find_route(&table, "/static/a.css") == &table.routes[2]);
    assert(proxy_find_route(&table, "/index.html") == NULL);                  /* → 정적 파일 */
    assert(proxy_find_route(&table, "/api") == NULL);                         /* "/api/"와 다름 */
    assert(proxy_find_route(&table, "/x?/api/") == NULL);                     /* 쿼리는 경로가 아님 */
}

static void test_round_robin(void)
{
    ProxyTable table = { .lock = PTHREAD_MUTEX_INITIALIZER };
    assert(proxy_add_route(&table, "/api/=a:1,b:2,c:3") == 0);
    ProxyRoute *route = &table.routes[0];
    size_t expected[] = {0, 1, 2, 0, 1, 2, 0};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        assert(proxy_next_backend(&table, route) == expected[i]);
}

static void parse(const char *text, HttpRequest *request)
{
    size_t consumed;
    int status;
    assert(http_parse_request((const unsigned char *)text, strlen(text), request, &consumed, &status) == HTTP_COMPLETE);
}

static void test_build_request(void)
{
    Backend backend = { "127.0.0.1", "9001" };
    HttpRequest request;
    char output[1024];

    /* hop-by-hop 헤더(Connection, Keep-Alive, Upgrade)는 빼고 나머지는 그대로, Connection: close 추가 */
    parse("POST /api/echo?x=1 HTTP/1.1\r\nHost: example.test\r\nConnection: keep-alive\r\n"
          "Keep-Alive: timeout=5\r\nUpgrade: websocket\r\nContent-Type: text/plain\r\n"
          "Content-Length: 5\r\nX-Custom: yes\r\n\r\nhello", &request);
    int length = proxy_build_request(&request, &backend, output, sizeof(output));
    const char *expected =
        "POST /api/echo?x=1 HTTP/1.1\r\nHost: example.test\r\nContent-Type: text/plain\r\n"
        "Content-Length: 5\r\nX-Custom: yes\r\nConnection: close\r\n\r\n";
    assert(length == (int)strlen(expected));
    assert(!memcmp(output, expected, (size_t)length));

    /* HTTP/1.0 요청에 Host가 없으면 백엔드 주소로 Host를 붙임 */
    parse("GET /api/x HTTP/1.0\r\nAccept: */*\r\n\r\n", &request);
    length = proxy_build_request(&request, &backend, output, sizeof(output));
    expected = "GET /api/x HTTP/1.1\r\nAccept: */*\r\nHost: 127.0.0.1:9001\r\nConnection: close\r\n\r\n";
    assert(length == (int)strlen(expected) && !memcmp(output, expected, (size_t)length));

    /* 출력 공간이 모자라면 -1 (버퍼 넘침 없이) */
    assert(proxy_build_request(&request, &backend, output, 20) == -1);
    assert(proxy_build_request(&request, &backend, output, (size_t)length) == -1);
}

int main(void)
{
    test_rule_parsing();
    test_longest_prefix_routing();
    test_round_robin();
    test_build_request();
    puts("PASS test_proxy: rule parsing, longest-prefix routing, round-robin, request rewrite (hop-by-hop, Host)");
    return 0;
}
