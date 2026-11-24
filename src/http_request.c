/* http_request.c — L2: HTTP 요청 파서 구현. 인터페이스 설명은 include/http_request.h
 *
 * 흐름
 *   1) 빈 줄("\r\n\r\n")을 찾아 헤더 영역의 끝을 정한다. 없으면 INCOMPLETE.
 *   2) 헤더 영역을 줄 단위로 읽으며 문법을 검사한다. 이때는 위치(slice)만 기록하고 버퍼는 안 바꾼다.
 *   3) Host / Content-Length / Connection 의미 검사, 경로 디코드(path_buf 에 씀).
 *   4) 본문이 Content-Length 만큼 안 왔으면 INCOMPLETE. 여기까지 버퍼가 그대로라 다시 불러도 안전하다.
 *   5) 다 왔으면 버퍼에 '\0' 을 써서 문자열을 완성하고 OK.
 */
#include "http_request.h"

#include <stdint.h>
#include <string.h>

/* 버퍼 안의 한 구간 (NUL 로 끝나지 않음) */
typedef struct {
    const char *p;
    size_t n;
} slice;

typedef struct {
    slice name, value;
} raw_header;

/* ---------- 문자 도우미 (locale 에 상관없이 ASCII 기준) ---------- */

/* 메서드 이름, 헤더 이름에 쓸 수 있는 문자 (RFC 9110 의 tchar) */
static int is_token_char(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* 구간 s 와 문자열 word 가 대소문자 무시하고 같은가 */
static int slice_ieq(slice s, const char *word)
{
    size_t i;
    for (i = 0; i < s.n; i++)
        if (word[i] == '\0' || to_lower(s.p[i]) != to_lower(word[i]))
            return 0;
    return word[i] == '\0';
}

static slice trim(slice s)
{
    while (s.n > 0 && is_space(s.p[0]))
        s.p++, s.n--;
    while (s.n > 0 && is_space(s.p[s.n - 1]))
        s.n--;
    return s;
}

/* "close, Upgrade" 같은 쉼표 목록에 word 가 있는가 (Connection 헤더용) */
static int list_contains(slice list, const char *word)
{
    while (list.n > 0) {
        const char *comma = memchr(list.p, ',', list.n);
        size_t len = comma ? (size_t)(comma - list.p) : list.n;
        if (slice_ieq(trim((slice){list.p, len}), word))
            return 1;
        if (!comma)
            break;
        list.p += len + 1;
        list.n -= len + 1;
    }
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---------- 요청 라인: 메서드 SP 대상 SP 버전 ---------- */

static http_parse_result parse_request_line(slice line, slice *method, slice *target, int *minor)
{
    const char *sp1 = memchr(line.p, ' ', line.n);
    if (!sp1)
        return HTTP_PARSE_BAD_REQUEST;
    const char *rest = sp1 + 1;
    size_t rest_n = line.n - (size_t)(rest - line.p);
    const char *sp2 = memchr(rest, ' ', rest_n);
    if (!sp2)
        return HTTP_PARSE_BAD_REQUEST;

    *method = (slice){line.p, (size_t)(sp1 - line.p)};
    *target = (slice){rest, (size_t)(sp2 - rest)};
    slice version = {sp2 + 1, line.n - (size_t)(sp2 + 1 - line.p)};

    /* 메서드: 비어 있지 않은 token */
    if (method->n == 0)
        return HTTP_PARSE_BAD_REQUEST;
    for (size_t i = 0; i < method->n; i++)
        if (!is_token_char((unsigned char)method->p[i]))
            return HTTP_PARSE_BAD_REQUEST;

    /* 대상: "/" 로 시작하는 경로 형식만 받는다. 공백·제어 문자·비 ASCII 는 안 됨 */
    if (target->n == 0 || target->p[0] != '/')
        return HTTP_PARSE_BAD_REQUEST;
    for (size_t i = 0; i < target->n; i++) {
        unsigned char c = (unsigned char)target->p[i];
        if (c <= ' ' || c >= 0x7F)
            return HTTP_PARSE_BAD_REQUEST;
    }

    /* 버전: 정확히 "HTTP/숫자.숫자" (공백이 하나 더 있었다면 여기 섞여 들어와 걸린다) */
    const char *v = version.p;
    if (version.n != 8 || memcmp(v, "HTTP/", 5) != 0 || v[6] != '.' ||
        v[5] < '0' || v[5] > '9' || v[7] < '0' || v[7] > '9')
        return HTTP_PARSE_BAD_REQUEST;
    if (v[5] != '1')
        return HTTP_PARSE_BAD_VERSION; /* HTTP/0.9, HTTP/2.0 등 */
    *minor = (v[7] == '0') ? 0 : 1;    /* 1.2 이상은 1.1 로 취급 */
    return HTTP_PARSE_OK;
}

/* 메서드는 대소문자를 구분한다 ("get" 은 다른 메서드) */
static http_parse_result classify_method(slice m, http_method *out)
{
    if (m.n == 3 && memcmp(m.p, "GET", 3) == 0)  { *out = HTTP_GET;  return HTTP_PARSE_OK; }
    if (m.n == 4 && memcmp(m.p, "POST", 4) == 0) { *out = HTTP_POST; return HTTP_PARSE_OK; }
    if (m.n == 4 && memcmp(m.p, "HEAD", 4) == 0) { *out = HTTP_HEAD; return HTTP_PARSE_OK; }
    return HTTP_PARSE_NOT_IMPLEMENTED;
}

/* ---------- 헤더 줄: 이름 ":" 공백 값 공백 ---------- */

static http_parse_result parse_header_line(slice line, raw_header *h)
{
    const char *colon = memchr(line.p, ':', line.n);
    if (!colon || colon == line.p)
        return HTTP_PARSE_BAD_REQUEST;
    h->name = (slice){line.p, (size_t)(colon - line.p)};
    /* 이름은 token 문자만. 그래서 "Host : x" (콜론 앞 공백) 과
     * 공백으로 시작하는 이어쓰기 줄(obs-fold)이 여기서 400 이 된다 */
    for (size_t i = 0; i < h->name.n; i++)
        if (!is_token_char((unsigned char)h->name.p[i]))
            return HTTP_PARSE_BAD_REQUEST;

    h->value = trim((slice){colon + 1, line.n - h->name.n - 1});
    for (size_t i = 0; i < h->value.n; i++) {
        unsigned char c = (unsigned char)h->value.p[i];
        if ((c < ' ' && c != '\t') || c == 0x7F) /* NUL 같은 제어 문자 */
            return HTTP_PARSE_BAD_REQUEST;
    }
    return HTTP_PARSE_OK;
}

/* Content-Length: 숫자만 ("+3", "3 4", "-1" 은 안 됨), 넘치면 실패 */
static int parse_size(slice s, size_t *out)
{
    if (s.n == 0)
        return -1;
    size_t v = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9')
            return -1;
        size_t d = (size_t)(s.p[i] - '0');
        if (v > (SIZE_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* 경로의 %XX 를 풀어 out 에 쓴다 (out 크기 >= in.n + 1 이어야 함).
 * 풀린 결과에 NUL 이 있거나 ".." 세그먼트가 있으면 400.
 * (".." 은 문서 루트 밖으로 나가려는 시도. "%2e%2e" 도 푼 뒤 검사하므로 같이 걸린다) */
static http_parse_result decode_path(slice in, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < in.n; i++) {
        char c = in.p[i];
        if (c == '%') {
            if (i + 2 >= in.n)
                return HTTP_PARSE_BAD_REQUEST;
            int hi = hex_digit(in.p[i + 1]), lo = hex_digit(in.p[i + 2]);
            if (hi < 0 || lo < 0)
                return HTTP_PARSE_BAD_REQUEST;
            c = (char)(hi * 16 + lo);
            if (c == '\0')
                return HTTP_PARSE_BAD_REQUEST;
            i += 2;
        }
        out[o++] = c;
    }
    out[o] = '\0';

    for (size_t s = 0; s < o;) { /* '/' 사이 구간을 하나씩 본다 */
        size_t e = s;
        while (e < o && out[e] != '/')
            e++;
        if (e - s == 2 && out[s] == '.' && out[s + 1] == '.')
            return HTTP_PARSE_BAD_REQUEST;
        s = e + 1;
    }
    return HTTP_PARSE_OK;
}

/* buf[from..len) 에서 "\r\n\r\n" 의 시작 위치. 없으면 len */
static size_t find_blank_line(const char *buf, size_t from, size_t len)
{
    for (size_t i = from; i + 4 <= len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i;
    return len;
}

/* ---------- 본체 ---------- */

http_parse_result http_parse(char *buf, size_t len, size_t max_body, http_request *req, size_t *consumed)
{
    *consumed = 0;

    /* 요청 앞의 빈 줄은 건너뛴다 (RFC 9112 2.2) */
    size_t start = 0;
    while (start + 2 <= len && buf[start] == '\r' && buf[start + 1] == '\n')
        start += 2;

    /* 1) 헤더 영역의 끝 */
    size_t blank = find_blank_line(buf, start, len);
    if (blank == len)
        return len >= HTTP_MAX_HEADER_BYTES ? HTTP_PARSE_HEADER_TOO_LARGE : HTTP_PARSE_INCOMPLETE;
    size_t body_start = blank + 4;
    if (body_start > HTTP_MAX_HEADER_BYTES)
        return HTTP_PARSE_HEADER_TOO_LARGE;

    /* 2) 줄 단위 문법 검사. 모든 줄은 CRLF 로 끝나고, 줄 안에 CR 이나 LF 가 따로 있으면 안 된다 */
    slice method = {0}, target = {0};
    int minor = 1;
    raw_header hdr[HTTP_MAX_HEADERS];
    size_t n_hdr = 0;
    size_t head_end = blank + 2; /* 마지막 헤더 줄의 CRLF 까지 */
    for (size_t pos = start, line_no = 0; pos < head_end; line_no++) {
        const char *lf = memchr(buf + pos, '\n', head_end - pos);
        if (!lf || lf == buf + pos || lf[-1] != '\r')
            return HTTP_PARSE_BAD_REQUEST; /* "\n" 만으로 줄을 바꿈 */
        slice line = {buf + pos, (size_t)(lf - 1 - (buf + pos))};
        if (memchr(line.p, '\r', line.n))
            return HTTP_PARSE_BAD_REQUEST; /* 줄 중간의 CR */

        http_parse_result r;
        if (line_no == 0) {
            r = parse_request_line(line, &method, &target, &minor);
        } else {
            if (n_hdr == HTTP_MAX_HEADERS)
                return HTTP_PARSE_HEADER_TOO_LARGE;
            r = parse_header_line(line, &hdr[n_hdr++]);
        }
        if (r != HTTP_PARSE_OK)
            return r;
        pos = (size_t)(lf - buf) + 1;
    }
    http_parse_result r = classify_method(method, &req->method);
    if (r != HTTP_PARSE_OK)
        return r;

    /* 3) 의미 검사 */
    const raw_header *host = NULL;
    int has_length = 0, conn_close = 0, conn_keep = 0;
    size_t length = 0;
    for (size_t i = 0; i < n_hdr; i++) {
        if (slice_ieq(hdr[i].name, "Host")) {
            if (host)
                return HTTP_PARSE_BAD_REQUEST; /* Host 가 두 개 */
            host = &hdr[i];
        } else if (slice_ieq(hdr[i].name, "Content-Length")) {
            size_t v;
            if (parse_size(hdr[i].value, &v) < 0)
                return HTTP_PARSE_BAD_REQUEST;
            if (has_length && v != length)
                return HTTP_PARSE_BAD_REQUEST; /* 값이 다른 Content-Length 두 개: 본문 경계가 모호함 */
            has_length = 1;
            length = v;
        } else if (slice_ieq(hdr[i].name, "Transfer-Encoding")) {
            return HTTP_PARSE_NOT_IMPLEMENTED; /* chunked 본문은 지원하지 않음 */
        } else if (slice_ieq(hdr[i].name, "Connection")) {
            conn_close |= list_contains(hdr[i].value, "close");
            conn_keep |= list_contains(hdr[i].value, "keep-alive");
        }
    }
    if (minor == 1 && !host)
        return HTTP_PARSE_BAD_REQUEST; /* HTTP/1.1 은 Host 필수 */
    if (length > max_body)
        return HTTP_PARSE_BODY_TOO_LARGE; /* 본문을 기다리지 않고 바로 거절 */

    const char *qmark = memchr(target.p, '?', target.n);
    slice path = {target.p, qmark ? (size_t)(qmark - target.p) : target.n};
    r = decode_path(path, req->path_buf); /* path.n < HTTP_MAX_HEADER_BYTES 라 넘치지 않는다 */
    if (r != HTTP_PARSE_OK)
        return r;

    /* 4) 본문이 다 왔나 */
    if (len - body_start < length)
        return HTTP_PARSE_INCOMPLETE;

    /* 5) 완성: 구분자 자리(공백, 콜론, 값 끝)에 '\0' 을 쓴다 */
    buf[method.p + method.n - buf] = '\0';
    buf[target.p + target.n - buf] = '\0';
    req->method_str = method.p;
    req->target = target.p;
    req->path = req->path_buf;
    req->query = qmark ? qmark + 1 : NULL;
    req->version_minor = minor;
    for (size_t i = 0; i < n_hdr; i++) {
        buf[hdr[i].name.p + hdr[i].name.n - buf] = '\0';
        buf[hdr[i].value.p + hdr[i].value.n - buf] = '\0';
        req->headers[i].name = hdr[i].name.p;
        req->headers[i].value = hdr[i].value.p;
    }
    req->n_headers = n_hdr;
    req->remote_addr = NULL;
    req->host = host ? host->value.p : NULL;
    req->content_length = length;
    req->body = length ? buf + body_start : NULL;
    req->body_len = length;
    /* HTTP/1.1 은 기본으로 연결 유지, 1.0 은 기본으로 닫음 */
    req->keep_alive = !conn_close && (minor == 1 || conn_keep);

    *consumed = body_start + length;
    return HTTP_PARSE_OK;
}

const char *http_header_get(const http_request *req, const char *name)
{
    for (size_t i = 0; i < req->n_headers; i++)
        if (slice_ieq((slice){req->headers[i].name, strlen(req->headers[i].name)}, name))
            return req->headers[i].value;
    return NULL;
}

int http_parse_status(http_parse_result r)
{
    switch (r) {
    case HTTP_PARSE_BAD_REQUEST:      return 400;
    case HTTP_PARSE_BODY_TOO_LARGE:   return 413;
    case HTTP_PARSE_HEADER_TOO_LARGE: return 431;
    case HTTP_PARSE_NOT_IMPLEMENTED:  return 501;
    case HTTP_PARSE_BAD_VERSION:      return 505;
    default:                          return 0;
    }
}
