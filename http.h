/*
 * http.h — HTTP/1.1 요청 파서와 응답 헤더 생성기
 *
 * 이 모듈은 "바이트 덩어리 ↔ HTTP 의미" 변환만 담당합니다.
 * 소켓을 읽거나 쓰는 일(네트워크 I/O)은 server.c가 하고,
 * 어떤 경로에 어떤 응답을 줄지는 football.c가 정합니다.
 * 덕분에 이 파일의 함수들은 네트워크 없이도 단위 테스트할 수 있습니다.
 */
#ifndef MICROHTTP_HTTP_H
#define MICROHTTP_HTTP_H

#include <stddef.h>

/* 서버가 받아들이는 크기 상한(바이트). 너무 큰 요청으로 메모리를 낭비하지 않도록 막습니다. */
enum {
    HTTP_MAX_HEADER = 16384, /* 요청 줄 + 모든 헤더 + 빈 줄까지의 최대 길이 (16 KiB) */
    HTTP_MAX_BODY = 65536,   /* 본문(body)의 최대 길이 (64 KiB) */
    HTTP_MAX_TARGET = 2048   /* 요청 경로(예: "/matches/1?x=1")의 최대 길이 */
};

/*
 * http_parse_request()의 결과
 *  - HTTP_NEED_MORE : 아직 요청이 다 도착하지 않음 → 더 recv() 한 뒤 다시 호출
 *  - HTTP_COMPLETE  : 요청 하나를 온전히 해석함
 *  - HTTP_ERROR     : 잘못된 요청 → status에 돌려줄 오류 코드(400, 413 등)가 들어 있음
 */
typedef enum { HTTP_NEED_MORE, HTTP_COMPLETE, HTTP_ERROR } HttpParseResult;

/* 해석이 끝난 HTTP 요청 하나 */
typedef struct {
    char method[32];                  /* "GET", "POST" 등 */
    char target[HTTP_MAX_TARGET + 1]; /* "/matches/1" 같은 경로 (+1은 문자열 끝 '\0' 자리) */
    char version[9];                  /* "HTTP/1.1" (8글자 + '\0') */
    char host[256];                   /* Host 헤더 값 */
    char content_type[128];           /* Content-Type 헤더 값 (없으면 빈 문자열) */
    int connection_close;             /* "Connection: close"가 있었으면 1 */
    /*
     * 본문은 복사하지 않고, 입력 버퍼 안의 위치를 가리키기만 합니다("빌린다").
     * 그래서 요청 처리가 끝나기 전에 원래 버퍼를 free하거나 옮기면 안 됩니다.
     */
    const unsigned char *body;
    size_t body_length;
} HttpRequest;

/* 라우터(football.c)가 채워서 서버(server.c)에게 돌려주는 응답 정보 */
typedef struct {
    int status;                /* 상태 코드. 예: 200, 404 */
    const char *content_type;  /* 예: "text/html; charset=utf-8" */
    const unsigned char *body; /* 보낼 본문의 시작 위치 */
    size_t body_length;        /* 본문 길이(바이트) */
    const char *allow;         /* 405 응답일 때 "Allow" 헤더에 넣을 메서드 목록. 그 외에는 NULL */
    /*
     * 본문 메모리의 주인이 누구인지 표시합니다.
     *  - NULL     : 정적 문자열 등 "빌린" 본문 → 해제할 필요 없음
     *  - NULL 아님 : malloc으로 만든 본문 → 송신이 끝난 뒤 호출자가 free해야 함
     */
    unsigned char *owned_body;
} HttpResponse;

/*
 * 지금까지 받은 바이트(data, length)를 보고 HTTP 요청 하나를 해석합니다.
 * - 네트워크 I/O를 하지 않고, 입력 버퍼를 수정하거나 소유하지도 않습니다.
 * - COMPLETE일 때만 *consumed에 "요청 하나가 차지한 전체 바이트 수"가 들어갑니다.
 * - ERROR이면 *status에 응답할 오류 코드가 들어갑니다.
 * - NEED_MORE이면 같은 버퍼 뒤에 데이터를 더 이어 받은 뒤 다시 호출하면 됩니다.
 */
HttpParseResult http_parse_request(const unsigned char *data, size_t length,
                                   HttpRequest *request, size_t *consumed,
                                   int *status);

/* 상태 코드에 맞는 설명 문구를 돌려줍니다. 예: 404 → "Not Found". 모르는 코드면 NULL. */
const char *http_reason(int status);

/*
 * 응답의 헤더 부분(상태 줄 + 헤더들 + 빈 줄)을 output에 문자열로 만듭니다.
 * - 반환값: 만들어진 헤더의 바이트 수. 모르는 상태 코드, 잘못된 인자, 공간 부족이면 -1.
 * - body_length는 Content-Length에 쓰일 값일 뿐, 본문 자체는 호출자가 따로 보냅니다.
 * - allow는 405 응답에서만 쓰는 허용 메서드 목록이고, 그 외에는 NULL을 넘깁니다.
 */
int http_response_header(char *output, size_t capacity, int status,
                         const char *content_type, size_t body_length,
                         const char *allow);

#endif
