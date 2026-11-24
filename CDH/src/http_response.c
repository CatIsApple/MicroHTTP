/* http_response.c — 상태 코드 → 이유 문구 테이블, 응답 머리 문자열 생성·검증, 응답 송신. */

#define _POSIX_C_SOURCE 200809L /* gmtime_r 를 헤더에서 보이게 함. include 보다 먼저 */

#include "http_response.h" /* 이 파일이 구현할 함수들의 선언 */
#include "tcp_server.h"    /* tcp_sendv_all (머리 + 메모리 본문), tcp_sendfile_all (머리 + 파일 본문) */

#include <errno.h>  /* errno, EINVAL */
#include <stdarg.h> /* va_list (appendf 의 가변 인자) */
#include <stdio.h>  /* snprintf, vsnprintf */

/* 상태 코드와 이유 문구 쌍 (RFC 7231 6장 등) */
static const struct {
    int status;         /* 상태 코드 숫자 */
    const char *reason; /* 이유 문구 */
} REASONS[] = {                                    /* 지금 서버가 실제로 쓰는 코드들 */
    {100, "Continue"},                             /* 본문을 보내도 됨 (Expect: 100-continue 응답) */
    {200, "OK"},                                   /* 성공 */
    {204, "No Content"},                           /* 성공, 본문 없음 */
    {301, "Moved Permanently"},                    /* 영구 이동 (디렉터리 "/dir" → "/dir/" 리다이렉트용) */
    {304, "Not Modified"},                         /* 캐시된 것 그대로 써도 됨 (조건부 GET) */
    {400, "Bad Request"},                          /* 요청 문법 오류 */
    {403, "Forbidden"},                            /* 접근 금지 (정적 파일 단계용) */
    {404, "Not Found"},                            /* 리소스 없음 (정적 파일 단계용) */
    {405, "Method Not Allowed"},                   /* 이 리소스에 허용 안 된 메서드 */
    {408, "Request Timeout"},                      /* 요청 수신 시간 초과 */
    {411, "Length Required"},                      /* POST 인데 Content-Length 없음 */
    {413, "Payload Too Large"},                    /* 본문이 상한 초과 */
    {414, "URI Too Long"},                         /* URI 가 상한 초과 */
    {417, "Expectation Failed"},                   /* 모르는 Expect 값 */
    {431, "Request Header Fields Too Large"},      /* 헤더가 너무 큼 / 너무 많음 */
    {500, "Internal Server Error"},                /* 서버 내부 오류 (malloc 실패 등) */
    {501, "Not Implemented"},                      /* 구현 안 한 메서드·기능 */
    {502, "Bad Gateway"},                          /* 백엔드 응답 이상 (프록시 단계용) */
    {504, "Gateway Timeout"},                      /* 백엔드 응답 없음 (프록시 단계용) */
    {505, "HTTP Version Not Supported"},           /* 지원 안 하는 HTTP 버전 */
};

const char *http_reason(int status) /* 표에서 상태 코드를 찾아 이유 문구 반환 */
{
    for (size_t i = 0; i < sizeof(REASONS) / sizeof(REASONS[0]); i++) /* 배열 원소 개수 = 전체 크기 / 원소 하나 크기 */
        if (REASONS[i].status == status)                              /* 코드가 같으면 */
            return REASONS[i].reason;                                 /* 그 문구 */
    return "Unknown"; /* 표에 없는 코드 */
}

int http_status_has_body(int status) /* 본문·Content-Length 를 붙일 수 있는 상태인지 */
{
    /* 204: 본문이 없다고 정의됨 → Content-Length 도 보내면 안 됨 (RFC 7230 3.3.2)
     * 304: 본문 없음. Content-Length 는 "원래 리소스 길이" 의미라 헷갈리므로 생략 (RFC 7232 4.1)
     * 1xx: 이 API 에서는 아예 거절 (http_format_head 참고) */
    return status != 204 && status != 304;
}

int http_format_date(char *out, size_t cap, time_t t) /* t → "Sun, 06 Nov 1994 08:49:37 GMT" */
{
    /* strftime("%a %b") 는 locale 에 따라 한글 등으로 나올 수 있어서 직접 만든 영문 표를 쓴다 */
    static const char *const DAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};   /* tm_wday: 일요일이 0 */
    static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};       /* tm_mon: 1월이 0 */
    struct tm tm;                         /* 쪼갠 시각 (년/월/일/시/분/초) */
    if (gmtime_r(&t, &tm) == NULL)        /* t 를 GMT(UTC) 기준으로 분해. 스레드 안전 버전 (gmtime 은 정적 버퍼를 씀) */
        return -1;                        /* 변환 불가 (범위 밖 시각) */
    int n = snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", /* RFC 7231 고정 길이 형식 */
                     DAYS[tm.tm_wday], tm.tm_mday, MONTHS[tm.tm_mon], /* 요일, 일, 월 */
                     tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec); /* tm_year 는 1900 년부터의 햇수 */
    if (n < 0 || (size_t)n >= cap)        /* 형식 오류 또는 잘림 */
        return -1;                        /* 버퍼 부족 */
    return n;                             /* 쓴 바이트 수 */
}

/* ---- 헤더 값 검증 (응답 분할 / 헤더 주입 방지) ---- */

/* 헤더 값 글자로 괜찮은지: HTAB, 보이는 글자(0x20~0x7E), obs-text(0x80 이상) 만. CR·LF 등 제어 문자 불가 */
static int is_value_char(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7f); /* 0x7f = DEL */
}

/* 문자열 전체가 헤더 값으로 안전하면 1 (Content-Type 검사용) */
static int valid_value(const char *s)
{
    for (; *s; s++)                           /* 한 글자씩 */
        if (!is_value_char((unsigned char)*s)) /* 제어 문자가 섞여 있으면 */
            return 0;                         /* 다음 줄을 끼워 넣을 수 있음 → 거절 */
    return 1;
}

/* extra_headers 가 "이름: 값\r\n" 줄들의 연속인지. 빈 문자열은 OK.
 * 거절하는 것: 빈 줄("\r\n" — 여기서 헤더가 끝나 버려 뒤가 본문으로 해석됨), 단독 CR/LF,
 *             이름 없는 줄, 이름의 공백·제어 문자, 값의 제어 문자, 마지막 CRLF 없음 */
static int valid_extra_headers(const char *s)
{
    while (*s) {                                          /* 줄 단위로 */
        const char *p = s;                                /* 이름 시작 */
        while (*p != ':') {                               /* ':' 까지가 이름 */
            unsigned char c = (unsigned char)*p;          /* 부호 없는 값으로 */
            if (c <= 0x20 || c >= 0x7f)                   /* '\0'(문자열 끝), 공백, CR, LF, 제어 문자, 비 ASCII → 이름으로 불가 */
                return 0;
            p++;
        }
        if (p == s)                                       /* ": 값" — 이름이 비어 있음 */
            return 0;
        for (p++; *p != '\r'; p++)                        /* ':' 다음부터 CR 까지가 값 */
            if (*p == '\0' || !is_value_char((unsigned char)*p)) /* CR 없이 끝났거나 LF 등 제어 문자 */
                return 0;
        if (p[1] != '\n')                                 /* CR 뒤에 LF 가 아님 (단독 CR) */
            return 0;
        s = p + 2;                                        /* 다음 줄 시작 */
    }
    return 1; /* 모든 줄이 올바름 */
}

/* out[*len..] 에 형식 문자열을 이어 쓴다. 자리가 모자라면 -1 (*len 은 그대로) */
static int appendf(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;                                              /* 가변 인자 목록 */
    va_start(ap, fmt);                                       /* fmt 다음 인자부터 */
    int n = vsnprintf(out + *len, cap - *len, fmt, ap);      /* 남은 자리만큼만 씀 (*len < cap 이 항상 유지됨) */
    va_end(ap);                                              /* 정리 */
    if (n < 0 || (size_t)n >= cap - *len)                    /* 형식 오류 또는 잘림 */
        return -1;
    *len += (size_t)n;                                       /* 쓴 만큼 길이 증가 */
    return 0;
}

int http_format_head(char *out, size_t cap, const http_response *res, time_t now) /* 응답 머리를 out 에 씀 */
{
    if (res == NULL || out == NULL || cap == 0)                    /* 쓸 곳·내용 없음 */
        return -1;
    if (res->status < 200 || res->status > 599)                    /* 최종 응답만 (1xx 는 중간 응답이라 형식이 다름) */
        return -1;
    if (res->content_type != NULL && !valid_value(res->content_type)) /* CR/LF 등이 있으면 헤더를 하나 더 끼워 넣을 수 있음 */
        return -1;
    if (res->extra_headers != NULL && !valid_extra_headers(res->extra_headers)) /* 줄 형식이 깨졌으면 응답 분할 위험 */
        return -1;

    char date[32];                                                 /* "Sun, 06 Nov 1994 08:49:37 GMT" = 29글자 + '\0' */
    if (http_format_date(date, sizeof(date), now) < 0)             /* Date 헤더 값 만들기 */
        return -1;

    size_t len = 0;                                                /* 지금까지 쓴 길이 */
    if (appendf(out, cap, &len,                                    /* 항상 들어가는 앞부분 */
                "HTTP/1.1 %d %s\r\n"                               /* 상태 줄: 버전, 코드, 이유 문구 */
                "Date: %s\r\n"                                     /* 응답 생성 시각 (시계가 있는 서버는 반드시 보냄) */
                "Server: " HTTP_SERVER_NAME "\r\n",                /* 서버 이름 */
                res->status, http_reason(res->status), date) < 0)
        return -1;
    if (res->content_type != NULL &&                               /* 본문 종류가 있으면 */
        appendf(out, cap, &len, "Content-Type: %s\r\n", res->content_type) < 0)
        return -1;
    if (http_status_has_body(res->status) &&                       /* 204 / 304 는 Content-Length 없음 */
        appendf(out, cap, &len, "Content-Length: %zu\r\n", res->body_len) < 0) /* %zu = size_t 출력 형식 */
        return -1;
    if (appendf(out, cap, &len,
                "Connection: %s\r\n"                               /* 연결 유지 여부 */
                "%s"                                               /* 추가 헤더들 (검증됨: 각 줄이 \r\n 으로 끝남) */
                "\r\n",                                            /* 빈 줄 = 헤더 끝 */
                res->keep_alive ? "keep-alive" : "close",
                res->extra_headers ? res->extra_headers : "") < 0)
        return -1;                                                 /* 버퍼 부족 → 실패 (잘린 머리를 보내지 않게) */
    return (int)len;                                               /* 실제로 쓴 바이트 수 (len < cap) */
}

int http_send_response(int fd, const http_response *res, int timeout_ms) /* 머리 + 본문 송신 */
{
    char head[HTTP_HEAD_MAX];                                        /* 응답 머리 (스택) */
    int hlen = http_format_head(head, sizeof(head), res, time(NULL)); /* 지금 시각으로 머리 만들기 */
    if (hlen < 0) {                                                  /* 만들 수 없음 (잘못된 값 또는 머리가 너무 큼) */
        errno = EINVAL;                                              /* 호출자가 "송신 실패" 와 구분할 수 있게 */
        return -1;
    }
    size_t blen = http_status_has_body(res->status) ? res->body_len : 0; /* 204 / 304 는 본문을 보내지 않음 */

    if (res->use_file && blen > 0)                                   /* 파일 본문: 머리 뒤에 파일 구간을 커널이 바로 복사 */
        return tcp_sendfile_all(fd, head, (size_t)hlen, res->file_fd, res->file_offset, blen, timeout_ms);

    /* 메모리 본문: 머리와 본문을 sendmsg 한 번으로 → 작은 응답은 패킷 하나, 큰 본문도 복사 없음 */
    struct iovec iov[2] = {{.iov_base = head, .iov_len = (size_t)hlen},
                           {.iov_base = (void *)res->body, .iov_len = res->use_file ? 0 : blen}};
    return tcp_sendv_all(fd, iov, 2, timeout_ms);
}

int http_send_error(int fd, int status, const char *extra_headers, int timeout_ms) /* "<코드> <이유>\n" 오류 응답 */
{
    char body[64];                                                   /* 본문: 예) "404 Not Found\n" */
    int blen = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status)); /* 본문 문자열 만들기 */
    if (blen < 0 || (size_t)blen >= sizeof(body))                    /* 만들기 실패나 잘림 (이유 문구는 짧아서 사실상 없음) */
        blen = 0;                                                    /* 본문 없이 보냄 */
    http_response res = {                                            /* 오류 응답 내용 */
        .status = status,                                            /* 상태 코드 */
        .content_type = "text/plain; charset=utf-8",                 /* 일반 텍스트 */
        .body = body,                                                /* 위에서 만든 본문 */
        .body_len = (size_t)blen,                                    /* 본문 길이 */
        .keep_alive = 0,                                             /* 오류 후엔 항상 연결 종료 */
        .extra_headers = extra_headers,                              /* 405 의 Allow 등 */
    };
    return http_send_response(fd, &res, timeout_ms);                 /* 보내기 */
}
