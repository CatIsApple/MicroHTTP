/* http_response.c — 상태 코드 → 이유 문구 테이블, 응답 머리 문자열 생성, 응답 송신. */

#define _POSIX_C_SOURCE 200809L /* gmtime_r 를 헤더에서 보이게 함. include 보다 먼저 */

#include "http_response.h" /* 이 파일이 구현할 함수들의 선언 */
#include "tcp_server.h"    /* tcp_send_all2 (머리 + 본문 한 번에 송신) */

#include <errno.h>  /* errno, EINVAL */
#include <stdio.h>  /* snprintf */
#include <string.h> /* strlen, strchr */

/* 상태 코드와 이유 문구 쌍 (RFC 7231 6장 등) */
static const struct {
    int status;         /* 상태 코드 숫자 */
    const char *reason; /* 이유 문구 */
} REASONS[] = {                                    /* 지금 서버가 실제로 쓰는 코드들 */
    {100, "Continue"},                             /* 본문을 보내도 됨 (Expect: 100-continue 응답) */
    {200, "OK"},                                   /* 성공 */
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

int http_format_head(char *out, size_t cap, const http_response *res, time_t now) /* 응답 머리를 out 에 씀 */
{
    if (res == NULL || res->content_type == NULL)                  /* 필수 값 없음 */
        return -1;
    if (res->status < 100 || res->status > 599)                    /* HTTP 상태 코드는 세 자리 (1xx~5xx) */
        return -1;
    if (strchr(res->content_type, '\r') || strchr(res->content_type, '\n')) /* CR/LF 가 있으면 헤더를 하나 더 끼워 넣을 수 있음 (헤더 주입) */
        return -1;

    const char *extra = res->extra_headers ? res->extra_headers : ""; /* 추가 헤더가 없으면 빈 문자열 */
    size_t elen = strlen(extra);                                   /* 추가 헤더 길이 */
    if (elen > 0 && (elen < 2 || extra[elen - 2] != '\r' || extra[elen - 1] != '\n')) /* 있으면 "\r\n" 으로 끝나야 뒤의 빈 줄과 안 붙음 */
        return -1;

    char date[32];                                                 /* "Sun, 06 Nov 1994 08:49:37 GMT" = 29글자 + '\0' */
    if (http_format_date(date, sizeof(date), now) < 0)             /* Date 헤더 값 만들기 */
        return -1;

    int n = snprintf(out, cap,                                     /* 형식 문자열대로 out 에 씀 (cap 을 절대 넘지 않음) */
                     "HTTP/1.1 %d %s\r\n"                          /* 상태 줄: 버전, 코드, 이유 문구 */
                     "Date: %s\r\n"                                /* 응답 생성 시각 (시계가 있는 서버는 반드시 보냄) */
                     "Server: " HTTP_SERVER_NAME "\r\n"            /* 서버 이름 */
                     "Content-Type: %s\r\n"                        /* 본문 종류 */
                     "Content-Length: %zu\r\n"                     /* 본문 길이 (%zu = size_t 출력 형식) */
                     "Connection: %s\r\n"                          /* 연결 유지 여부 */
                     "%s"                                          /* 추가 헤더들 (각각 \r\n 으로 끝남) */
                     "\r\n",                                       /* 빈 줄 = 헤더 끝 */
                     res->status, http_reason(res->status), date,  /* 상태 줄과 Date */
                     res->content_type, res->body_len,             /* Content-Type, Content-Length */
                     res->keep_alive ? "keep-alive" : "close", extra); /* Connection 값, 추가 헤더 */
    if (n < 0 || (size_t)n >= cap) /* 형식 오류, 또는 잘렸음 (snprintf 는 "썼어야 할" 길이를 돌려줌) */
        return -1;                 /* 버퍼 부족 → 실패 (잘린 머리를 보내지 않게) */
    return n;                      /* 실제로 쓴 바이트 수 */
}

int http_send_response(int fd, const http_response *res, int timeout_ms) /* 머리 + 본문 송신 */
{
    char head[HTTP_HEAD_MAX];                                        /* 응답 머리 (스택) */
    int hlen = http_format_head(head, sizeof(head), res, time(NULL)); /* 지금 시각으로 머리 만들기 */
    if (hlen < 0) {                                                  /* 만들 수 없음 (잘못된 값 또는 머리가 너무 큼) */
        errno = EINVAL;                                              /* 호출자가 "송신 실패" 와 구분할 수 있게 */
        return -1;
    }
    /* 머리와 본문을 sendmsg 한 번으로: 작은 응답은 패킷 하나로 나가고, 큰 본문도 복사하지 않는다 */
    return tcp_send_all2(fd, head, (size_t)hlen, res->body, res->body_len, timeout_ms);
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
