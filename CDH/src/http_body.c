/* http_body.c — Content-Length 기반 요청 본문 수신.
 *
 * tcp_recv_request 는 "헤더 끝" 까지만 기다리지만, 한 번의 recv 에 본문 일부(또는 전부)가
 * 같이 딸려 오는 경우가 흔하다. 그래서 버퍼 상태는 이렇게 생겼다:
 *
 *   buf: [ 헤더 ... \r\n\r\n | 본문 앞부분(have) | 아직 안 온 부분 ... ]
 *         0          header_len           received
 *
 * 할 일: 부족한 (content_length - have) 바이트를 더 받아서 본문을 완성한다.
 *   - 버퍼에 자리가 있으면 그대로 이어 받음 (malloc 없음, 작은 POST 대부분)
 *   - 자리가 없으면 본문 크기만큼 malloc 해서 앞부분 복사 후 나머지를 받음 */

#include "http_body.h"  /* http_read_body 선언, HTTP_MAX_BODY_SIZE */
#include "tcp_server.h" /* tcp_recv_exact (정확히 n 바이트 받기), tcp_send_all (100 Continue 전송) */
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* memcpy */

/* Expect: 100-continue 에 대한 중간 응답. 큰 본문을 보내기 전에 서버의 허락을 기다리는 클라이언트(curl 등)용 */
static const char RESP_100[] = "HTTP/1.1 100 Continue\r\n\r\n"; /* 상태 줄 + 빈 줄만 있는 응답 (본문 없음) */

/* 대소문자 무시 비교 (ASCII). 같으면 1 */
static int ieq(const char *a, const char *b) /* Expect 값 비교용. http_parser.c 의 것은 static 이라 여기서 따로 둠 */
{
    for (; *a && *b; a++, b++) {                     /* 둘 중 하나가 끝날 때까지 한 글자씩 */
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a; /* a 의 글자를 소문자로 */
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b; /* b 의 글자를 소문자로 */
        if (x != y)                                  /* 다르면 */
            return 0;                                /* 다른 문자열 */
    }
    return *a == *b; /* 둘 다 같이 끝나야 같음 */
}

int http_read_body(int fd, http_request *req, char *buf, size_t cap,
                   size_t received, size_t header_len, int timeout_ms, char **heap_out) /* 본문을 받아 req 에 연결 */
{
    *heap_out = NULL;   /* 기본값: malloc 안 함 */
    req->body = NULL;   /* 기본값: 본문 없음 */
    req->body_len = 0;  /* 기본값: 길이 0 */

    /* 1) Expect 헤더: "100-continue" 만 지원 (RFC 7231 5.1.1). HTTP/1.0 클라이언트의 Expect 는 무시 */
    int want_continue = 0;                                 /* 100 Continue 를 보내야 하는지 */
    const char *expect = http_get_header(req, "Expect");   /* Expect 헤더 값 (없으면 NULL) */
    if (expect != NULL && req->version_minor == 1) {       /* HTTP/1.1 이고 Expect 가 있으면 */
        if (!ieq(expect, "100-continue"))                  /* 모르는 기대값이면 */
            return 417;                                    /* 417 Expectation Failed */
        want_continue = 1;                                 /* 본문을 받기 전에 100 Continue 를 보낼 것 */
    }

    /* 2) Content-Length 가 없는 경우 */
    if (!req->has_content_length) {                        /* 본문 길이를 알 방법이 없음 (chunked 는 파서가 이미 501 로 거절) */
        if (req->method == HTTP_METHOD_POST)               /* POST 인데 길이가 없으면 */
            return 411;                                    /* 411 Length Required */
        return 0; /* GET 등: 본문 없음. 헤더 뒤에 더 온 바이트는 다음 요청(파이프라이닝)이라 건드리지 않음 */
    }

    size_t cl = req->content_length;     /* 받아야 할 본문 길이 */
    if (cl > HTTP_MAX_BODY_SIZE)         /* 상한 초과 (100 Continue 를 보내기 전에 판정 → 클라이언트가 본문을 안 보내도 됨) */
        return 413;                      /* 413 Payload Too Large */
    if (cl == 0) {                       /* "Content-Length: 0" */
        req->body = buf + header_len;    /* 빈 본문. 포인터는 유효한 위치를 가리키게 (NULL 대신) */
        return 0;                        /* 성공 */
    }

    /* 3) 이미 받은 본문 앞부분 */
    size_t have = received - header_len; /* 헤더 뒤에 딸려 온 바이트 수 */
    if (have >= cl) {                    /* 본문이 이미 전부 도착함 (가장 흔한 작은 POST) */
        req->body = buf + header_len;    /* 버퍼 안의 본문 위치 */
        req->body_len = cl;              /* cl 뒤의 남는 바이트는 다음 요청이므로 본문에 포함 안 함 */
        return 0;                        /* 성공, 추가 수신 없음 */
    }

    /* 4) 100 Continue: 클라이언트가 아직 본문을 한 바이트도 안 보냈을 때만 의미 있음 */
    if (want_continue && have == 0) {                                          /* 허락을 기다리는 중이면 */
        if (tcp_send_all(fd, RESP_100, sizeof(RESP_100) - 1, timeout_ms) < 0) /* "보내도 됨" 응답 전송 */
            return -1;                                                         /* 전송 실패 → 연결 닫기 */
    }

    /* 5) 본문을 담을 곳 정하기 */
    char *dst;                                     /* 본문 전체가 들어갈 시작 위치 */
    if (header_len + cl < cap) {                   /* 헤더 버퍼에 본문 + '\0' 자리가 남으면 */
        dst = buf + header_len;                    /* 그대로 이어 받음 (앞부분 have 바이트는 이미 제자리에 있음) */
    } else {                                       /* 자리가 모자라면 */
        dst = malloc(cl + 1);                      /* 본문 크기 + '\0' 만큼 힙에 할당 (cl <= 1MB 라 +1 오버플로 없음) */
        if (dst == NULL)                           /* 할당 실패 */
            return 500;                            /* 500 Internal Server Error */
        memcpy(dst, buf + header_len, have);       /* 이미 받은 앞부분을 새 공간으로 복사 */
        *heap_out = dst;                           /* 호출자가 나중에 free 하도록 알려 줌 */
    }

    /* 6) 나머지 (cl - have) 바이트 수신 */
    if (tcp_recv_exact(fd, dst + have, cl - have, timeout_ms) < 0) { /* 앞부분 뒤에 이어서 정확히 남은 만큼만 받음 */
        free(*heap_out);                           /* 할당했다면 반납 (NULL 이면 아무 일 안 함) */
        *heap_out = NULL;                          /* 호출자가 두 번 free 하지 않게 */
        return -1;                                 /* 타임아웃·끊김 → 응답 없이 닫기 */
    }
    dst[cl] = '\0'; /* 본문 끝에 '\0' (본문을 문자열로 찍어 볼 때 안전. body_len 이 진짜 길이) */

    req->body = dst;      /* 완성된 본문 위치 */
    req->body_len = cl;   /* 본문 길이 */
    return 0;             /* 성공 */
}
