/* http_response.c — 상태 코드 → 이유 문구 테이블, 응답 머리 문자열 생성. */

#include "http_response.h" /* http_reason, http_format_head 선언 */
#include <stdio.h>         /* snprintf */

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

int http_format_head(char *out, size_t cap, int status, const char *content_type,
                     size_t content_length, int keep_alive) /* 응답 머리를 out 에 씀 */
{
    int n = snprintf(out, cap,                                  /* 형식 문자열대로 out 에 씀 (cap 을 절대 넘지 않음) */
                     "HTTP/1.1 %d %s\r\n"                       /* 상태 줄: 버전, 코드, 이유 문구 */
                     "Content-Type: %s\r\n"                     /* 본문 종류 */
                     "Content-Length: %zu\r\n"                  /* 본문 길이 (%zu = size_t 출력 형식) */
                     "Connection: %s\r\n"                       /* 연결 유지 여부 */
                     "\r\n",                                    /* 빈 줄 = 헤더 끝 */
                     status, http_reason(status), content_type, /* 상태 줄과 Content-Type 값 */
                     content_length, keep_alive ? "keep-alive" : "close"); /* 길이와 Connection 값 */
    if (n < 0 || (size_t)n >= cap) /* 형식 오류, 또는 잘렸음 (snprintf 는 "썼어야 할" 길이를 돌려줌) */
        return -1;                 /* 버퍼 부족 → 실패 */
    return n;                      /* 실제로 쓴 바이트 수 */
}
