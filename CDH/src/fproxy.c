/* fproxy.c — Forward Proxy (선언·설명은 include/fproxy.h).
 *
 * absolute-form ("GET http://host/a"):
 *   1. 대상 쪼개기 (host, port, Host 값, origin-form 경로) → 형식이 틀리면 400
 *   2. 이름 해석(getaddrinfo) + connect → 실패하면 502, 시간 초과면 504
 *   3. 요청 줄을 "GET /a" 로, Host 를 대상 것으로 바꿔 pxy_exchange 로 주고받기
 *
 * CONNECT ("CONNECT host:443"):
 *   1~2 는 같고, 연결되면 "200 Connection Established" 를 보낸 뒤
 *   poll 로 양쪽을 지켜보며 받은 바이트를 반대쪽으로 그대로 보낸다 (TLS 내용은 해석하지 않음).
 *   한쪽이 송신을 끝내면(FIN) 반대쪽에도 송신 끝(shutdown SHUT_WR)을 전하고, 양쪽 다 끝나면 닫는다. */

#define _POSIX_C_SOURCE 200809L /* getaddrinfo, freeaddrinfo, poll 등 POSIX 함수 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* macOS: MSG_DONTWAIT */
#endif

#include "fproxy.h"     /* 이 파일이 구현할 선언 */
#include "proxy_core.h" /* pxy_exchange, pxy_req_opts, PXY_CONNECT_TIMEOUT_MS */
#include "tcp_server.h" /* tcp_connect, tcp_send_all */

#include <errno.h>      /* errno, ETIMEDOUT, EINTR, EAGAIN */
#include <netdb.h>      /* getaddrinfo, freeaddrinfo */
#include <netinet/in.h> /* struct sockaddr_in */
#include <poll.h>       /* poll */
#include <stdio.h>      /* snprintf */
#include <string.h>     /* memcpy, memset, strlen, strncmp, strrchr */
#include <sys/socket.h> /* recv, shutdown, AF_INET */
#include <unistd.h>     /* close */

#define FPROXY_TUNNEL_BUF 16384 /* 터널 중계 버퍼 (방향마다 따로 쓰지 않고 하나를 돌려 씀) */
#define FPROXY_POLL_SLICE_MS 500 /* 터널 poll 한 번의 최대 대기 (그 사이 종료 신호·무활동 시간 확인) */

static const char RESP_200_TUNNEL[] = "HTTP/1.1 200 Connection Established\r\n\r\n";

void fproxy_init(fproxy_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->any_client = 0;
    cfg->timeout_ms = FPROXY_DEFAULT_TIMEOUT_MS;
    cfg->tunnel_idle_ms = FPROXY_TUNNEL_IDLE_MS;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int fproxy_is_absolute(const char *uri)
{
    static const char P[] = "http://";
    if (uri == NULL)
        return 0;
    for (size_t i = 0; i < sizeof(P) - 1; i++)
        if (lower(uri[i]) != P[i])                        /* uri 가 짧으면 '\0' 에서 다름 → 0 */
            return 0;
    return 1;
}

/* 호스트 이름에 쓸 수 있는 글자 (영문·숫자·'-'·'.'·'_'). IPv4 숫자 주소도 여기에 들어감 */
static int host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_';
}

/* "host[:port]" (s[0..len)) → t->host, t->port, t->authority.
 * port_required: CONNECT 는 포트 필수. 생략하면 80 */
static int parse_hostport(const char *s, size_t len, int port_required, fproxy_target *t)
{
    size_t hl = len;                                      /* 호스트 길이 */
    const char *port = NULL;
    size_t pl = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ':') {                                /* 첫 ':' 뒤가 포트 (IPv6 는 '[' 에서 이미 거절됨) */
            hl = i;
            port = s + i + 1;
            pl = len - i - 1;
            break;
        }
    }
    if (hl == 0 || hl >= FPROXY_HOST_MAX)
        return -1;
    for (size_t i = 0; i < hl; i++)
        if (!host_char(s[i]))                             /* '@'(userinfo), '['(IPv6), '%' 등 거절 */
            return -1;
    if (port == NULL || pl == 0) {                        /* 포트 생략 ("host" / "host:") */
        if (port_required)
            return -1;
        memcpy(t->port, "80", 3);
    } else {
        if (pl > 5)
            return -1;
        long v = 0;
        for (size_t i = 0; i < pl; i++) {
            if (port[i] < '0' || port[i] > '9')
                return -1;
            v = v * 10 + (port[i] - '0');
        }
        if (v < 1 || v > 65535)
            return -1;
        snprintf(t->port, sizeof(t->port), "%ld", v);     /* "080" → "80" 처럼 정규화 */
    }
    memcpy(t->host, s, hl);
    t->host[hl] = '\0';
    if (port != NULL && pl > 0 && strcmp(t->port, "80") != 0)
        snprintf(t->authority, sizeof(t->authority), "%s:%s", t->host, t->port);
    else
        snprintf(t->authority, sizeof(t->authority), "%s", t->host); /* 기본 포트는 Host 에 안 적음 */
    return 0;
}

int fproxy_parse_absolute(const char *uri, fproxy_target *t)
{
    memset(t, 0, sizeof(*t));
    if (!fproxy_is_absolute(uri))
        return -1;
    const char *a = uri + 7;                              /* authority 시작 */
    size_t al = 0;
    while (a[al] != '\0' && a[al] != '/' && a[al] != '?' && a[al] != '#')
        al++;
    if (parse_hostport(a, al, 0, t) < 0)
        return -1;
    const char *rest = a + al;                            /* "/path?q", "?q", "" */
    if (strchr(rest, '#') != NULL)                        /* fragment 는 요청에 오면 안 됨 */
        return -1;
    int n = snprintf(t->path, sizeof(t->path), "%s%s", rest[0] == '/' ? "" : "/", rest);
    if (n < 0 || (size_t)n >= sizeof(t->path))
        return -1;
    return 0;
}

int fproxy_parse_authority(const char *uri, fproxy_target *t)
{
    memset(t, 0, sizeof(*t));
    if (uri == NULL)
        return -1;
    return parse_hostport(uri, strlen(uri), 1, t);
}

int fproxy_client_allowed(const fproxy_config *cfg, const char *client_ip)
{
    if (cfg->any_client)
        return 1;
    return client_ip != NULL && strncmp(client_ip, "127.", 4) == 0; /* 루프백 대역 127.0.0.0/8 */
}

/* 대상에 연결. 반환: fd, 실패면 -(상태 코드) (-502 / -504) */
static int connect_target(const fproxy_config *cfg, const fproxy_target *t)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;                            /* 서버 전체가 IPv4 */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(t->host, t->port, &hints, &res) != 0 || res == NULL)
        return -502;                                      /* 이름 해석 실패 */
    struct sockaddr_in addr;
    memcpy(&addr, res->ai_addr, sizeof(addr));            /* 첫 번째 주소 */
    freeaddrinfo(res);
    int conn_to = cfg->timeout_ms < PXY_CONNECT_TIMEOUT_MS ? cfg->timeout_ms : PXY_CONNECT_TIMEOUT_MS;
    int fd = tcp_connect(&addr, conn_to);
    if (fd < 0)
        return errno == ETIMEDOUT ? -504 : -502;
    return fd;
}

int fproxy_handle(const fproxy_config *cfg, int cfd, const http_request *req, const char *client_ip, int *keep,
                  int *status, int client_timeout_ms)
{
    *status = 0;
    fproxy_target t;
    if (fproxy_parse_absolute(req->uri, &t) < 0)
        return 400;
    int bfd = connect_target(cfg, &t);
    if (bfd < 0)
        return -bfd;
    pxy_req_opts o = {.target = t.path, .client_ip = client_ip, .host = t.authority, .replace_host = 1};
    return pxy_exchange(bfd, cfd, req, &o, cfg->timeout_ms, client_timeout_ms, keep, status);
}

int fproxy_tunnel(const fproxy_config *cfg, int cfd, const http_request *req, const char *pre, size_t pre_len,
                  int client_timeout_ms, int (*should_stop)(void *), void *arg, unsigned long long *bytes)
{
    *bytes = 0;
    fproxy_target t;
    if (fproxy_parse_authority(req->uri, &t) < 0)
        return 400;
    int bfd = connect_target(cfg, &t);
    if (bfd < 0)
        return -bfd;
    if (tcp_send_all(cfd, RESP_200_TUNNEL, sizeof(RESP_200_TUNNEL) - 1, client_timeout_ms) < 0) {
        close(bfd);
        return -1;
    }
    if (pre_len > 0) {                                    /* CONNECT 뒤에 미리 온 바이트 (TLS ClientHello 등) */
        if (tcp_send_all(bfd, pre, pre_len, client_timeout_ms) < 0) {
            close(bfd);
            return 0;                                     /* 200 은 이미 보냄 → 그냥 닫기 */
        }
        *bytes += pre_len;
    }

    /* 양방향 중계: [0] 클라이언트 → 목적지, [1] 목적지 → 클라이언트 */
    int src[2] = {cfd, bfd}, dst[2] = {bfd, cfd};
    int open[2] = {1, 1};                                 /* 이 방향의 송신 쪽이 아직 안 끝남 */
    int idle = 0;                                         /* 연속으로 조용했던 시간 */
    char buf[FPROXY_TUNNEL_BUF];
    while (open[0] || open[1]) {
        struct pollfd p[2] = {
            {.fd = open[0] ? cfd : -1, .events = POLLIN}, /* fd -1 = 이 방향은 끝났으니 안 봄 (POLLHUP 무한 반복 방지) */
            {.fd = open[1] ? bfd : -1, .events = POLLIN},
        };
        int r = poll(p, 2, FPROXY_POLL_SLICE_MS);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0) {                                     /* 조용함 */
            idle += FPROXY_POLL_SLICE_MS;
            if (idle >= cfg->tunnel_idle_ms || (should_stop != NULL && should_stop(arg)))
                break;
            continue;
        }
        idle = 0;
        int fail = 0;
        for (int i = 0; i < 2 && !fail; i++) {
            if (!open[i] || p[i].revents == 0)
                continue;
            ssize_t n = recv(src[i], buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                if (tcp_send_all(dst[i], buf, (size_t)n, client_timeout_ms) < 0)
                    fail = 1;                             /* 반대쪽이 끊김·멈춤 */
                else
                    *bytes += (unsigned long long)n;
            } else if (n == 0) {                          /* 이 방향 송신 끝 → 반대쪽에도 FIN 전달 */
                shutdown(dst[i], SHUT_WR);
                open[i] = 0;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fail = 1;                                 /* ECONNRESET 등 */
            }
        }
        if (fail)
            break;
        if (should_stop != NULL && should_stop(arg))
            break;
    }
    close(bfd);
    return 0;
}
