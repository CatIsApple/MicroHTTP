/* proxy_core.c — 프록시 공통 부품 (선언·설명은 include/proxy_core.h).
 *
 * pxy_exchange 의 흐름 (워커 스레드, 블로킹 + 마감 시간). connect 는 호출자(rproxy / fproxy)가 해 둔 상태:
 *   1. 요청 머리를 다시 써서(hop-by-hop 제거, X-Forwarded-For 추가, Connection: close) 본문과 함께 전송
 *   2. 응답 머리 수신 (1xx 는 건너뜀) → 검사 → 클라이언트용으로 다시 써서 전송
 *   3. 본문 중계: Content-Length 면 정확히 그만큼 (클라이언트 keep-alive 유지 가능),
 *      chunked / 길이 없음이면 상대가 닫을 때까지 그대로 흘려보내고 클라이언트도 닫는다
 *
 * 오류 매핑: 응답 머리 전에 타임아웃 → 504 / 응답이 HTTP 가 아님·너무 큼·도중에 닫힘 → 502.
 *           클라이언트에 한 바이트라도 보낸 뒤의 실패는 연결을 끊는다. */

#define _POSIX_C_SOURCE 200809L

#include "proxy_core.h" /* 이 파일이 구현할 선언 */
#include "tcp_server.h" /* tcp_sendv_all, tcp_send_all, tcp_recv_request, tcp_recv_some */

#include <errno.h>      /* errno, ETIMEDOUT */
#include <string.h>     /* memcpy, memcmp, memmove, memset, memchr, strlen */
#include <sys/uio.h>    /* struct iovec */
#include <unistd.h>     /* close */

#define PXY_REQ_HEAD_MAX 12288 /* 상대로 보낼 요청 머리 버퍼 (클라이언트 헤더 최대 8KB + 추가 헤더) */
#define PXY_RESP_HEAD_MAX 8192 /* 상대 응답 머리 최대 크기 (넘으면 502) */
#define PXY_RELAY_BUF 16384    /* 본문 중계 버퍼 */

/* ---- 작은 도우미 ---- */

static char lower(char c) /* ASCII 소문자로 (헤더 이름 비교용, 로캘 무관) */
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* a[0..an) 과 '\0' 종료 문자열 b 가 대소문자 무시하고 같은지 */
static int ieq_n(const char *a, size_t an, const char *b)
{
    size_t i = 0;
    for (; i < an && b[i] != '\0'; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return i == an && b[i] == '\0';
}

static int ieq(const char *a, const char *b) /* 두 '\0' 종료 문자열이 대소문자 무시하고 같은지 */
{
    return ieq_n(a, strlen(a), b);
}

/* 쉼표로 나뉜 목록 list[0..len) 에 토큰 name(길이 nlen) 이 있는지 (앞뒤 공백 무시, 대소문자 무시). Connection 헤더용 */
static int list_has(const char *list, size_t len, const char *name, size_t nlen)
{
    size_t i = 0;
    while (i < len) {
        while (i < len && (list[i] == ' ' || list[i] == '\t' || list[i] == ','))
            i++;                                          /* 구분자·공백 건너뜀 */
        size_t s = i;
        while (i < len && list[i] != ',')
            i++;                                          /* 토큰 끝 */
        size_t e = i;
        while (e > s && (list[e - 1] == ' ' || list[e - 1] == '\t'))
            e--;                                          /* 뒤 공백 제거 */
        if (e - s == nlen) {
            size_t k = 0;
            while (k < nlen && lower(list[s + k]) == lower(name[k]))
                k++;
            if (k == nlen)
                return 1;
        }
    }
    return 0;
}

/* 요청·응답 공통 hop-by-hop 헤더 (RFC 7230 6.1): 한 구간(클라이언트↔프록시, 프록시↔상대)에서만 의미가 있어 넘기지 않는다 */
static int is_hop_by_hop(const char *name, size_t nlen)
{
    static const char *const HOP[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Upgrade"};
    for (size_t i = 0; i < sizeof(HOP) / sizeof(HOP[0]); i++)
        if (ieq_n(name, nlen, HOP[i]))
            return 1;
    return 0;
}

/* 경계 검사가 있는 출력 버퍼 (넘치면 err 만 켜고 더 쓰지 않음 → 마지막에 한 번만 확인) */
typedef struct {
    char *p;    /* 출력 버퍼 */
    size_t cap; /* 크기 ('\0' 자리 1 포함) */
    size_t len; /* 지금까지 쓴 길이 */
    int err;    /* 넘침 */
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->err || n > b->cap - 1 - b->len) { /* '\0' 자리 남김 */
        b->err = 1;
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

/* ---- 요청 머리 ---- */

/* 요청의 Connection 헤더 목록에 name 이 있는지 ("Connection: close, X-Foo" 면 X-Foo 도 hop-by-hop) */
static int req_conn_lists(const http_request *req, const char *name)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i < req->header_count; i++)
        if (ieq(req->headers[i].name, "Connection") &&
            list_has(req->headers[i].value, strlen(req->headers[i].value), name, nlen))
            return 1;
    return 0;
}

int pxy_build_request(char *out, size_t cap, const http_request *req, const pxy_req_opts *o)
{
    if (cap == 0)
        return -1;
    sbuf b = {.p = out, .cap = cap, .len = 0, .err = 0};

    /* 요청 줄: 메서드 그대로, 경로는 target, 버전은 클라이언트와 같게 (1.0 클라이언트면 상대도 chunked 로 답하지 않음) */
    sb_str(&b, req->method_str);
    sb_str(&b, " ");
    sb_str(&b, o->target);
    sb_str(&b, req->version_minor == 0 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n");

    for (size_t i = 0; i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        size_t nlen = strlen(name);
        if (is_hop_by_hop(name, nlen) || ieq(name, "Transfer-Encoding") || /* 본문은 Content-Length 로만 보냄 */
            ieq(name, "Expect") ||                                        /* 100-continue 는 이미 우리가 처리함 */
            ieq(name, "X-Forwarded-For") || ieq(name, "X-Forwarded-Proto") || /* 아래에서 우리가 씀 */
            ieq(name, "Proxy-Authorization") ||                           /* 프록시에게 준 자격 증명: 상대에게 새면 안 됨 */
            (o->replace_host && ieq(name, "Host")) ||                     /* 포워드: Host 는 요청 대상으로 바꿈 */
            req_conn_lists(req, name))
            continue;
        sb_str(&b, name);
        sb_str(&b, ": ");
        sb_str(&b, req->headers[i].value);
        sb_str(&b, "\r\n");
    }
    if (req->host == NULL || o->replace_host) {           /* Host 가 없거나(HTTP/1.0) 바꿔야 함(포워드) → o->host */
        sb_str(&b, "Host: ");
        sb_str(&b, o->host);
        sb_str(&b, "\r\n");
    }

    /* X-Forwarded-For: 앞 프록시들이 남긴 값 뒤에 이번 클라이언트 IP 를 덧붙인다 */
    sb_str(&b, "X-Forwarded-For: ");
    for (size_t i = 0; i < req->header_count; i++) {
        if (ieq(req->headers[i].name, "X-Forwarded-For") && req->headers[i].value[0] != '\0') {
            sb_str(&b, req->headers[i].value);
            sb_str(&b, ", ");
        }
    }
    sb_str(&b, o->client_ip);
    sb_str(&b, "\r\nX-Forwarded-Proto: http\r\n");
    sb_str(&b, "Connection: close\r\n\r\n");              /* 상대 연결은 요청 하나로 끝 → 응답 끝 = 상대가 닫을 때도 성립 */
    return b.err ? -1 : (int)b.len;
}

/* ---- 응답 머리 ---- */

/* buf[pos..) 에서 다음 줄의 끝(CR 위치)을 찾는다. 없으면 head_len */
static size_t line_end(const char *buf, size_t pos, size_t head_len)
{
    while (pos + 1 < head_len && !(buf[pos] == '\r' && buf[pos + 1] == '\n'))
        pos++;
    return pos + 1 < head_len ? pos : head_len;
}

/* 헤더 줄 [s, e) 를 이름/값으로 나눈다. 값은 앞뒤 공백 제거. 형식이 틀리면 -1 */
static int split_header(const char *s, const char *e, const char **name, size_t *nlen, const char **val,
                        size_t *vlen)
{
    if (s == e || *s == ' ' || *s == '\t')                /* 빈 이름 / obs-fold (줄 이어 쓰기, RFC 7230 이 금지) */
        return -1;
    const char *colon = memchr(s, ':', (size_t)(e - s));
    if (colon == NULL || colon == s)
        return -1;
    for (const char *q = s; q < colon; q++)
        if (*q == ' ' || *q == '\t')                      /* "Name : v" 처럼 이름에 공백 */
            return -1;
    const char *v = colon + 1, *ve = e;
    while (v < ve && (*v == ' ' || *v == '\t'))
        v++;
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
        ve--;
    *name = s;
    *nlen = (size_t)(colon - s);
    *val = v;
    *vlen = (size_t)(ve - v);
    return 0;
}

int pxy_parse_response_head(const char *buf, size_t head_len, pxy_resp_info *info)
{
    memset(info, 0, sizeof(*info));
    /* 상태 줄: "HTTP/1.x DDD[ 이유]" */
    if (head_len < 16 || memcmp(buf, "HTTP/1.", 7) != 0 || (buf[7] != '0' && buf[7] != '1') || buf[8] != ' ')
        return -1;
    for (int i = 9; i < 12; i++)
        if (buf[i] < '0' || buf[i] > '9')
            return -1;
    if (buf[12] != ' ' && buf[12] != '\r')
        return -1;
    info->status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    if (info->status < 100 || info->status > 599)
        return -1;

    size_t pos = 0;
    int first = 1;
    for (;;) {
        size_t e = line_end(buf, pos, head_len);
        if (e >= head_len)                                /* 빈 줄 없이 끝남 (호출자가 준 head_len 이 틀림) */
            return -1;
        if (e == pos)                                     /* 빈 줄 = 머리 끝 */
            break;
        for (size_t k = pos; k < e; k++)                  /* 줄 안의 CR / LF / NUL → 응답 분할 위험 */
            if (buf[k] == '\r' || buf[k] == '\n' || buf[k] == '\0')
                return -1;
        if (!first) {
            const char *name, *val;
            size_t nlen, vlen;
            if (split_header(buf + pos, buf + e, &name, &nlen, &val, &vlen) < 0)
                return -1;
            if (ieq_n(name, nlen, "Content-Length")) {
                if (vlen == 0 || vlen > 19)               /* 비었거나 너무 큼 (unsigned long long 안에서 계산) */
                    return -1;
                unsigned long long v = 0;
                for (size_t k = 0; k < vlen; k++) {
                    if (val[k] < '0' || val[k] > '9')
                        return -1;
                    v = v * 10 + (unsigned)(val[k] - '0');
                }
                if (info->has_length && v != info->length) /* 서로 다른 값 → 본문 경계가 모호 */
                    return -1;
                info->has_length = 1;
                info->length = v;
            } else if (ieq_n(name, nlen, "Transfer-Encoding")) {
                info->has_te = 1;
                const char *last = val;                   /* 목록의 마지막 코딩만 본다 (chunked 는 항상 마지막) */
                for (size_t k = 0; k < vlen; k++)
                    if (val[k] == ',')
                        last = val + k + 1;
                while (last < val + vlen && (*last == ' ' || *last == '\t'))
                    last++;
                info->chunked = ieq_n(last, (size_t)(val + vlen - last), "chunked");
            }
        }
        first = 0;
        pos = e + 2;
    }
    if (info->has_te)                                     /* TE 가 있으면 Content-Length 는 무시 (RFC 7230 3.3.3) */
        info->has_length = 0;
    return 0;
}

/* 응답의 Connection 헤더 목록에 name(길이 nlen) 이 있는지 */
static int resp_conn_lists(const char *buf, size_t head_len, const char *name, size_t nlen)
{
    size_t pos = line_end(buf, 0, head_len) + 2;          /* 상태 줄 다음부터 */
    for (;;) {
        size_t e = line_end(buf, pos, head_len);
        if (e >= head_len || e == pos)
            return 0;
        const char *n, *v;
        size_t nl, vl;
        if (split_header(buf + pos, buf + e, &n, &nl, &v, &vl) == 0 && ieq_n(n, nl, "Connection") &&
            list_has(v, vl, name, nlen))
            return 1;
        pos = e + 2;
    }
}

int pxy_rewrite_response_head(char *out, size_t cap, const char *buf, size_t head_len, int keep_alive)
{
    if (cap == 0 || head_len < 16)
        return -1;
    sbuf b = {.p = out, .cap = cap, .len = 0, .err = 0};
    size_t e = line_end(buf, 0, head_len);
    if (e >= head_len)
        return -1;
    sb_str(&b, "HTTP/1.1");                               /* 우리는 1.1 로 말한다. 코드·문구는 상대 것 그대로 */
    sb_put(&b, buf + 8, e - 8);
    sb_str(&b, "\r\n");
    size_t pos = e + 2;
    for (;;) {
        e = line_end(buf, pos, head_len);
        if (e >= head_len)
            return -1;
        if (e == pos)
            break;
        const char *name, *val;
        size_t nlen, vlen;
        if (split_header(buf + pos, buf + e, &name, &nlen, &val, &vlen) < 0)
            return -1;
        if (!is_hop_by_hop(name, nlen) && !resp_conn_lists(buf, head_len, name, nlen))
            sb_put(&b, buf + pos, e - pos + 2);           /* 줄 그대로 (CRLF 포함) */
        pos = e + 2;
    }
    sb_str(&b, keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
    return b.err ? -1 : (int)b.len;
}

/* ---- 요청 하나 주고받기 ---- */

int pxy_exchange(int bfd, int cfd, const http_request *req, const pxy_req_opts *o, int backend_timeout_ms,
                 int client_timeout_ms, int *keep, int *status)
{
    *status = 0;
    int resp_to = backend_timeout_ms;

    /* 1. 요청 전송: 다시 쓴 머리 + 이미 받아 둔 본문 */
    char head[PXY_REQ_HEAD_MAX];
    int hl = pxy_build_request(head, sizeof(head), req, o);
    if (hl < 0) {
        close(bfd);
        return 500;                                                        /* 우리 버퍼가 모자람 (사실상 없음) */
    }
    struct iovec iov[2] = {
        {.iov_base = head, .iov_len = (size_t)hl},
        {.iov_base = (void *)req->body, .iov_len = req->body_len},
    };
    if (tcp_sendv_all(bfd, iov, 2, resp_to) < 0) {
        int e = errno;
        close(bfd);
        return e == ETIMEDOUT ? 504 : 502;
    }

    /* 2. 응답 머리 (1xx 정보 응답은 버리고 다음 머리를 기다림) */
    char rbuf[PXY_RESP_HEAD_MAX];
    size_t used = 0, hlen = 0;
    ssize_t n;
    pxy_resp_info info;
    for (;;) {
        n = tcp_recv_request(bfd, rbuf, sizeof(rbuf), used, resp_to, &hlen);
        if (n < 0) {
            int e = errno;
            close(bfd);
            return e == ETIMEDOUT ? 504 : 502;                             /* 상대가 제때 답하지 않음 */
        }
        if (hlen == 0 || pxy_parse_response_head(rbuf, hlen, &info) < 0) {
            close(bfd);                                                    /* 머리 전에 닫힘 / 너무 큼 / HTTP 아님 */
            return 502;
        }
        if (info.status >= 200)
            break;
        if (info.status == 101) {                                          /* Upgrade 는 지원 안 함 (Upgrade 헤더도 안 넘김) */
            close(bfd);
            return 502;
        }
        used = (size_t)n - hlen;                                           /* 100 Continue 등: 뒤에 온 바이트만 남김 */
        memmove(rbuf, rbuf + hlen, used);
    }

    /* 3. 본문 길이 정하기 (RFC 7230 3.3.3) */
    enum { BODY_NONE, BODY_LENGTH, BODY_EOF } mode;
    if (req->method == HTTP_METHOD_HEAD || info.status == 204 || info.status == 304)
        mode = BODY_NONE;                                                  /* 본문이 없는 응답 */
    else if (info.has_length)
        mode = BODY_LENGTH;                                                /* 정확히 length 바이트 */
    else {
        mode = BODY_EOF;                                                   /* chunked / 길이 없음: 상대가 닫을 때까지 */
        *keep = 0;                                                         /* 끝을 클라이언트에게 알릴 방법 = 연결 닫기 */
    }

    char out[PXY_RESP_HEAD_MAX + 64];                                      /* 다시 쓴 머리 (Connection 줄 정도 늘어남) */
    int ol = pxy_rewrite_response_head(out, sizeof(out), rbuf, hlen, *keep);
    if (ol < 0) {
        close(bfd);
        return 502;
    }
    *status = info.status;

    /* 4. 머리 + 이미 받은 본문 앞부분 전송 */
    size_t have = (size_t)n - hlen;                                        /* 머리 뒤에 같이 온 바이트 */
    unsigned long long left = 0;                                           /* 더 받아야 할 본문 */
    if (mode == BODY_NONE)
        have = 0;                                                          /* (와 있어도 버림: 본문이 없어야 하는 응답) */
    else if (mode == BODY_LENGTH) {
        if (have > info.length)
            have = (size_t)info.length;                                    /* 길이 넘는 쓰레기는 안 넘김 */
        left = info.length - have;
    }
    struct iovec ov[2] = {
        {.iov_base = out, .iov_len = (size_t)ol},
        {.iov_base = rbuf + hlen, .iov_len = have},
    };
    if (tcp_sendv_all(cfd, ov, 2, client_timeout_ms) < 0) {
        close(bfd);
        return -1;                                                         /* 클라이언트가 끊김 */
    }

    /* 5. 나머지 본문 중계: 상대에서 받은 만큼 바로 클라이언트로 (큰 응답도 메모리 16KB 로) */
    char relay[PXY_RELAY_BUF];
    int rc = 0;
    while (mode == BODY_LENGTH && left > 0) {
        size_t want = left < sizeof(relay) ? (size_t)left : sizeof(relay);
        ssize_t k = tcp_recv_some(bfd, relay, want, resp_to);
        if (k <= 0 || tcp_send_all(cfd, relay, (size_t)k, client_timeout_ms) < 0) {
            rc = -1;                                                       /* 상대가 덜 보내고 닫음/멈춤, 또는 클라이언트 끊김 → 잘린 응답이니 연결 끊기 */
            break;
        }
        left -= (unsigned long long)k;
    }
    while (mode == BODY_EOF) {
        ssize_t k = tcp_recv_some(bfd, relay, sizeof(relay), resp_to);
        if (k == 0)                                                        /* 상대가 다 보내고 닫음 = 본문 끝 */
            break;
        if (k < 0 || tcp_send_all(cfd, relay, (size_t)k, client_timeout_ms) < 0) {
            rc = -1;
            break;
        }
    }
    close(bfd);
    return rc;
}
