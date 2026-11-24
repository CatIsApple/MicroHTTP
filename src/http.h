/*
 * http.h — HTTP 요청 파서와 응답 헤더 생성기
 *
 * 이 모듈은 "바이트 ↔ HTTP 의미" 변환만 합니다. 소켓 I/O를 하지 않으므로
 * 네트워크 없이 단위 테스트할 수 있습니다 (tests/unit/test_http.c).
 */
#ifndef MICROHTTP_HTTP_H
#define MICROHTTP_HTTP_H

#include <stddef.h>

/* 요청 크기 상한(바이트). 큰 요청으로 메모리를 낭비하거나 넘치지 않게 막습니다. */
enum {
    HTTP_MAX_HEADER = 16384, /* 요청 줄 + 헤더 + 빈 줄 (16 KiB) → 넘으면 431 */
    HTTP_MAX_BODY = 65536,   /* 본문 (64 KiB) → 넘으면 413 */
    HTTP_MAX_TARGET = 2048   /* 요청 경로 "/a/b?x=1" → 넘으면 414 */
};

/*
 * http_parse_request()의 결과
 *  HTTP_NEED_MORE : 요청이 아직 덜 도착함 → recv()로 더 받은 뒤 다시 호출
 *  HTTP_COMPLETE  : 요청 하나를 완성함 → *consumed 바이트가 이 요청
 *  HTTP_ERROR     : 잘못된 요청 → *status 코드로 오류 응답 후 연결 종료
 */
typedef enum { HTTP_NEED_MORE, HTTP_COMPLETE, HTTP_ERROR } HttpParseResult;

/* 해석이 끝난 HTTP 요청 하나 */
typedef struct {
    char method[32];                  /* "GET", "POST" ... */
    char target[HTTP_MAX_TARGET + 1]; /* "/index.html?x=1" (+1은 '\0' 자리) */
    char version[9];                  /* "HTTP/1.0" 또는 "HTTP/1.1" */
    char host[256];                   /* Host 헤더 값 (HTTP/1.0이면 비어 있을 수 있음) */
    /*
     * 이 요청에 응답한 뒤 연결을 유지할지(keep-alive) 여부.
     *  HTTP/1.1: 기본 유지, "Connection: close"가 있으면 닫음
     *  HTTP/1.0: 기본 닫음, "Connection: keep-alive"가 있으면 유지
     */
    int keep_alive;
    /*
     * 아래 포인터들은 복사본이 아니라 입력 버퍼 안을 "빌려" 가리킵니다.
     * 요청 처리가 끝나기 전에 입력 버퍼를 free하거나 옮기면 안 됩니다.
     */
    const unsigned char *headers; /* 요청 줄 다음의 헤더 원문 (각 줄 "\r\n"으로 끝남). 프록시가 사용 */
    size_t headers_length;
    const unsigned char *body;    /* 본문 시작 위치 (Content-Length 바이트) */
    size_t body_length;
} HttpRequest;

/*
 * 지금까지 받은 바이트(data, length)로 요청 하나를 해석합니다.
 * 입력 버퍼를 수정하거나 소유하지 않습니다.
 */
HttpParseResult http_parse_request(const unsigned char *data, size_t length,
                                   HttpRequest *request, size_t *consumed,
                                   int *status);

/* 상태 코드 → 설명 문구. 예: 404 → "Not Found". 모르는 코드면 NULL */
const char *http_reason(int status);

/*
 * 응답 헤더(상태 줄 + 헤더 + 빈 줄)를 output에 만듭니다.
 *   keep_alive : 1이면 "Connection: keep-alive", 0이면 "Connection: close"
 *   allow      : 405 응답의 Allow 헤더 값. 필요 없으면 NULL
 * 반환값: 헤더 바이트 수. 모르는 상태 코드·잘못된 값·공간 부족이면 -1
 */
int http_response_header(char *output, size_t capacity, int status,
                         const char *content_type, size_t body_length,
                         int keep_alive, const char *allow);

#endif
