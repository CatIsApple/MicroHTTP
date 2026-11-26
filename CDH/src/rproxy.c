/* rproxy.c — Reverse Proxy (선언·설명은 include/rproxy.h).
 *
 * 요청 하나의 흐름 (워커 스레드):
 *   1. 라우트의 백엔드를 라운드 로빈으로 고르고 connect (실패하면 다음 백엔드로 넘어감 = 단순 failover,
 *      실패한 백엔드는 RPROXY_DOWN_MS 동안 건너뜀 = 수동적 헬스 체크)
 *   2. 나머지(요청 전송, 응답 검사·중계)는 proxy_core 의 pxy_exchange
 *
 * 오류 매핑: 백엔드에 연결 못 함 → 502 (모두 타임아웃이면 504). 그 뒤는 pxy_exchange 를 따른다. */

#define _POSIX_C_SOURCE 200809L /* getaddrinfo, freeaddrinfo 등 POSIX 함수 */

#include "rproxy.h"     /* 이 파일이 구현할 선언 */
#include "proxy_core.h" /* pxy_exchange, PXY_CONNECT_TIMEOUT_MS */
#include "tcp_server.h" /* tcp_connect */

#include <errno.h>      /* errno, ETIMEDOUT */
#include <netdb.h>      /* getaddrinfo, freeaddrinfo, struct addrinfo */
#include <string.h>     /* memcpy, memcmp, memset, strchr, strlen, strncmp, strrchr */
#include <sys/socket.h> /* AF_INET, SOCK_STREAM */
#include <time.h>       /* clock_gettime, CLOCK_MONOTONIC */

static long long mono_ms(void) /* 단조 시계 ms (시스템 시각이 바뀌어도 거꾸로 가지 않음) */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- 라우팅 규칙 ---- */

void rproxy_init(rproxy_table *t)
{
    memset(t, 0, sizeof(*t));
    t->timeout_ms = RPROXY_DEFAULT_TIMEOUT_MS;
}

/* "host:port" (s[0..len)) → be. 이름은 시작할 때 한 번만 해석 (요청마다 DNS 를 부르지 않게) */
static int parse_backend(const char *s, size_t len, rproxy_backend *be, const char **why)
{
    if (len == 0 || len >= RPROXY_NAME_MAX) {
        *why = "backend must be host:port";
        return -1;
    }
    memcpy(be->name, s, len);
    be->name[len] = '\0';
    char *colon = strrchr(be->name, ':');
    if (colon == NULL || colon == be->name || colon[1] == '\0') {
        *why = "backend must be host:port";
        return -1;
    }
    long port = 0;
    for (const char *q = colon + 1; *q; q++) {            /* 포트: 숫자만, 1~65535 */
        if (*q < '0' || *q > '9' || port > 65535) {
            *why = "backend port must be 1-65535";
            return -1;
        }
        port = port * 10 + (*q - '0');
    }
    if (port < 1 || port > 65535) {
        *why = "backend port must be 1-65535";
        return -1;
    }
    char host[RPROXY_NAME_MAX];
    size_t hl = (size_t)(colon - be->name);
    memcpy(host, be->name, hl);
    host[hl] = '\0';

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;                            /* 서버 전체가 IPv4 */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL) {
        *why = "cannot resolve backend host";
        return -1;
    }
    memcpy(&be->addr, res->ai_addr, sizeof(be->addr));    /* 첫 번째 주소 사용 */
    freeaddrinfo(res);
    return 0;
}

int rproxy_add_route(rproxy_table *t, const char *spec, const char **why)
{
    const char *dummy;
    if (why == NULL)
        why = &dummy;
    if (t->nroutes >= RPROXY_MAX_ROUTES) {
        *why = "too many routes";
        return -1;
    }
    const char *eq = strchr(spec, '=');
    if (eq == NULL) {
        *why = "expected prefix=host:port[,host:port...]";
        return -1;
    }
    size_t pl = (size_t)(eq - spec);
    if (pl == 0 || spec[0] != '/' || pl >= RPROXY_PREFIX_MAX) {
        *why = "prefix must start with '/' (max 255 chars)";
        return -1;
    }
    for (size_t i = 0; i < pl; i++) {
        unsigned char ch = (unsigned char)spec[i];
        if (ch <= ' ' || ch == 0x7f) {
            *why = "prefix must not contain spaces or control characters";
            return -1;
        }
    }
    for (int i = 0; i < t->nroutes; i++) {
        if (t->routes[i].prefix_len == pl && memcmp(t->routes[i].prefix, spec, pl) == 0) {
            *why = "duplicate prefix";
            return -1;
        }
    }

    rproxy_route *rt = &t->routes[t->nroutes];            /* 성공할 때만 nroutes 를 늘림 → 실패하면 자리만 덮어씀 */
    memset(rt, 0, sizeof(*rt));
    memcpy(rt->prefix, spec, pl);
    rt->prefix[pl] = '\0';
    rt->prefix_len = pl;

    const char *p = eq + 1;
    if (*p == '\0') {
        *why = "no backend given";
        return -1;
    }
    for (;;) {                                            /* 쉼표로 나뉜 백엔드 목록 */
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (rt->nbackends >= RPROXY_MAX_BACKENDS) {
            *why = "too many backends in one route";
            return -1;
        }
        if (parse_backend(p, len, &rt->backends[rt->nbackends], why) < 0)
            return -1;
        atomic_init(&rt->backends[rt->nbackends].down_until, 0);
        rt->nbackends++;
        if (comma == NULL)
            break;
        p = comma + 1;
    }
    atomic_init(&rt->rr, 0);
    t->nroutes++;
    return 0;
}

/* 경로에 "." / ".." 세그먼트(퍼센트 인코딩 %2e 포함)가 있는지.
 * "/api/../admin" 이 "/api/" 규칙에 걸려 백엔드로 간 뒤 백엔드가 "/admin" 으로 정규화하면
 * 프록시의 라우팅 경계를 우회한다 → 이런 URI 는 프록시로 보내지 않는다 (정적 파일 쪽이 거절함) */
static int has_dot_segment(const char *uri)
{
    const char *p = uri;
    while (*p && *p != '?') {
        if (*p == '/') {
            p++;
            continue;
        }
        int dots = 0, other = 0;                          /* 세그먼트 하나: 점 개수 / 점 아닌 글자 */
        while (*p && *p != '/' && *p != '?') {
            if (*p == '.') {
                dots++;
                p++;
            } else if (p[0] == '%' && p[1] == '2' && (p[2] == 'e' || p[2] == 'E')) {
                dots++;
                p += 3;
            } else {
                other = 1;
                p++;
            }
        }
        if (!other && (dots == 1 || dots == 2))
            return 1;
    }
    return 0;
}

rproxy_route *rproxy_match(rproxy_table *t, const char *uri)
{
    if (t == NULL || uri == NULL || uri[0] != '/')        /* origin-form 만 ("http://..." absolute-form 은 Forward Proxy 몫) */
        return NULL;
    rproxy_route *best = NULL;
    for (int i = 0; i < t->nroutes; i++) {
        rproxy_route *rt = &t->routes[i];
        if (strncmp(uri, rt->prefix, rt->prefix_len) != 0)
            continue;
        char next = uri[rt->prefix_len];                  /* prefix 바로 다음 글자 */
        int boundary = rt->prefix[rt->prefix_len - 1] == '/' || next == '\0' || next == '/' || next == '?';
        if (boundary && (best == NULL || rt->prefix_len > best->prefix_len))
            best = rt;                                    /* 더 긴 prefix 가 이김 */
    }
    if (best != NULL && has_dot_segment(uri))
        return NULL;
    return best;
}

/* ---- 전달 ---- */

int rproxy_handle(const rproxy_table *t, rproxy_route *rt, int cfd, const http_request *req,
                  const char *client_ip, int *keep, int *status, int client_timeout_ms)
{
    *status = 0;
    int resp_to = t->timeout_ms;                                           /* 백엔드 응답 대기 */
    int conn_to = resp_to < PXY_CONNECT_TIMEOUT_MS ? resp_to : PXY_CONNECT_TIMEOUT_MS;

    /* 백엔드 고르기: 라운드 로빈 시작점부터 차례로, connect 가 되는 첫 백엔드.
     * 최근에 connect 가 실패한 백엔드는 건너뜀 (전부 그렇다면 표시를 무시하고 모두 시도) */
    unsigned start = atomic_fetch_add(&rt->rr, 1u);                        /* 여러 워커가 동시에 불러도 안전 */
    unsigned nb = (unsigned)rt->nbackends;
    long long now = mono_ms();
    int alive = 0;
    for (unsigned i = 0; i < nb; i++)
        alive += atomic_load(&rt->backends[i].down_until) <= now;
    int bfd = -1, tried = 0, timeouts = 0;
    rproxy_backend *be = NULL;
    for (unsigned i = 0; i < nb && bfd < 0; i++) {
        rproxy_backend *cand = &rt->backends[(start + i) % nb];
        if (alive > 0 && atomic_load(&cand->down_until) > now)
            continue;                                                      /* 죽음 표시 → 이번엔 건너뜀 */
        tried++;
        bfd = tcp_connect(&cand->addr, conn_to);
        if (bfd >= 0) {
            be = cand;
            atomic_store(&cand->down_until, 0);                            /* 되살아남 */
        } else {
            if (errno == ETIMEDOUT)
                timeouts++;
            atomic_store(&cand->down_until, mono_ms() + RPROXY_DOWN_MS);   /* 한동안 건너뜀 */
        }
    }
    if (bfd < 0)
        return tried > 0 && timeouts == tried ? 504 : 502;                 /* 전부 응답 없음 → 504, 거절 등 → 502 */

    /* URI 는 그대로, Host 는 클라이언트 것 (없으면 백엔드 이름으로) */
    pxy_req_opts o = {.target = req->uri, .client_ip = client_ip, .host = be->name, .replace_host = 0};
    return pxy_exchange(bfd, cfd, req, &o, resp_to, client_timeout_ms, keep, status);
}
