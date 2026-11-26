/* test_fproxy.c — Forward Proxy 순수 함수 단위 테스트:
 * absolute-form / authority-form 대상 쪼개기, 클라이언트 허용 검사, 포워드용 요청 머리 다시 쓰기(Host 교체).
 * 소켓 I/O(fproxy_handle, fproxy_tunnel)는 test_functional 이 실제 목적지로 검사한다. */

#define _POSIX_C_SOURCE 200809L

#include "fproxy.h"      /* 테스트 대상 */
#include "http_parser.h" /* http_parse_request */
#include "proxy_core.h"  /* pxy_build_request (replace_host) */
#include "test.h"        /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <arpa/inet.h>   /* inet_pton, htons */
#include <string.h>      /* strstr, strlen, strcpy, memset */

static void test_is_absolute(void)
{
    CHECK(fproxy_is_absolute("http://a/"));
    CHECK(fproxy_is_absolute("HTTP://a"));                         /* 스킴은 대소문자 무시 */
    CHECK(!fproxy_is_absolute("/index.html"));
    CHECK(!fproxy_is_absolute("https://a/"));
    CHECK(!fproxy_is_absolute("http:/"));
    CHECK(!fproxy_is_absolute(""));
    CHECK(!fproxy_is_absolute(NULL));
}

static void test_parse_absolute(void)
{
    fproxy_target t;
    CHECK_INT(fproxy_parse_absolute("http://example.com/a/b?x=1", &t), 0);
    CHECK_STR(t.host, "example.com");
    CHECK_STR(t.port, "80");
    CHECK_STR(t.authority, "example.com");
    CHECK_STR(t.path, "/a/b?x=1");

    CHECK_INT(fproxy_parse_absolute("http://127.0.0.1:8080/x", &t), 0);
    CHECK_STR(t.host, "127.0.0.1");
    CHECK_STR(t.port, "8080");
    CHECK_STR(t.authority, "127.0.0.1:8080");                     /* 기본 포트가 아니면 Host 에 포트 포함 */
    CHECK_STR(t.path, "/x");

    CHECK_INT(fproxy_parse_absolute("http://h", &t), 0);           /* 경로 없음 → "/" */
    CHECK_STR(t.path, "/");
    CHECK_INT(fproxy_parse_absolute("http://h?q=1", &t), 0);       /* 쿼리만 → "/?q=1" */
    CHECK_STR(t.path, "/?q=1");
    CHECK_INT(fproxy_parse_absolute("http://h:80/", &t), 0);       /* 명시한 기본 포트는 Host 에서 뺌 */
    CHECK_STR(t.authority, "h");
    CHECK_INT(fproxy_parse_absolute("http://h:/", &t), 0);         /* 빈 포트 = 기본 (RFC 3986) */
    CHECK_STR(t.port, "80");
    CHECK_INT(fproxy_parse_absolute("http://h:0080/", &t), 0);     /* 앞의 0 은 정규화 */
    CHECK_STR(t.port, "80");

    const char *bad[] = {
        "https://h/",           /* TLS 는 CONNECT 로 */
        "/a",                   /* origin-form */
        "http:///a",            /* 빈 호스트 */
        "http://:80/",          /* 빈 호스트 */
        "http://u:p@h/",        /* userinfo (자격 증명을 URL 에 — 거절) */
        "http://[::1]/",        /* IPv6 리터럴 (서버가 IPv4 전용) */
        "http://h:0/",          /* 포트 0 */
        "http://h:65536/",      /* 포트 범위 밖 */
        "http://h:8x/",         /* 포트에 글자 */
        "http://h:123456/",     /* 포트 자릿수 */
        "http://h%2e/",         /* 이상한 글자 */
        "http://h/a#frag",      /* fragment */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = fproxy_parse_absolute(bad[i], &t);
        CHECK_INT(r, -1);
        if (r != -1)
            fprintf(stderr, "accepted bad absolute uri: %s\n", bad[i]);
    }
}

static void test_parse_authority(void)
{
    fproxy_target t;
    CHECK_INT(fproxy_parse_authority("example.com:443", &t), 0);
    CHECK_STR(t.host, "example.com");
    CHECK_STR(t.port, "443");
    CHECK_STR(t.authority, "example.com:443");
    CHECK_STR(t.path, "");

    const char *bad[] = {"example.com", "example.com:", ":443", "h:0", "h:99999", "h/x:443", "u@h:443", "[::1]:443", ""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = fproxy_parse_authority(bad[i], &t);
        CHECK_INT(r, -1);
        if (r != -1)
            fprintf(stderr, "accepted bad authority: %s\n", bad[i]);
    }
    CHECK_INT(fproxy_parse_authority(NULL, &t), -1);
}

static void test_client_allowed(void)
{
    fproxy_config cfg;
    fproxy_init(&cfg);
    CHECK_INT(cfg.any_client, 0);
    CHECK_INT(cfg.timeout_ms, FPROXY_DEFAULT_TIMEOUT_MS);
    CHECK(fproxy_client_allowed(&cfg, "127.0.0.1"));
    CHECK(fproxy_client_allowed(&cfg, "127.5.6.7"));
    CHECK(!fproxy_client_allowed(&cfg, "10.0.0.1"));
    CHECK(!fproxy_client_allowed(&cfg, "1.127.0.1"));
    CHECK(!fproxy_client_allowed(&cfg, NULL));
    cfg.any_client = 1;
    CHECK(fproxy_client_allowed(&cfg, "10.0.0.1"));
    CHECK_INT(cfg.max_tunnels, FPROXY_DEFAULT_MAX_TUNNELS);
}

static struct sockaddr_in addr_of(const char *ip, unsigned port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

/* -F any 일 때 목적지 제한 (SSRF 방지). -F local 은 제한 없음 */
static void test_dest_allowed(void)
{
    fproxy_config cfg;
    fproxy_init(&cfg);
    struct sockaddr_in lo = addr_of("127.0.0.1", 6379);
    CHECK(fproxy_dest_allowed(&cfg, &lo, 1));                     /* local: 로컬 사용자는 원래 내부망에 닿음 */

    cfg.any_client = 1;
    const char *blocked[] = {"0.0.0.0", "10.1.2.3", "100.64.0.1", "100.127.255.255", "127.0.0.1", "127.9.9.9",
                             "169.254.169.254", "172.16.0.1", "172.31.255.255", "192.168.0.1", "224.0.0.1",
                             "255.255.255.255"};
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        struct sockaddr_in a = addr_of(blocked[i], 80);
        int r = fproxy_dest_allowed(&cfg, &a, 0);
        CHECK_INT(r, 0);
        if (r != 0)
            fprintf(stderr, "allowed internal dest: %s\n", blocked[i]);
    }
    const char *open[] = {"8.8.8.8", "93.184.216.34", "100.63.255.255", "100.128.0.1", "172.15.0.1", "172.32.0.1",
                          "192.167.0.1", "11.0.0.1"};
    for (size_t i = 0; i < sizeof(open) / sizeof(open[0]); i++) {
        struct sockaddr_in a = addr_of(open[i], 80);
        int r = fproxy_dest_allowed(&cfg, &a, 0);
        CHECK_INT(r, 1);
        if (r != 1)
            fprintf(stderr, "refused public dest: %s\n", open[i]);
    }
    struct sockaddr_in pub443 = addr_of("8.8.8.8", 443), pub25 = addr_of("8.8.8.8", 25);
    CHECK(fproxy_dest_allowed(&cfg, &pub443, 1));                 /* CONNECT 는 443 만 */
    CHECK(!fproxy_dest_allowed(&cfg, &pub25, 1));                 /* SMTP 중계 금지 */
    CHECK(fproxy_dest_allowed(&cfg, &pub25, 0));                  /* absolute-form 은 포트 제한 없음 */
}

/* 포워드용 다시 쓰기: 요청 줄은 origin-form, Host 는 대상 것으로, Proxy-* 헤더 제거 */
static void test_build_forward(void)
{
    char raw[] = "GET http://example.com:8080/a?b HTTP/1.1\r\n"
                 "Host: wrong.example\r\n"
                 "Proxy-Connection: keep-alive\r\n"
                 "Proxy-Authorization: Basic eDp5\r\n"
                 "Accept: */*\r\n"
                 "\r\n";
    http_request req;
    CHECK_INT(http_parse_request(raw, strlen(raw), &req), 0);
    fproxy_target t;
    CHECK_INT(fproxy_parse_absolute(req.uri, &t), 0);
    pxy_req_opts o = {.target = t.path, .client_ip = "127.0.0.1", .host = t.authority, .replace_host = 1};
    char out[1024];
    int n = pxy_build_request(out, sizeof(out), &req, &o);
    CHECK(n > 0);
    CHECK_STR(out, "GET /a?b HTTP/1.1\r\n"
                   "Accept: */*\r\n"
                   "Host: example.com:8080\r\n"
                   "X-Forwarded-For: 127.0.0.1\r\n"
                   "X-Forwarded-Proto: http\r\n"
                   "Connection: close\r\n"
                   "\r\n");
}

int main(void)
{
    test_is_absolute();
    test_parse_absolute();
    test_parse_authority();
    test_client_allowed();
    test_dest_allowed();
    test_build_forward();
    return TEST_REPORT();
}
