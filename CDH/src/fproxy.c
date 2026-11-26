/* fproxy.c — Forward Proxy (선언·설명은 include/fproxy.h).
 *
 * absolute-form ("GET http://host/a"):
 *   1. 대상 쪼개기 (host, port, Host 값, origin-form 경로) → 형식이 틀리면 400
 *   2. 이름 해석(별도 스레드, 마감 있음) → 목적지 검사(-F any 면 내부 주소 금지, 403) → connect
 *      → 실패하면 502, 시간 초과면 504, 해석 스레드가 너무 많으면 503
 *   3. 요청 줄을 "GET /a" 로, Host 를 대상 것으로 바꿔 pxy_exchange 로 주고받기
 *
 * CONNECT ("CONNECT host:443"):
 *   동시 터널 수가 상한이면 503. 1~2 는 같고 (-F any 면 포트 443 만), 연결되면 "200 Connection Established" 를 보낸 뒤
 *   poll 로 양쪽을 지켜보며 받은 바이트를 반대쪽으로 그대로 보낸다 (TLS 내용은 해석하지 않음).
 *   한쪽이 송신을 끝내면(FIN) 반대쪽에도 송신 끝(shutdown SHUT_WR)을 전하고, 양쪽 다 끝나면 닫는다. */

#define _POSIX_C_SOURCE 200809L /* getaddrinfo, freeaddrinfo, poll 등 POSIX 함수 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* macOS: MSG_DONTWAIT */
#endif

#include "fproxy.h"     /* 이 파일이 구현할 선언 */
#include "proxy_core.h" /* pxy_exchange, pxy_req_opts, PXY_CONNECT_TIMEOUT_MS */
#include "tcp_server.h" /* tcp_connect, tcp_send_all */

#include <arpa/inet.h>  /* inet_pton, ntohl, htons */
#include <errno.h>      /* errno, ETIMEDOUT, EINTR, EAGAIN */
#include <netdb.h>      /* getaddrinfo, freeaddrinfo */
#include <netinet/in.h> /* struct sockaddr_in */
#include <poll.h>       /* poll */
#include <pthread.h>    /* 이름 해석 스레드 */
#include <stdio.h>      /* snprintf */
#include <stdlib.h>     /* calloc, free, strtol */
#include <time.h>       /* clock_gettime (cond 대기 마감) */
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
    cfg->max_tunnels = FPROXY_DEFAULT_MAX_TUNNELS;
    atomic_init(&cfg->tunnels, 0);
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

int fproxy_dest_allowed(const fproxy_config *cfg, const struct sockaddr_in *addr, int is_connect)
{
    if (!cfg->any_client)                                 /* 로컬 사용자만 쓰는 프록시: 제한 없음 */
        return 1;
    uint32_t ip = ntohl(addr->sin_addr.s_addr);
    unsigned a = ip >> 24, b = (ip >> 16) & 0xff;
    if (a == 0 || a == 10 || a == 127 || a >= 224 ||      /* "이 호스트", 사설 10/8, 루프백, 멀티캐스트·예약 */
        (a == 100 && b >= 64 && b <= 127) ||              /* CGNAT 100.64/10 */
        (a == 169 && b == 254) ||                         /* 링크 로컬 (클라우드 메타데이터 169.254.169.254 포함) */
        (a == 172 && b >= 16 && b <= 31) ||               /* 사설 172.16/12 */
        (a == 192 && b == 168))                           /* 사설 192.168/16 */
        return 0;
    if (is_connect && ntohs(addr->sin_port) != 443)       /* 터널은 HTTPS 만 (SMTP 등 임의 포트 중계 금지) */
        return 0;
    return 1;
}

/* ---- 마감 시간이 있는 이름 해석 ----
 * getaddrinfo 는 마감을 줄 수 없어서 별도 스레드에서 돌리고, 워커는 cond 로 정해진 시간만 기다린다.
 * 시간이 지나면 워커는 포기하고 떠나며, 해석 스레드는 끝나는 대로 스스로 정리한다 (refs 가 0 이 되는 쪽이 free).
 * 해석 스레드 수는 FPROXY_MAX_DNS 로 묶는다 (느린 DNS 로 스레드가 끝없이 늘지 않게). */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;                 /* 해석 끝남 */
    int refs;                 /* 워커 + 해석 스레드 (0 이 되면 free) */
    int ok;                   /* 해석 성공 */
    struct sockaddr_in addr;  /* 결과 */
    char host[FPROXY_HOST_MAX];
    char port[6];
} dns_job;

static atomic_int g_dns_busy; /* 진행 중인 해석 스레드 수 (정적 변수라 0 으로 시작) */

static void dns_release(dns_job *j)
{
    pthread_mutex_lock(&j->mu);
    int left = --j->refs;
    pthread_mutex_unlock(&j->mu);
    if (left == 0) {
        pthread_cond_destroy(&j->cv);
        pthread_mutex_destroy(&j->mu);
        free(j);
    }
}

static void *dns_thread(void *arg)
{
    dns_job *j = arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;                            /* 서버 전체가 IPv4 */
    hints.ai_socktype = SOCK_STREAM;
    int ok = getaddrinfo(j->host, j->port, &hints, &res) == 0 && res != NULL;
    pthread_mutex_lock(&j->mu);
    if (ok)
        memcpy(&j->addr, res->ai_addr, sizeof(j->addr)); /* 첫 번째 주소 */
    j->ok = ok;
    j->done = 1;
    pthread_cond_signal(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (res != NULL)
        freeaddrinfo(res);
    atomic_fetch_sub(&g_dns_busy, 1);
    dns_release(j);
    return NULL;
}

/* host:port → out. 반환: 0 성공 / -502 해석 실패 / -503 해석 스레드가 너무 많음 / -504 시간 초과 */
static int resolve(const char *host, const char *port, struct sockaddr_in *out, int timeout_ms)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)strtol(port, NULL, 10));
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1)    /* 숫자 IPv4 → DNS 필요 없음 */
        return 0;

    if (atomic_fetch_add(&g_dns_busy, 1) >= FPROXY_MAX_DNS) {
        atomic_fetch_sub(&g_dns_busy, 1);
        return -503;
    }
    dns_job *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        atomic_fetch_sub(&g_dns_busy, 1);
        return -502;
    }
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    j->refs = 2;
    snprintf(j->host, sizeof(j->host), "%s", host);
    snprintf(j->port, sizeof(j->port), "%s", port);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int cr = pthread_create(&th, &attr, dns_thread, j);
    pthread_attr_destroy(&attr);
    if (cr != 0) {                                        /* 스레드를 못 만듦 → 정리하고 실패 */
        atomic_fetch_sub(&g_dns_busy, 1);
        j->refs = 1;
        dns_release(j);
        return -502;
    }

    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);                   /* pthread_cond_timedwait 는 기본이 REALTIME 기준 */
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&j->mu);
    while (!j->done)
        if (pthread_cond_timedwait(&j->cv, &j->mu, &dl) == ETIMEDOUT)
            break;
    int done = j->done, ok = j->ok;
    if (ok)
        memcpy(out, &j->addr, sizeof(*out));
    pthread_mutex_unlock(&j->mu);
    dns_release(j);                                       /* 늦게 끝나는 해석 스레드가 마지막에 free */
    if (!done)
        return -504;
    return ok ? 0 : -502;
}

/* 대상에 연결. 반환: fd, 실패면 -(상태 코드) (-403 / -502 / -503 / -504) */
static int connect_target(const fproxy_config *cfg, const fproxy_target *t, int is_connect)
{
    struct sockaddr_in addr;
    int dns_to = cfg->timeout_ms < FPROXY_DNS_TIMEOUT_MS ? cfg->timeout_ms : FPROXY_DNS_TIMEOUT_MS;
    int r = resolve(t->host, t->port, &addr, dns_to);
    if (r < 0)
        return r;
    if (!fproxy_dest_allowed(cfg, &addr, is_connect))     /* 해석한 뒤에 검사 (이름으로 내부 주소를 가리켜도 막힘) */
        return -403;
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
    int bfd = connect_target(cfg, &t, 0);
    if (bfd < 0)
        return -bfd;
    pxy_req_opts o = {.target = t.path,
                      .client_ip = client_ip,
                      .host = t.authority,
                      .replace_host = 1,
                      .min_rate_bps = FPROXY_MIN_RATE_BPS,       /* 믿을 수 없는 목적지: 너무 느리면 끊음 */
                      .rate_grace_ms = cfg->timeout_ms};
    return pxy_exchange(bfd, cfd, req, &o, cfg->timeout_ms, client_timeout_ms, keep, status);
}

static int tunnel_run(const fproxy_config *cfg, int cfd, const fproxy_target *t, const char *pre, size_t pre_len,
                      int client_timeout_ms, int (*should_stop)(void *), void *arg, unsigned long long *bytes);

int fproxy_tunnel(fproxy_config *cfg, int cfd, const http_request *req, const char *pre, size_t pre_len,
                  int client_timeout_ms, int (*should_stop)(void *), void *arg, unsigned long long *bytes)
{
    *bytes = 0;
    fproxy_target t;
    if (fproxy_parse_authority(req->uri, &t) < 0)
        return 400;
    if (atomic_fetch_add(&cfg->tunnels, 1) >= cfg->max_tunnels) { /* 터널이 워커를 다 차지하지 못하게 */
        atomic_fetch_sub(&cfg->tunnels, 1);
        return 503;
    }
    int r = tunnel_run(cfg, cfd, &t, pre, pre_len, client_timeout_ms, should_stop, arg, bytes);
    atomic_fetch_sub(&cfg->tunnels, 1);
    return r;
}

static int tunnel_run(const fproxy_config *cfg, int cfd, const fproxy_target *t, const char *pre, size_t pre_len,
                      int client_timeout_ms, int (*should_stop)(void *), void *arg, unsigned long long *bytes)
{
    int bfd = connect_target(cfg, t, 1);
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
