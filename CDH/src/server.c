/* server.c — 연결 하나를 처리하는 HTTP 서버 본체 (워커 스레드에서 돈다).
 *
 * 한 연결에서 요청 → 응답을 반복한다 (HTTP/1.1 keep-alive):
 *   1. tcp_recv_request 로 헤더 수신 (앞 요청이 남긴 파이프라이닝 잔여가 있으면 그 뒤에 이어서)
 *   2. 파싱(http_parser) → 본문 수신(http_body) → 응답
 *        GET / HEAD → 문서 루트의 정적 파일 (static_file + sendfile)
 *        POST       → 요청 내용을 그대로 보여 주는 echo
 *   3. 계속할지(keep) 결정: 클라이언트가 원함 + 정상 응답 + 요청 수 상한 미만 + 종료 중 아님 + 큐가 한가함
 *   4. 다음 요청 바이트가 이미 버퍼에 있거나(파이프라이닝) 소켓에 와 있으면 바로 반복,
 *      아무것도 없으면 idle poller 에 맡기고 워커를 놓아 준다 (쉬는 연결이 워커를 붙잡지 않게).
 *
 * 스레드 안전: 공유 상태는 시작 후 읽기만 하는 server_ctx 뿐. strerror 대신 strerror_r,
 * printf/fprintf 는 호출 단위로 잠금이 걸려 줄이 섞이지 않는다. */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 + XSI strerror_r(int 반환) 를 보이게 함. _GNU_SOURCE 는 켜지 않는다 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* macOS: _POSIX_C_SOURCE 를 켜면 숨겨지는 MSG_DONTWAIT 를 다시 보이게 */
#endif

#include "server.h"        /* serve_connection, server_on_idle_ready, server_ctx */
#include "http_body.h"     /* http_read_body (Content-Length 만큼 본문 받기) */
#include "http_parser.h"   /* http_parse_request, http_request */
#include "http_response.h" /* http_response, http_send_response, http_send_error_ex, http_reason, http_format_head */
#include "static_file.h"   /* static_lookup (URI → 파일) */
#include "tcp_server.h"    /* tcp_recv_request, tcp_sendv_all, tcp_close_graceful */

#include <errno.h>      /* errno, ETIMEDOUT, EAGAIN, EWOULDBLOCK, EINTR */
#include <stdio.h>      /* printf, fprintf, snprintf */
#include <stdlib.h>     /* free */
#include <string.h>     /* memcmp, memmove, strcspn, strerror_r */
#include <sys/socket.h> /* recv, MSG_PEEK, MSG_DONTWAIT */
#include <sys/uio.h>    /* struct iovec (echo 응답 세 조각) */
#include <time.h>       /* time (응답 Date) */
#include <unistd.h>     /* close */

#define RECV_BUF_SIZE 8192     /* 요청 헤더를 받을 버퍼 크기 (헤더 최대 8191바이트 + '\0'). 워커 스택에 둔다 */
#define CLIENT_TIMEOUT_MS 5000 /* 요청 수신, 응답 송신 각각의 전체 마감 시간 */
#define REJECT_DRAIN_MAX (64u * 1024u) /* 503 거절 전에 비울 수신 데이터 상한 (이미 와 있는 것만, 기다리지 않음) */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS 등 없는 곳: SIGPIPE 는 main 에서 이미 무시하므로 0 이어도 안전 */
#endif

/* 바쁠 때(큐 가득 / poller 가득 / 종료 중) 보내는 고정 응답 */
static const char RESP_503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

/* errno → 설명 문자열 (스레드 안전). strerror 는 정적 버퍼를 쓸 수 있어 여러 워커가 동시에 부르면 안전하지 않다.
 * _GNU_SOURCE 없이 _POSIX_C_SOURCE 200809L 이면 int 를 돌려주는 XSI 판 strerror_r 이 선택된다 */
static const char *err_str(int err, char *buf, size_t cap)  /* err: errno 값, buf/cap: 문자열을 쓸 호출자 버퍼 */
{
    if (strerror_r(err, buf, cap) != 0)        /* 모르는 번호 / 버퍼 부족 */
        snprintf(buf, cap, "errno %d", err);   /* 번호라도 남김 */
    return buf;                                /* 호출자 버퍼를 그대로 돌려줌 → printf 인자로 바로 쓰기 좋게 */
}

/* handle_request 의 결과: 연결을 어떻게 할지 */
enum {                                                      /* 연결 처리 결과 세 가지 */
    CONN_ABORT,    /* 응답 없이(또는 송신 실패로) 포기 → 바로 close */
    CONN_GRACEFUL, /* 응답을 보냈고 연결은 끝 → tcp_close_graceful (RST 로 응답이 잘리지 않게) */
    CONN_KEEP,     /* 응답을 보냈고 keep-alive → 다음 요청으로 */
};

/* 송신 결과 로그 (모든 응답 경로가 여기로). rc: 송신 함수의 반환값. 그대로 돌려준다 */
static int log_sent(const server_ctx *c, int rc, const char *peer, int status) /* c: 설정(quiet), peer: 로그용 "IP:포트", status: 보낸 상태 코드 */
{
    if (rc < 0) {                                                      /* 전송 실패 */
        char eb[128];                                                  /* strerror_r 용 버퍼 */
        fprintf(stderr, "[%s] send error: %s\n", peer, err_str(errno, eb, sizeof(eb))); /* 실패 이유 (EPIPE, ETIMEDOUT 등) */
    } else if (!c->quiet) {                                            /* 성공 + 조용 모드가 아니면 */
        printf("[%s] -> %d %s\n", peer, status, http_reason(status)); /* 결과 로그 */
    }
    return rc;                                                         /* 0 = 성공, -1 = 실패 */
}

/* 오류 응답: 상태 줄 + 헤더 + "<코드> <이유>\n" 본문. 항상 Connection: close (요청 경계를 믿을 수 없음).
 * head_only: HEAD 요청이면 본문 없이 머리만 (RFC 7231 4.3.2) */
static int send_error(const server_ctx *c, int cfd, const char *peer, int status, int head_only) /* status: 400, 413 등 */
{
    return log_sent(c, http_send_error_ex(cfd, status, NULL, head_only, CLIENT_TIMEOUT_MS), peer, status); /* 전송 + 로그 */
}

/* GET / HEAD → 정적 파일. 파일 내용은 유저 공간으로 읽지 않는다 (sendfile).
 * *keep: 들어올 때 keep-alive 로 답할지, 나갈 때 실제로 그렇게 답했는지 (오류 응답이면 0 으로 바꿈) */
static int serve_static(const server_ctx *c, int cfd, const char *peer, const http_request *req, int *keep) /* 반환: 0 = 보냄, -1 = 송신 실패 */
{
    static_result r;                                               /* 상태 코드 + 열린 파일 + 추가 헤더 */
    static_lookup(c->root_fd, req->uri, http_get_header(req, "If-Modified-Since"), &r); /* URI → 파일 찾기 (304 판단까지) */
    int head_only = req->method == HTTP_METHOD_HEAD;               /* HEAD: 머리만 (Content-Length 는 그대로) */

    if (r.status == 301 || r.status == 403 || r.status == 404) {   /* "자원" 단계의 결과: 요청은 끝까지 정상으로 읽었음 */
        /* 요청 경계가 확실하고 응답도 Content-Length 로 틀이 잡혀 있으니 연결을 유지해도 안전하다.
         * 404 가 잦은 페이지(없는 favicon 등)마다 재연결하지 않게 (nginx 도 4xx 뒤 keep-alive 유지) */
        char body[64];                                             /* 본문: 예) "404 Not Found\n" (http_send_error 와 같은 모양) */
        int blen = snprintf(body, sizeof(body), "%d %s\n", r.status, http_reason(r.status)); /* 상태 코드 + 이유 문구 */
        if (blen < 0 || (size_t)blen >= sizeof(body))              /* 사실상 없음 */
            blen = 0;                                              /* 본문 없이 보냄 */
        http_response res = {                                      /* 메모리 본문 응답 */
            .status = r.status,                                    /* 301 / 403 / 404 */
            .content_type = "text/plain; charset=utf-8",           /* 일반 텍스트 */
            .body = body,                                          /* 위에서 만든 한 줄 */
            .body_len = (size_t)blen,                              /* Content-Length */
            .keep_alive = *keep,                                   /* 들어온 결정 그대로 */
            .head_only = head_only,                                /* HEAD 면 머리만 */
            .extra_headers = r.headers,                            /* 301 의 Location */
        };
        return log_sent(c, http_send_response(cfd, &res, CLIENT_TIMEOUT_MS), peer, r.status); /* 전송 + 로그 */
    }
    if (r.status != 200 && r.status != 304) {                      /* 400 / 414 / 500 ... */
        *keep = 0;                                                 /* 이런 오류 뒤에는 close */
        return log_sent(c, http_send_error_ex(cfd, r.status, r.headers, head_only, CLIENT_TIMEOUT_MS), peer, r.status); /* Connection: close 고정 */
    }

    http_response res = {                                   /* 200 / 304 응답 내용 */
        .status = r.status,                                        /* 200 또는 304 */
        .content_type = r.content_type,                            /* 304 는 NULL → Content-Type 생략 */
        .body_len = r.size,                                        /* Content-Length = 파일 크기 */
        .keep_alive = *keep,                                       /* Connection 헤더 = 실제로 연결을 유지할지 */
        .head_only = head_only,                             /* HEAD 면 머리만 (Content-Length 는 파일 크기 그대로) */
        .extra_headers = r.headers,                                /* Last-Modified */
        .use_file = 1,                                             /* 본문 = 파일 (sendfile) */
        .file_fd = r.fd,                                           /* 304 면 -1 이지만 본문을 안 보내서 쓰이지 않음 */
        .file_offset = 0,                                          /* 파일 처음부터 (Range 미지원) */
    };
    int rc = log_sent(c, http_send_response(cfd, &res, CLIENT_TIMEOUT_MS), peer, r.status); /* 머리 + sendfile + 로그 */
    if (r.fd >= 0)                                                 /* 200 이면 열린 파일이 있음 */
        close(r.fd);                                               /* 보냈든 실패했든 파일은 닫는다 */
    return rc;                                                     /* 0 = 보냄, -1 = 송신 실패 */
}

/* POST → echo: 파싱 결과 요약 + 받은 본문 그대로. *keep 은 serve_static 과 같은 뜻 */
static int send_echo(const server_ctx *c, int cfd, const char *peer, const http_request *req, int *keep) /* 반환: 0 = 보냄, -1 = 송신 실패 */
{
    char summary[4096]; /* 요약 텍스트 (URI 최대 2048 + 나머지 여유) */
    int slen = snprintf(summary, sizeof(summary),               /* 요약 만들기 */
                        "method: %s\n"                          /* 메서드 */
                        "uri: %s\n"                             /* 요청 대상 */
                        "version: HTTP/1.%d\n"                  /* 버전 */
                        "host: %.256s\n"                        /* Host (너무 길면 256 글자까지만) */
                        "headers: %zu\n"                        /* 헤더 개수 */
                        "keep-alive: %s\n"                      /* 클라이언트가 원한 연결 유지 여부 */
                        "body-length: %zu\n"                    /* 본문 길이 */
                        "\n",                                   /* 빈 줄 뒤에 본문 원문 */
                        req->method_str, req->uri, req->version_minor,          /* 메서드, URI, 버전 */
                        req->host ? req->host : "(none)", req->header_count,   /* Host (HTTP/1.0 이면 없을 수 있음), 헤더 수 */
                        req->keep_alive ? "yes" : "no", req->body_len);        /* keep-alive, 본문 길이 */
    if (slen < 0) {                                              /* 형식 오류 (사실상 없음) */
        *keep = 0;                                               /* 오류 응답 → close */
        return send_error(c, cfd, peer, 500, 0);                 /* 500 Internal Server Error */
    }
    if ((size_t)slen >= sizeof(summary))                         /* 잘렸으면 */
        slen = (int)sizeof(summary) - 1;                         /* 실제로 버퍼에 들어간 길이만 보냄 */

    /* 본문이 두 조각(요약 + 받은 본문)이라 머리만 만들고 tcp_sendv_all 로 세 조각을 한 번에 보낸다 (합치는 복사 없음) */
    http_response res = {                                        /* 머리에 들어갈 내용 (본문은 아래 iov 로 따로) */
        .status = 200,                                           /* 200 OK */
        .content_type = "text/plain; charset=utf-8",             /* 일반 텍스트 */
        .body_len = (size_t)slen + req->body_len,                /* 본문 길이 = 요약 + 받은 본문 */
        .keep_alive = *keep,                                     /* Connection 헤더 = 실제로 연결을 유지할지 */
    };
    char head[HTTP_HEAD_MAX];                                    /* 응답 머리 */
    int hlen = http_format_head(head, sizeof(head), &res, time(NULL)); /* 지금 시각으로 머리 만들기 */
    if (hlen < 0) {                                              /* 만들 수 없음 (사실상 없음) */
        *keep = 0;                                               /* 오류 응답 → close */
        return send_error(c, cfd, peer, 500, 0);                 /* 500 Internal Server Error */
    }
    struct iovec iov[3] = {                                      /* 보낼 순서대로 세 조각 */
        {.iov_base = head, .iov_len = (size_t)hlen},             /* 머리 */
        {.iov_base = summary, .iov_len = (size_t)slen},          /* 요약 */
        {.iov_base = (void *)req->body, .iov_len = req->body_len}, /* 받은 본문 (없으면 길이 0 → 건너뜀) */
    };
    return log_sent(c, tcp_sendv_all(cfd, iov, 3, CLIENT_TIMEOUT_MS), peer, 200); /* 전송 + 로그 */
}

/* 오류 응답을 보내고 연결 처리 방법을 돌려준다 (보냈으면 graceful, 실패면 abort) */
static int error_and_close(const server_ctx *c, int cfd, const char *peer, int status, int head_only) /* 반환: CONN_GRACEFUL / CONN_ABORT */
{
    return send_error(c, cfd, peer, status, head_only) == 0 ? CONN_GRACEFUL : CONN_ABORT; /* 오류 뒤엔 keep-alive 없음 */
}

/* 요청 하나 처리: 헤더 수신 → 파싱 → 본문 수신 → 응답 → keep 결정 → 다음 요청 잔여 정리.
 * buf[0..*used) 에는 앞 요청이 남긴 바이트가 있다. CONN_KEEP 이면 *used 를 새 잔여 길이로 바꾼다 */
static int handle_request(const server_ctx *c, conn_job *job, char *buf, size_t cap, size_t *used) /* 반환: CONN_ABORT / CONN_GRACEFUL / CONN_KEEP */
{
    int cfd = job->fd;                 /* 클라이언트 소켓 */
    const char *peer = job->peer;      /* 로그용 "IP:포트" */
    char eb[128];                      /* strerror_r 용 버퍼 */
    size_t header_len;                 /* 헤더 길이 (못 찾으면 0) */
    ssize_t n = tcp_recv_request(cfd, buf, cap, *used, CLIENT_TIMEOUT_MS, &header_len); /* 잔여 뒤에 이어서 헤더 끝까지 */
    *used = 0;                         /* 잔여는 이제 buf[0..n) 안에 포함됨 */
    if (n < 0) {                                                             /* 수신 실패 */
        if (errno == ETIMEDOUT)                                              /* 마감 시간 안에 헤더가 다 안 옴 */
            fprintf(stderr, "[%s] recv timeout\n", peer);                    /* 타임아웃 로그 (slowloris 등) */
        else                                                                 /* 그 밖의 오류 (ECONNRESET 등) */
            fprintf(stderr, "[%s] recv error: %s\n", peer, err_str(errno, eb, sizeof(eb))); /* 오류 이유 로그 */
        return CONN_ABORT;                                                   /* 응답 없이 포기 */
    }
    if (n == 0) {                                          /* 아무것도 안 보내고 닫음 */
        if (!c->quiet && job->nreq == 0)                   /* 첫 요청 전이면 로그 (keep-alive 뒤의 정상 종료는 조용히) */
            printf("[%s] closed without data\n", peer);    /* 로그만 남김 */
        return CONN_ABORT;                                 /* 응답할 대상이 없음 */
    }

    /* HEAD 요청인지: 파싱 전에 오류가 나도(431, 414 ...) 본문 없이 답해야 해서 첫 5바이트로 미리 판단 */
    int head_only = (size_t)n >= 5 && memcmp(buf, "HEAD ", 5) == 0; /* "HEAD " 로 시작하면 1 */

    if (header_len == 0) {                                 /* 헤더 끝을 못 찾고 수신이 끝남 */
        if ((size_t)n >= cap - 1) {                        /* 버퍼가 꽉 참 = 헤더가 너무 큼 */
            fprintf(stderr, "[%s] header too large (> %zu bytes)\n", peer, cap - 1); /* 로그 (상한 = 버퍼 크기 - 1) */
            return error_and_close(c, cfd, peer, 431, head_only); /* 431 Request Header Fields Too Large */
        }
        fprintf(stderr, "[%s] incomplete request (%zd bytes)\n", peer, n); /* 헤더 도중 상대가 송신을 닫음 */
        return error_and_close(c, cfd, peer, 400, head_only); /* 400 Bad Request */
    }

    /* 첫 줄(요청 라인)만 로그 — 파싱이 '\0' 을 써 넣기 전에 찍는다 */
    if (!c->quiet) {                                        /* 조용 모드가 아니면 */
        size_t line_len = strcspn(buf, "\r\n");      /* 요청 라인 길이 */
        if (line_len > 200)                          /* 긴 URI 로 로그가 도배되지 않게 */
            line_len = 200;                                 /* 200 글자까지만 */
        printf("[%s] %.*s\n", peer, (int)line_len, buf); /* %.*s: line_len 글자까지만 출력 */
    }

    http_request req;                                     /* 파싱 결과 (buf 안을 가리키는 포인터들) */
    int rc = http_parse_request(buf, header_len, &req);   /* 요청 라인 + 헤더 파싱 (buf[0..header_len) 만 건드림) */
    if (rc != 0)                                          /* 문법 오류·미지원 등 */
        return error_and_close(c, cfd, peer, rc, head_only); /* 파서가 정한 상태 코드로 응답 후 닫기 */

    char *heap = NULL;                                    /* 본문이 버퍼에 안 들어가면 http_read_body 가 malloc 한 포인터 */
    rc = http_read_body(cfd, &req, buf, cap, (size_t)n, header_len, CLIENT_TIMEOUT_MS, &heap); /* 본문 마저 받기 (정확히 필요한 만큼만) */
    if (rc < 0) {                                         /* 본문 수신 중 타임아웃·끊김 (heap 은 이미 반납됨) */
        fprintf(stderr, "[%s] body recv failed: %s\n", peer, err_str(errno, eb, sizeof(eb))); /* 로그 */
        return CONN_ABORT;                                /* 요청이 덜 왔으니 응답 없이 닫기 */
    }
    if (rc > 0)                                           /* 411 / 413 / 417 / 500 (heap 은 NULL) */
        return error_and_close(c, cfd, peer, rc, head_only); /* 오류 응답 후 닫기 (본문을 안 읽었으니 경계를 모름) */

    /* keep-alive 결정 (응답 머리의 Connection 이 이 값과 같아야 하므로 보내기 전에):
     * 클라이언트가 원함 + 요청 수 상한 미만 + 종료 중 아님 + 큐가 3/4 미만 (바쁘면 연결을 돌려 다른 클라이언트에게 워커를 양보) */
    int keep = req.keep_alive && job->nreq + 1 < c->keepalive_max; /* 클라이언트 의사 + 연결당 요청 수 상한 */
    if (keep && c->pool != NULL)                          /* 풀 없이 부르는 경우(테스트)는 여기까지만 */
        keep = !tp_stopping(c->pool) && tp_queued(c->pool) * 4 < tp_capacity(c->pool) * 3; /* 종료 중 아님 + 큐 75% 미만 */

    if (req.method == HTTP_METHOD_POST)                   /* POST → echo */
        rc = send_echo(c, cfd, peer, &req, &keep);          /* 요약 + 받은 본문을 그대로 돌려줌 */
    else                                                  /* GET / HEAD → 정적 파일 (오류 응답이면 keep 이 0 이 됨) */
        rc = serve_static(c, cfd, peer, &req, &keep);       /* 문서 루트에서 파일 찾아 sendfile */
    job->nreq++;                                          /* 이 연결에서 처리한 요청 수 */
    free(heap);                                           /* 힙 본문 반납 (NULL 이면 아무 일 없음). 송신 뒤라 안전 */
    if (rc < 0)                                           /* 송신 실패 */
        return CONN_ABORT;                                /* 상대가 끊었거나 타임아웃 → 바로 닫기 */
    if (!keep)                                            /* 이번 응답으로 끝 */
        return CONN_GRACEFUL;                             /* RST 없이 닫기 */

    /* 다음 요청 잔여: buf = [헤더 | 본문(버퍼 안이면) | 다음 요청 앞부분 ...] (0..n)
     * http_read_body 는 본문을 정확히 Content-Length 만큼만 받는다:
     *   - 본문 없음(GET) / Content-Length 0 : body_len 0 → 헤더 뒤 전부가 다음 요청
     *   - 본문이 이미 다 와 있었음          : 헤더 뒤 바이트 > body_len → 그 차이가 다음 요청
     *   - 본문을 더 받았음(버퍼 안 / 힙)     : 헤더 뒤 바이트 < body_len → 잔여 없음 (recv_exact 는 넘치게 안 받음) */
    size_t after = (size_t)n - header_len;                /* 헤더 뒤에 같이 들어온 바이트 */
    if (after > req.body_len) {                           /* 본문 뒤에 다음 요청이 붙어 있음 (파이프라이닝) */
        size_t left = after - req.body_len;               /* 다음 요청 바이트 수 */
        memmove(buf, buf + header_len + req.body_len, left); /* 버퍼 앞으로 (응답을 다 보낸 뒤라 req 포인터가 깨져도 됨) */
        *used = left;                                     /* 다음 handle_request 가 이 뒤에 이어서 받음 */
    }
    return CONN_KEEP;                                     /* 연결 유지 */
}

void serve_connection(conn_job *job, void *ctx)             /* tp_job_fn: 워커가 job 하나마다 호출. job->fd 소유 */
{
    const server_ctx *c = ctx;    /* 공유 설정 (읽기만) */
    char buf[RECV_BUF_SIZE];      /* 요청 버퍼 (워커 스택 8KB). 작은 본문·파이프라이닝 잔여도 여기 */
    size_t used = 0;              /* buf 앞에 남아 있는 다음 요청 바이트 수 (poller 에서 돌아온 연결은 항상 0) */

    for (;;) {                            /* keep-alive: 연결이 끝나거나 poller 로 넘길 때까지 */
        int r = handle_request(c, job, buf, sizeof(buf), &used); /* 요청 하나 처리 */
        if (r == CONN_GRACEFUL) {         /* 응답을 보냈고 끝 */
            tcp_close_graceful(job->fd);  /* 안 읽은 데이터가 있으면 FIN 먼저 → RST 로 응답이 잘리지 않게 */
            return;                       /* 워커는 풀로 돌아감 */
        }
        if (r == CONN_ABORT) {            /* 포기 */
            close(job->fd);               /* 기다리지 않고 바로 닫음 */
            return;                                         /* 워커는 풀로 돌아감 */
        }
        if (used > 0)                     /* 다음 요청 일부가 이미 버퍼에 있음 → 바로 이어서 */
            continue;                                       /* poll·recv 없이 바로 다음 handle_request */

        /* 버퍼는 비었다. 소켓에 다음 요청이 이미 와 있으면 poller 를 거치지 않고 바로 처리 (왕복 절약) */
        char ch;                          /* 엿보기용 1바이트 (큐에서 빼지 않음) */
        ssize_t p;                        /* recv 결과: >0 데이터 있음, 0 FIN, -1 없음/오류 */
        do                                                  /* 시그널에 끊기면 다시 시도하는 반복 */
            p = recv(job->fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT); /* PEEK: 보기만 / DONTWAIT: 없으면 바로 EAGAIN */
        while (p < 0 && errno == EINTR);  /* 시그널로 끊기면 다시 */
        if (p > 0)                        /* 다음 요청 도착 */
            continue;                     /* 바로 다음 handle_request (tcp_recv_request 가 받아 감) */
        if (p == 0) {                     /* 상대가 닫음 (FIN) */
            close(job->fd);               /* 우리도 닫음 (보낼 것 없음) */
            return;                                         /* 워커는 풀로 돌아감 */
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && c->idle != NULL && /* 데이터 없음(정상) + poller 가 있으면 */
            idle_park(c->idle, job) == 0) /* 아직 아무것도 없음 → poller 에 맡기고 워커를 놓아 줌 */
            return;                       /* fd 는 이제 poller 소유 → 손대지 않는다 */
        close(job->fd);                   /* 맡기기 실패(가득 참·종료 중) 또는 소켓 오류 → 닫음 */
        return;                           /* 워커는 풀로 돌아감 */
    }
}

/* idle poller 스레드에서 호출: 쉬던 연결에 다음 요청이 왔다(또는 상대가 닫았다) → 워커에게 다시 맡긴다.
 * poller 스레드는 모든 쉬는 연결을 혼자 보므로 여기서 절대 오래 머물면 안 된다 (tp_submit 은 기다리지 않음) */
void server_on_idle_ready(const conn_job *job, void *ctx)   /* idle_ready_fn: poller 스레드에서 호출, job->fd 소유 */
{
    const server_ctx *c = ctx;            /* 공유 설정 (읽기만) */
    if (tp_submit(c->pool, job) == 0)     /* 워커에게 다시 (fd 소유권이 풀로) */
        return;                           /* 워커가 handle_request 에서 recv → FIN 이면 n == 0 으로 조용히 닫음 */
    int err = errno;                      /* EAGAIN (큐 가득) / ECANCELED (종료 중) */
    if (!c->quiet) {                      /* 조용 모드가 아니면 */
        char eb[128];                     /* strerror_r 용 버퍼 */
        fprintf(stderr, "[%s] busy -> 503: %s\n", job->peer, err_str(err, eb, sizeof(eb))); /* 거절 이유 로그 */
    }
    server_reject_busy(job->fd);          /* 요청이 이미 와 있음 → 그냥 close 하면 RST. 비우고 503 후 닫음 (기다리지 않음) */
}

/* 바빠서 처리할 수 없는 연결을 닫는다. 어느 스레드에서 불러도 절대 기다리지 않는다.
 *
 * 왜 그냥 close 하면 안 되나: 수신 버퍼에 아직 안 읽은 데이터(= 클라이언트가 보낸 요청)가 남은 채 close 하면
 * 커널은 FIN 대신 RST 를 보낸다. RST 를 받은 클라이언트는 먼저 도착한 503 까지 버리고 ECONNRESET 을 본다.
 * → 이미 와 있는 바이트를 먼저 읽어 버려서(논블로킹) 수신 버퍼를 비운 뒤 503 을 보내고 닫는다.
 *   상대가 아무것도 안 보내고 이미 닫았으면(FIN 만 옴) 503 없이 그냥 닫는다.
 * 한계: 닫은 "뒤에" 도착하는 바이트는 여전히 RST 를 부른다 (기다리면 호출 스레드가 막히므로 감수) */
void server_reject_busy(int fd)
{
    char junk[4096];                      /* 읽어서 버릴 임시 버퍼 */
    size_t total = 0;                     /* 버린 바이트 수 */
    int eof = 0;                          /* 상대가 송신을 닫았는지 */
    while (total < REJECT_DRAIN_MAX) {    /* 상한까지 (악의적으로 많이 보내도 오래 안 붙잡힘) */
        ssize_t n = recv(fd, junk, sizeof(junk), MSG_DONTWAIT); /* 이미 와 있는 것만 */
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)      /* 시그널 → 재시도 */
            continue;
        if (n == 0)                       /* FIN */
            eof = 1;
        break;                            /* EAGAIN(다 비움) / FIN / 오류 */
    }
    if (!(eof && total == 0)) {           /* 상대가 아무것도 안 보내고 닫은 게 아니면 503 */
        ssize_t r = send(fd, RESP_503, sizeof(RESP_503) - 1, MSG_DONTWAIT | MSG_NOSIGNAL); /* 송신 버퍼가 차 있어도 기다리지 않음 */
        (void)r;                          /* 실패해도(EPIPE 등) 닫는 건 같음 */
    }
    close(fd);                            /* 수신 버퍼가 비었으니 FIN → 503 이 클라이언트에 그대로 도착 */
}

/* accept 스레드에서 호출: 새 연결을 어디로 보낼지 정한다 (fd 소유권을 넘겨받음, 절대 기다리지 않음).
 *   - 요청 바이트가 벌써 와 있음 → 바로 워커 (poller 를 거치는 왕복을 아낌)
 *   - 아직 아무것도 없음        → idle poller 에 맡김: 읽을 게 생기면 그때 워커로.
 *     예전처럼 곧장 워커에 넣으면, 연결만 맺고 안 보내는 클라이언트가 워커 하나를 CLIENT_TIMEOUT_MS 동안
 *     recv 에서 붙잡아, 워커 수만큼의 빈 연결로 서버 전체가 멈췄다.
 *   - 상대가 벌써 닫음 → 닫기 / 맡길 곳이 가득 → 503 */
void server_dispatch_new(const conn_job *job, void *ctx)
{
    const server_ctx *c = ctx;            /* 공유 설정 (읽기만) */
    char ch;                              /* 엿보기용 1바이트 (큐에서 빼지 않음) */
    ssize_t p;
    do
        p = recv(job->fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT); /* 와 있는지 보기만 */
    while (p < 0 && errno == EINTR);

    if (p == 0) {                         /* 연결만 하고 바로 닫음 */
        if (!c->quiet)
            printf("[%s] closed without data\n", job->peer);
        close(job->fd);
        return;
    }
    if (p < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { /* ECONNRESET 등: 이미 망가진 연결 */
        close(job->fd);
        return;
    }

    const char *why;                      /* 거절 이유 (로그용) */
    if (p < 0 && c->idle != NULL) {       /* 아직 데이터 없음 → poller 가 대신 기다림 */
        if (idle_park(c->idle, job) == 0)
            return;                       /* fd 는 이제 poller 소유 */
        why = errno == ECANCELED ? "shutting down" : "idle poller full";
    } else {                              /* 데이터 있음 (또는 poller 없이 부르는 경우) → 워커 */
        if (tp_submit(c->pool, job) == 0)
            return;                       /* fd 는 이제 풀 소유 */
        why = errno == ECANCELED ? "shutting down" : "queue full";
    }
    if (!c->quiet)
        fprintf(stderr, "[%s] %s -> 503\n", job->peer, why);
    server_reject_busy(job->fd);          /* 503 후 RST 없이 닫기 */
}
