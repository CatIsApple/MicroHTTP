/*
 * http.c — HTTP/1.1 요청 파서와 응답 헤더 생성기 구현
 *
 * HTTP 요청은 이렇게 생겼습니다 (줄 끝은 항상 "\r\n" = CRLF):
 *
 *     POST /api/matches/1/messages HTTP/1.1     ← 요청 줄 (메서드, 경로, 버전)
 *     Host: localhost:8080                      ← 헤더 "이름: 값"
 *     Content-Length: 6
 *                                               ← 빈 줄 = 헤더 끝 ("\r\n\r\n")
 *     골!!!                                      ← 본문 (Content-Length 바이트만큼)
 *
 * 파서는 (1) 헤더 끝을 찾고, (2) 요청 줄을 해석하고, (3) 헤더를 한 줄씩 검사한 뒤,
 * (4) 본문이 다 도착했는지 확인하는 순서로 동작합니다.
 * 의심스러운 입력은 "추측해서 받아주기"보다 "거절하기"를 택하는 보수적인 설계입니다.
 */
#include "http.h"

#include <stdio.h>
#include <string.h>

/*
 * HTTP 명세에서 "token"으로 허용하는 문자인지 검사합니다.
 * 메서드 이름(GET)과 헤더 이름(Content-Type)은 이 문자들로만 이루어져야 합니다.
 * 영문자, 숫자, 그리고 몇 가지 특수문자만 허용되고 공백·괄호·콜론 등은 안 됩니다.
 */
static int token_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/*
 * 대소문자를 무시하고 text(길이 length)가 name과 같은지 비교합니다.
 * HTTP 헤더 이름은 대소문자를 구분하지 않으므로 "hOsT"도 "host"로 인식해야 합니다.
 * name은 항상 소문자로 넘긴다고 가정하고, text 쪽만 소문자로 바꿔 비교합니다.
 */
static int equal_ci(const unsigned char *text, size_t length, const char *name)
{
    if (length != strlen(name)) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = text[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; /* 대문자 → 소문자 */
        if (c != (unsigned char)name[i]) return 0;
    }
    return 1;
}

/* 오류 코드를 *status에 기록하고 HTTP_ERROR를 돌려주는 작은 도우미입니다. */
static int error(int code, int *status)
{
    *status = code;
    return HTTP_ERROR;
}

/*
 * start부터 length 바이트를 output에 복사하고 끝에 '\0'을 붙입니다.
 * '\0'까지 들어갈 자리가 없으면 아무것도 하지 않고 -1을 돌려줍니다 (버퍼 넘침 방지).
 */
static int copy_field(char *output, size_t capacity,
                      const unsigned char *start, size_t length)
{
    if (length >= capacity) return -1;
    memcpy(output, start, length);
    output[length] = '\0';
    return 0;
}

/* [start, end) 범위에서 처음 나오는 "\r\n"의 위치를 찾습니다. 없으면 NULL. */
static const unsigned char *find_crlf(const unsigned char *start,
                                      const unsigned char *end)
{
    for (const unsigned char *p = start; p + 1 < end; ++p)
        if (p[0] == '\r' && p[1] == '\n') return p;
    return NULL;
}

/*
 * 요청 줄 "METHOD SP TARGET SP VERSION"을 해석해 request에 채웁니다.
 * (SP는 공백 한 칸입니다. 공백이 두 칸이면 형식 오류로 봅니다.)
 * 반환값: 0이면 성공, 아니면 응답할 HTTP 오류 코드(400, 414, 505).
 */
static int parse_request_line(const unsigned char *start, size_t length,
                               HttpRequest *request)
{
    /* 첫 번째 공백 앞은 메서드, 두 공백 사이는 경로, 두 번째 공백 뒤는 버전입니다. */
    const unsigned char *first = memchr(start, ' ', length);
    if (!first || first == start) return 400;            /* 공백이 없거나 메서드가 비어 있음 */
    const unsigned char *target = first + 1;
    const unsigned char *second = memchr(target, ' ', length - (size_t)(target - start));
    if (!second || second == target) return 400;         /* 두 번째 공백이 없거나 경로가 비어 있음 */
    size_t method_length = (size_t)(first - start);
    size_t target_length = (size_t)(second - target);
    size_t version_length = length - (size_t)(second + 1 - start);

    /* 메서드는 token 문자로만 이루어져야 합니다. (예: "GE(T"는 거절) */
    for (size_t i = 0; i < method_length; ++i)
        if (!token_char(start[i])) return 400;

    /*
     * 경로는 '/'로 시작하는 형태(origin-form)만 지원합니다.
     * "http://a/" 같은 절대 URL은 거절합니다. ".." 같은 경로 해석은 라우터의 몫입니다.
     */
    if (target[0] != '/') return 400;
    /* 경로에 공백·제어문자·비ASCII·'#'(프래그먼트는 서버로 보내면 안 됨)이 있으면 거절합니다. */
    for (size_t i = 0; i < target_length; ++i)
        if (target[i] <= 0x20 || target[i] >= 0x7f || target[i] == '#') return 400;
    if (target_length > HTTP_MAX_TARGET) return 414;      /* 414 URI Too Long */

    if (copy_field(request->method, sizeof(request->method), start, method_length)) return 400;
    if (copy_field(request->target, sizeof(request->target), target, target_length)) return 414;

    /* 버전은 정확히 "HTTP/숫자.숫자" 8글자 형식이어야 합니다. */
    const unsigned char *version = second + 1;
    if (version_length != 8 || memcmp(version, "HTTP/", 5) ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') return 400;
    memcpy(request->version, version, 8);
    request->version[8] = '\0';
    /* 형식은 맞지만 1.1이 아니면(예: HTTP/1.0) 505 HTTP Version Not Supported */
    return strcmp(request->version, "HTTP/1.1") ? 505 : 0;
}

/*
 * Connection 헤더 값을 해석합니다. 값은 "keep-alive, Close"처럼 쉼표로 구분된 목록입니다.
 * 그중 "close"(대소문자 무관)가 있으면 request->connection_close = 1로 표시합니다.
 * "close;bad"처럼 목록 형식이 깨져 있으면 400을 돌려줍니다.
 */
static int parse_connection(const unsigned char *value, size_t length, HttpRequest *request)
{
    size_t i = 0;
    while (i < length) {
        /* 앞쪽 공백과 쉼표 건너뛰기 */
        while (i < length && (value[i] == ' ' || value[i] == '\t' || value[i] == ',')) ++i;
        /* token 하나 읽기 */
        size_t start = i;
        while (i < length && token_char(value[i])) ++i;
        if (equal_ci(value + start, i - start, "close")) request->connection_close = 1;
        /* token 뒤 공백 건너뛰기. 그다음에는 쉼표나 값의 끝만 올 수 있습니다. */
        while (i < length && (value[i] == ' ' || value[i] == '\t')) ++i;
        if (i < length && value[i] != ',') return 400;
    }
    return 0;
}

HttpParseResult http_parse_request(const unsigned char *data, size_t length,
                                   HttpRequest *request, size_t *consumed,
                                   int *status)
{
    /* 매번 결과를 깨끗하게 초기화한 뒤 처음부터 다시 해석합니다. */
    memset(request, 0, sizeof(*request));
    *consumed = 0;
    *status = 0;

    /*
     * ── 1단계: 헤더의 끝("\r\n\r\n")을 찾습니다 ──
     * 헤더 상한(HTTP_MAX_HEADER)까지만 살펴봅니다.
     * 찾는 김에 헤더 안에 있으면 안 되는 바이트도 함께 검사합니다.
     */
    size_t head_length = 0;
    size_t scan_length = length < HTTP_MAX_HEADER ? length : HTTP_MAX_HEADER;
    for (size_t i = 0; i < scan_length; ++i) {
        /*
         * 헤더에서 거절하는 경우:
         *  - NUL(0) 바이트: C 문자열 함수가 여기서 끊겨 오해석할 수 있음
         *  - '\r' 없이 홀로 쓰인 '\n'
         *  - 뒤에 '\n'이 오지 않는 '\r'
         * (본문은 바이너리일 수 있으므로, 헤더와 본문을 문자열 함수로 한꺼번에 다루지 않습니다.)
         */
        if (data[i] == 0 || (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) ||
            (data[i] == '\r' && i + 1 < scan_length && data[i + 1] != '\n'))
            return error(400, status);
        if (i >= 3 && memcmp(data + i - 3, "\r\n\r\n", 4) == 0) {
            head_length = i + 1; /* 빈 줄까지 포함한 헤더 전체 길이 */
            break;
        }
    }
    if (!head_length)
        /* 상한까지 봤는데도 헤더가 안 끝났으면 431, 아직 덜 받았으면 더 기다립니다. */
        return length >= HTTP_MAX_HEADER ? error(431, status) : HTTP_NEED_MORE;

    /* ── 2단계: 첫 줄(요청 줄)을 해석합니다 ── */
    const unsigned char *end = data + head_length;
    const unsigned char *line_end = find_crlf(data, end);
    int code = parse_request_line(data, (size_t)(line_end - data), request);
    if (code) return error(code, status);

    /*
     * ── 3단계: 헤더를 한 줄씩 해석합니다 ──
     * *_seen 변수는 "이 헤더가 이미 나왔는지" 기억해서 중복이나 충돌을 잡아냅니다.
     */
    int host_seen = 0, length_seen = 0, transfer_seen = 0, expect_seen = 0, type_seen = 0;
    size_t body_length = 0;
    const unsigned char *line = line_end + 2; /* 요청 줄 다음 줄부터 */
    while (line < end - 2) {                  /* 마지막 빈 줄("\r\n") 전까지 */
        line_end = find_crlf(line, end);
        if (!line_end) return error(400, status);

        /* "이름: 값" 형식인지 확인합니다. 콜론이 없거나 이름이 비어 있으면 오류. */
        const unsigned char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (!colon || colon == line) return error(400, status);
        /* 헤더 이름은 token 문자만. "Host : a"처럼 콜론 앞 공백도 여기서 걸러집니다. */
        for (const unsigned char *p = line; p < colon; ++p)
            if (!token_char(*p)) return error(400, status);
        /* 헤더 값에는 탭을 제외한 제어문자가 들어갈 수 없습니다. */
        for (const unsigned char *p = colon + 1; p < line_end; ++p)
            if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return error(400, status);

        /* 값 앞뒤의 공백/탭을 잘라냅니다. "Host:   a  " → "a" */
        const unsigned char *value = colon + 1;
        const unsigned char *value_end = line_end;
        while (value < value_end && (*value == ' ' || *value == '\t')) ++value;
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
        size_t name_length = (size_t)(colon - line);
        size_t value_length = (size_t)(value_end - value);

        if (equal_ci(line, name_length, "host")) {
            /* Host는 정확히 한 번, 비어 있지 않은 값으로 와야 합니다. */
            if (host_seen++ || !value_length ||
                copy_field(request->host, sizeof(request->host), value, value_length))
                return error(400, status);
            /*
             * 완벽한 URI authority 검증기는 아니지만,
             * 공백·제어문자나 경로/구분자 문자("/,?#@\")가 섞인 값은 거절합니다.
             */
            for (size_t i = 0; i < value_length; ++i)
                if (value[i] <= 0x20 || value[i] >= 0x7f || strchr("/,?#@\\", value[i]))
                    return error(400, status);
        } else if (equal_ci(line, name_length, "content-length")) {
            /*
             * Content-Length는 "본문이 어디서 끝나는지"를 정하는 중요한 값입니다.
             * 같은 값이 두 번 와도 거절하는 보수적인 정책으로, 해석이 하나로만 정해지게 합니다.
             * (서버마다 다르게 해석하면 요청 밀반입(request smuggling) 공격의 빌미가 됩니다.)
             */
            if (length_seen++ || !value_length) return error(400, status);
            /* 숫자만 허용("-1", "+1", "1, 1" 거절). 자리마다 상한 초과를 미리 검사해 오버플로를 막습니다. */
            for (size_t i = 0; i < value_length; ++i) {
                if (value[i] < '0' || value[i] > '9') return error(400, status);
                size_t digit = (size_t)(value[i] - '0');
                /* body_length * 10 + digit > HTTP_MAX_BODY 인지를 곱하기 전에 확인 */
                if (body_length > (HTTP_MAX_BODY - digit) / 10) return error(413, status);
                body_length = body_length * 10 + digit;
            }
        } else if (equal_ci(line, name_length, "content-type")) {
            if (type_seen++ || !value_length ||
                copy_field(request->content_type, sizeof(request->content_type), value, value_length))
                return error(400, status);
        } else if (equal_ci(line, name_length, "transfer-encoding")) {
            transfer_seen = 1;
        } else if (equal_ci(line, name_length, "connection")) {
            code = parse_connection(value, value_length, request);
            if (code) return error(code, status);
        } else if (equal_ci(line, name_length, "expect")) {
            expect_seen = 1;
        }
        /* 그 밖의 헤더(User-Agent 등)는 형식만 검사하고 무시합니다. */
        line = line_end + 2; /* 다음 줄로 */
    }

    /* ── 4단계: 헤더 전체를 본 뒤의 규칙 검사 ── */
    if (!host_seen) return error(400, status);                 /* HTTP/1.1은 Host 필수 */
    if (transfer_seen && length_seen) return error(400, status); /* 본문 길이를 정하는 방법이 둘이면 모호함 */
    /*
     * chunked 같은 Transfer-Encoding은 지원하지 않습니다.
     * 조용히 무시하면 본문을 다음 요청으로 잘못 읽을 수 있으므로 501로 확실히 거절합니다.
     */
    if (transfer_seen) return error(501, status);
    /* "Expect: 100-continue"도 지원하지 않으므로 417로 알려줍니다. */
    if (expect_seen) return error(417, status);
    /* POST인데 본문 길이를 알려주지 않으면 411 Length Required */
    if (!strcmp(request->method, "POST") && !length_seen) return error(411, status);

    /* ── 5단계: 본문이 다 도착했는지 확인 ── */
    request->body_length = body_length;
    if (length - head_length < body_length) return HTTP_NEED_MORE; /* 아직 덜 받음 */
    request->body = data + head_length;      /* 본문은 복사하지 않고 위치만 가리킵니다 */
    *consumed = head_length + body_length;   /* 이 요청이 차지한 전체 바이트 수 */
    return HTTP_COMPLETE;
}

/* 상태 코드 → 설명 문구. 여기에 없는 코드는 서버가 보낼 수 없습니다(NULL 반환). */
const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

int http_response_header(char *output, size_t capacity, int status,
                         const char *content_type, size_t body_length,
                         const char *allow)
{
    const char *reason = http_reason(status);
    /*
     * 값 안에 "\r\n"이 있으면 헤더를 마음대로 끼워 넣을 수 있게 됩니다(헤더 주입).
     * 그런 값은 아예 거절합니다.
     */
    if (!reason || !content_type || strpbrk(content_type, "\r\n") ||
        (allow && strpbrk(allow, "\r\n"))) return -1;
    /*
     * 만들어지는 예:
     *   HTTP/1.1 200 OK\r\n
     *   Content-Type: text/html; charset=utf-8\r\n
     *   Content-Length: 1234\r\n
     *   Connection: close\r\n          ← 응답 하나 보내고 연결을 닫는다는 뜻
     *   Cache-Control: no-store\r\n    ← 브라우저가 캐시하지 않게 (채팅이 항상 최신으로 보이도록)
     *   Allow: GET\r\n                 ← allow가 있을 때만
     *   \r\n                           ← 헤더 끝
     */
    int length = snprintf(output, capacity,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n%s%s%s\r\n",
        status, reason, content_type, body_length,
        allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "");
    /* snprintf는 공간이 모자라도 "필요했던 길이"를 돌려주므로, capacity 이상이면 잘린 것입니다. */
    return length < 0 || (size_t)length >= capacity ? -1 : length;
}
