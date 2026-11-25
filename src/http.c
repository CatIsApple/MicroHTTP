/*
 * http.c — HTTP 요청 파서와 응답 헤더 생성기
 *
 * HTTP 요청의 모양 (줄 끝은 항상 "\r\n" = CRLF):
 *
 *     POST /api/echo HTTP/1.1        ← 요청 줄: 메서드 SP 경로 SP 버전
 *     Host: localhost:8080           ← 헤더 "이름: 값"
 *     Content-Length: 5
 *                                    ← 빈 줄 = 헤더 끝 ("\r\n\r\n")
 *     hello                          ← 본문: Content-Length 바이트만큼
 *
 * 파서 순서
 *   1) 헤더 끝 "\r\n\r\n" 찾기          (없으면 NEED_MORE)
 *   2) 요청 줄 해석                     (메서드/경로/버전)
 *   3) 헤더 한 줄씩 해석                 (Host, Content-Length, Connection ...)
 *   4) 헤더 전체 규칙 검사               (Host 필수, 길이 정보 충돌 ...)
 *   5) 본문이 다 도착했는지 확인          (없으면 NEED_MORE)
 *
 * 의심스러운 입력은 추측해서 받아주지 않고 거절합니다 (보수적 설계).
 */
#include "http.h"

#include <stdio.h>
#include <string.h>

/* HTTP "token" 문자인지 검사. 메서드와 헤더 이름은 이 문자들로만 이루어집니다. */
static int token_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/* 대소문자 무시 비교. 헤더 이름은 대소문자를 구분하지 않습니다 ("hOsT" == "host"). name은 소문자. */
static int equal_ci(const unsigned char *text, size_t length, const char *name)
{
    if (length != strlen(name)) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = text[i], n = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (n >= 'A' && n <= 'Z') n += 'a' - 'A';   /* 양쪽 모두 소문자로 비교 ("Cookie" == "cookie") */
        if (c != n) return 0;
    }
    return 1;
}

/* *status에 오류 코드를 넣고 HTTP_ERROR를 돌려줍니다. */
static HttpParseResult error(int code, int *status)
{
    *status = code;
    return HTTP_ERROR;
}

/* length 바이트를 복사하고 '\0'을 붙입니다. 자리가 모자라면 -1 (버퍼 넘침 방지). */
static int copy_field(char *output, size_t capacity,
                      const unsigned char *start, size_t length)
{
    if (length >= capacity) return -1;
    memcpy(output, start, length);
    output[length] = '\0';
    return 0;
}

/* [start, end)에서 처음 나오는 "\r\n"의 위치. 없으면 NULL. */
static const unsigned char *find_crlf(const unsigned char *start,
                                      const unsigned char *end)
{
    for (const unsigned char *p = start; p + 1 < end; ++p)
        if (p[0] == '\r' && p[1] == '\n') return p;
    return NULL;
}

/*
 * 요청 줄 "METHOD SP TARGET SP VERSION"을 해석합니다.
 * 반환값: 0이면 성공, 아니면 응답할 오류 코드(400, 414, 505).
 */
static int parse_request_line(const unsigned char *start, size_t length,
                              HttpRequest *request)
{
    const unsigned char *first = memchr(start, ' ', length);
    if (!first || first == start) return 400;            /* 메서드가 없음 */
    const unsigned char *target = first + 1;
    const unsigned char *second = memchr(target, ' ', length - (size_t)(target - start));
    if (!second || second == target) return 400;         /* 경로가 없음 */
    size_t method_length = (size_t)(first - start);
    size_t target_length = (size_t)(second - target);
    size_t version_length = length - (size_t)(second + 1 - start);

    /* 메서드는 token 문자만 ("GE(T" 거절) */
    for (size_t i = 0; i < method_length; ++i)
        if (!token_char(start[i])) return 400;

    /* 경로는 '/'로 시작하는 형태만 지원. 공백·제어문자·비ASCII·'#'은 거절 */
    if (target[0] != '/') return 400;
    for (size_t i = 0; i < target_length; ++i)
        if (target[i] <= 0x20 || target[i] >= 0x7f || target[i] == '#') return 400;
    if (target_length > HTTP_MAX_TARGET) return 414;

    if (copy_field(request->method, sizeof(request->method), start, method_length)) return 400;
    if (copy_field(request->target, sizeof(request->target), target, target_length)) return 414;

    /* 버전은 정확히 "HTTP/숫자.숫자" 형식 */
    const unsigned char *version = second + 1;
    if (version_length != 8 || memcmp(version, "HTTP/", 5) ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') return 400;
    memcpy(request->version, version, 8);
    request->version[8] = '\0';
    /* 형식은 맞지만 1.0/1.1이 아니면(예: HTTP/2.0) 505 */
    if (strcmp(request->version, "HTTP/1.1") && strcmp(request->version, "HTTP/1.0")) return 505;
    return 0;
}

/*
 * Connection 헤더 값("keep-alive, close" 같은 쉼표 목록)에서
 * close / keep-alive 토큰이 있는지 찾습니다. 목록 형식이 깨졌으면 400.
 */
static int parse_connection(const unsigned char *value, size_t length,
                            int *has_close, int *has_keep_alive)
{
    size_t i = 0;
    while (i < length) {
        while (i < length && (value[i] == ' ' || value[i] == '\t' || value[i] == ',')) ++i;
        size_t start = i;
        while (i < length && token_char(value[i])) ++i;
        if (equal_ci(value + start, i - start, "close")) *has_close = 1;
        if (equal_ci(value + start, i - start, "keep-alive")) *has_keep_alive = 1;
        while (i < length && (value[i] == ' ' || value[i] == '\t')) ++i;
        if (i < length && value[i] != ',') return 400;
    }
    return 0;
}

HttpParseResult http_parse_request(const unsigned char *data, size_t length,
                                   HttpRequest *request, size_t *consumed,
                                   int *status)
{
    /* 매번 처음부터 다시 해석합니다 (상태를 따로 기억하지 않아 단순함). */
    memset(request, 0, sizeof(*request));
    *consumed = 0;
    *status = 0;

    /* ── 1단계: 헤더 끝 "\r\n\r\n" 찾기 (최대 HTTP_MAX_HEADER 바이트까지) ── */
    size_t head_length = 0;
    size_t scan_length = length < HTTP_MAX_HEADER ? length : HTTP_MAX_HEADER;
    for (size_t i = 0; i < scan_length; ++i) {
        /* 헤더 안의 NUL, '\r' 없는 '\n', '\n' 없는 '\r'은 거절 */
        if (data[i] == 0 || (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) ||
            (data[i] == '\r' && i + 1 < scan_length && data[i + 1] != '\n'))
            return error(400, status);
        if (i >= 3 && memcmp(data + i - 3, "\r\n\r\n", 4) == 0) {
            head_length = i + 1;
            break;
        }
    }
    if (!head_length) {
        if (length < HTTP_MAX_HEADER) return HTTP_NEED_MORE;
        /* 상한까지 요청 줄조차 끝나지 않았다면 원인은 긴 URI → 414, 아니면 헤더가 큼 → 431 */
        return error(find_crlf(data, data + HTTP_MAX_HEADER) ? 431 : 414, status);
    }

    /* ── 2단계: 요청 줄 ── */
    const unsigned char *end = data + head_length;
    const unsigned char *line_end = find_crlf(data, end);
    int code = parse_request_line(data, (size_t)(line_end - data), request);
    if (code) return error(code, status);
    int http11 = strcmp(request->version, "HTTP/1.1") == 0;

    /* ── 3단계: 헤더 한 줄씩 ── */
    int host_seen = 0, length_seen = 0, transfer_seen = 0, expect_seen = 0;
    int has_close = 0, has_keep_alive = 0;
    size_t body_length = 0;
    const unsigned char *line = line_end + 2;
    request->headers = line;
    request->headers_length = (size_t)(end - 2 - line); /* 마지막 빈 줄 "\r\n" 제외 */
    while (line < end - 2) {
        line_end = find_crlf(line, end);
        if (!line_end) return error(400, status);

        /* "이름: 값" 형식. 이름은 token 문자만 ("Host : a"처럼 콜론 앞 공백도 거절) */
        const unsigned char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (!colon || colon == line) return error(400, status);
        for (const unsigned char *p = line; p < colon; ++p)
            if (!token_char(*p)) return error(400, status);
        for (const unsigned char *p = colon + 1; p < line_end; ++p)
            if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return error(400, status);

        /* 값 앞뒤 공백 제거 */
        const unsigned char *value = colon + 1;
        const unsigned char *value_end = line_end;
        while (value < value_end && (*value == ' ' || *value == '\t')) ++value;
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
        size_t name_length = (size_t)(colon - line);
        size_t value_length = (size_t)(value_end - value);

        if (equal_ci(line, name_length, "host")) {
            /* Host는 한 번만, 비어 있지 않게, 공백·구분자 없이 */
            if (host_seen++ || !value_length ||
                copy_field(request->host, sizeof(request->host), value, value_length))
                return error(400, status);
            for (size_t i = 0; i < value_length; ++i)
                if (value[i] <= 0x20 || value[i] >= 0x7f || strchr("/,?#@\\", value[i]))
                    return error(400, status);
        } else if (equal_ci(line, name_length, "content-length")) {
            /*
             * 본문이 어디서 끝나는지 정하는 값. 중복은 거절합니다.
             * (서버마다 다르게 해석하면 request smuggling 공격의 빌미가 됨)
             * 곱하기 전에 상한을 검사해서 정수 오버플로를 막습니다.
             */
            if (length_seen++ || !value_length) return error(400, status);
            for (size_t i = 0; i < value_length; ++i) {
                if (value[i] < '0' || value[i] > '9') return error(400, status);
                size_t digit = (size_t)(value[i] - '0');
                if (body_length > (HTTP_MAX_BODY - digit) / 10) return error(413, status);
                body_length = body_length * 10 + digit;
            }
        } else if (equal_ci(line, name_length, "transfer-encoding")) {
            transfer_seen = 1;
        } else if (equal_ci(line, name_length, "connection")) {
            code = parse_connection(value, value_length, &has_close, &has_keep_alive);
            if (code) return error(code, status);
        } else if (equal_ci(line, name_length, "expect")) {
            expect_seen = 1;
        }
        /* 그 밖의 헤더는 형식만 검사합니다 (프록시는 headers 원문을 그대로 전달). */
        line = line_end + 2;
    }

    /* ── 4단계: 헤더 전체 규칙 ── */
    if (http11 && !host_seen) return error(400, status);         /* HTTP/1.1은 Host 필수 */
    if (transfer_seen && length_seen) return error(400, status); /* 길이 정보가 둘이면 모호함 */
    if (transfer_seen) return error(501, status);                /* chunked 디코더 없음 */
    if (expect_seen) return error(417, status);                  /* 100-continue 미지원 */
    if (!strcmp(request->method, "POST") && !length_seen) return error(411, status);
    request->keep_alive = http11 ? !has_close : (has_keep_alive && !has_close);

    /* ── 5단계: 본문이 다 왔는지 ── */
    request->body_length = body_length;
    if (length - head_length < body_length) return HTTP_NEED_MORE;
    request->body = data + head_length;
    *consumed = head_length + body_length;
    return HTTP_COMPLETE;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

int http_response_header(char *output, size_t capacity, int status,
                         const char *content_type, size_t body_length,
                         int keep_alive, const char *allow, const char *extra)
{
    const char *reason = http_reason(status);
    /* 값에 "\r\n"이 섞이면 헤더를 끼워 넣을 수 있으므로(헤더 주입) 거절합니다. */
    if (!reason || !content_type || strpbrk(content_type, "\r\n") ||
        (allow && strpbrk(allow, "\r\n"))) return -1;
    /*
     * 예)
     *   HTTP/1.1 200 OK\r\n
     *   Content-Type: text/html; charset=utf-8\r\n
     *   Content-Length: 1234\r\n
     *   Connection: keep-alive\r\n
     *   \r\n
     */
    int length = snprintf(output, capacity,
        "HTTP/1.1 %d %s\r\nServer: MicroHTTP\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: %s\r\n%s%s%s%s\r\n",
        status, reason, content_type, body_length, keep_alive ? "keep-alive" : "close",
        allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "",
        extra ? extra : "");
    /* snprintf는 잘려도 "필요한 길이"를 돌려주므로 capacity 이상이면 실패 */
    return length < 0 || (size_t)length >= capacity ? -1 : length;
}

int http_find_header(const HttpRequest *request, const char *name,
                     const unsigned char **value, size_t *length)
{
    size_t name_length = strlen(name);
    const unsigned char *line = request->headers;
    const unsigned char *end = request->headers + request->headers_length;
    while (line < end) {
        const unsigned char *line_end = find_crlf(line, end);
        if (!line_end) line_end = end;
        /* 파서가 이미 "이름: 값" 형식과 이름 뒤 공백 없음을 검증했습니다. */
        if ((size_t)(line_end - line) > name_length && line[name_length] == ':' &&
            equal_ci(line, name_length, name)) {
            const unsigned char *start = line + name_length + 1;
            const unsigned char *stop = line_end;
            while (start < stop && (*start == ' ' || *start == '\t')) ++start;
            while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t')) --stop;
            *value = start;
            *length = (size_t)(stop - start);
            return 1;
        }
        line = line_end + 2;
    }
    return 0;
}
