/* main.c — 서버 프로그램의 진입점.
 * 명령줄 인자 처리, 시그널 설정, 연결을 하나씩 받아 처리하는 메인 루프,
 * 그리고 요청을 파싱(http_parser)·본문 수신(http_body)한 뒤 응답을 보내는 일을 맡는다.
 * 지금 응답은 요청 내용을 그대로 보여 주는 echo (정적 파일·프록시는 다음 단계). */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 함수(sigaction, nanosleep 등)를 헤더에서 보이게 함. include 보다 먼저 */

#include "http_body.h"     /* http_read_body (Content-Length 만큼 본문 받기) */
#include "http_parser.h"   /* http_parse_request, http_request (요청 헤더 파싱) */
#include "http_response.h" /* http_response, http_send_response, http_send_error, http_reason */
#include "tcp_server.h" /* tcp_listen, tcp_accept, tcp_recv_request, tcp_close_graceful */

#include <errno.h>      /* errno, EINTR, ETIMEDOUT, EMFILE, ENFILE */
#include <fcntl.h>      /* open, O_RDONLY (예비 fd 용) */
#include <signal.h>     /* sigaction, SIGINT/SIGTERM/SIGPIPE, sig_atomic_t, SIG_IGN */
#include <stdio.h>      /* printf, fprintf, perror, fflush */
#include <stdlib.h>     /* strtol, EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* memset, strcmp, strcspn, strerror */
#include <sys/socket.h> /* accept (handle_accept_error 에서 직접 호출) */
#include <time.h>       /* nanosleep, struct timespec */
#include <unistd.h>     /* close */

#define DEFAULT_PORT 8080      /* 포트 인자를 안 주면 쓰는 기본 포트 */
#define BACKLOG 128            /* listen() 대기 큐 크기: handshake 끝나고 accept 를 기다리는 연결 최대 수 */
#define RECV_BUF_SIZE 8192     /* 요청 헤더를 받을 버퍼 크기 (헤더 최대 8191바이트 + '\0') */
#define CLIENT_TIMEOUT_MS 5000 /* 요청 수신, 응답 송신 각각의 전체 마감 시간 */

static volatile sig_atomic_t g_running = 1; /* 서버 실행 중 플래그. 시그널 핸들러가 0 으로 바꾸면 메인 루프 종료.
                                               volatile: 컴파일러가 값을 캐시하지 않고 매번 메모리에서 읽게 함
                                               sig_atomic_t: 시그널 핸들러에서 안전하게 쓸 수 있는 정수 타입 */

static void on_shutdown_signal(int sig) /* SIGINT(Ctrl+C), SIGTERM(kill) 을 받으면 커널이 호출하는 함수 */
{
    (void)sig;     /* 어떤 시그널인지는 안 씀. "안 쓰는 인자" 경고 제거용 */
    g_running = 0; /* 종료 요청 표시만 하고 끝 (핸들러 안에서는 printf 같은 복잡한 일을 하면 안전하지 않음) */
}

static int install_signal_handlers(void) /* 시그널별 동작 등록. 성공 0, 실패 -1 */
{
    struct sigaction sa;                   /* 시그널 처리 방법을 담는 구조체 */
    memset(&sa, 0, sizeof(sa));            /* 모든 필드 0 으로 초기화 */
    sa.sa_handler = on_shutdown_signal;    /* 시그널이 오면 이 함수를 호출 */
    sigemptyset(&sa.sa_mask);              /* 핸들러 실행 중에 추가로 막을 시그널: 없음 */
    sa.sa_flags = 0; /* SA_RESTART 미사용 → 블로킹 accept() 가 EINTR 로 깨어남 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) /* Ctrl+C 와 kill 에 위 설정 적용 */
        return -1;                         /* 등록 실패 */

    /* 쓰는 도중 클라이언트가 끊으면 SIGPIPE 로 프로세스가 죽는 것을 방지 */
    sa.sa_handler = SIG_IGN;               /* 처리 방식: 무시 */
    if (sigaction(SIGPIPE, &sa, NULL) < 0) /* SIGPIPE 무시 등록 → send 는 대신 -1/EPIPE 를 반환 */
        return -1;                         /* 등록 실패 */
    return 0;                              /* 성공 */
}

static int parse_port(const char *s, uint16_t *out) /* 문자열 → 포트 번호(1~65535). 성공 0, 잘못된 값 -1 */
{
    char *end = NULL;                  /* strtol 이 숫자 다음 위치를 여기에 알려 줌 */
    errno = 0;                         /* strtol 은 범위 초과 시에만 errno 를 바꾸므로 미리 0 으로 */
    long v = strtol(s, &end, 10);      /* 10진수로 변환 */
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535) /* 범위 초과 / 숫자 없음 / 뒤에 문자 남음("80a") / 포트 범위 밖 */
        return -1;                     /* 잘못된 포트 */
    *out = (uint16_t)v;                /* 검증된 값을 저장 */
    return 0;                          /* 성공 */
}

static int g_quiet = 0; /* -q: 요청마다 찍는 로그 끔 (부하 테스트용) */

/* 응답 전송 + 결과 로그. 성공 0, 실패 -1 */
static int send_resp(int cfd, const char *peer, const http_response *res) /* res: 보낼 응답 */
{
    if (http_send_response(cfd, res, CLIENT_TIMEOUT_MS) < 0) {           /* 마감 시간 안에 머리 + 본문 전송 시도 */
        fprintf(stderr, "[%s] send error: %s\n", peer, strerror(errno)); /* 실패 이유 출력 (EPIPE, ETIMEDOUT 등) */
        return -1;                                                       /* 실패 */
    }
    if (!g_quiet)                                                        /* 조용 모드가 아니면 */
        printf("[%s] -> %d %s\n", peer, res->status, http_reason(res->status)); /* 결과 로그 */
    return 0;                                                            /* 성공 */
}

/* 오류 응답: 상태 줄 + 헤더 + "<코드> <이유>\n" 본문 (오류 뒤에는 연결을 닫는다) */
static int send_error(int cfd, const char *peer, int status) /* status: 400, 413 등 */
{
    if (http_send_error(cfd, status, NULL, CLIENT_TIMEOUT_MS) < 0) {     /* 오류 응답 전송 */
        fprintf(stderr, "[%s] send error: %s\n", peer, strerror(errno)); /* 실패 이유 출력 */
        return -1;                                                       /* 실패 */
    }
    if (!g_quiet)                                                        /* 조용 모드가 아니면 */
        printf("[%s] -> %d %s\n", peer, status, http_reason(status));   /* 결과 로그 */
    return 0;                                                            /* 성공 */
}

/* 정상 요청에 대한 echo 응답: 파싱 결과 요약 + 받은 본문 그대로.
 * (정적 파일·동적 처리·프록시는 다음 단계에서 이 자리를 대체한다) */
static int send_echo(int cfd, const char *peer, const http_request *req) /* req: 파싱 + 본문 수신이 끝난 요청 */
{
    char summary[4096]; /* 요약 텍스트 (URI 최대 2048 + 나머지 여유) */
    int slen = snprintf(summary, sizeof(summary),               /* 요약 만들기 */
                        "method: %s\n"                          /* 메서드 */
                        "uri: %s\n"                             /* 요청 대상 */
                        "version: HTTP/1.%d\n"                  /* 버전 */
                        "host: %.256s\n"                        /* Host (너무 길면 256 글자까지만) */
                        "headers: %zu\n"                        /* 헤더 개수 */
                        "keep-alive: %s\n"                      /* 클라이언트가 원한 연결 유지 여부 (아직 실제 유지는 안 함) */
                        "body-length: %zu\n"                    /* 본문 길이 */
                        "\n",                                   /* 빈 줄 뒤에 본문 원문 */
                        req->method_str, req->uri, req->version_minor,              /* 메서드, URI, 버전 */
                        req->host ? req->host : "(none)", req->header_count,       /* Host (HTTP/1.0 이면 없을 수 있음), 헤더 수 */
                        req->keep_alive ? "yes" : "no", req->body_len);            /* keep-alive, 본문 길이 */
    if (slen < 0)                                                /* 형식 오류 (사실상 없음) */
        return send_error(cfd, peer, 500);                       /* 서버 오류로 응답 */
    if ((size_t)slen >= sizeof(summary))                         /* 잘렸으면 */
        slen = (int)sizeof(summary) - 1;                         /* 실제로 버퍼에 들어간 길이만 보냄 */

    size_t total = (size_t)slen + req->body_len;                 /* 응답 본문 = 요약 + 받은 본문 */
    char *out = malloc(total);                                   /* 두 조각을 이어 붙일 버퍼 (total >= 요약 길이 > 0) */
    if (out == NULL)                                             /* 메모리 부족 */
        return send_error(cfd, peer, 500);                       /* 500 Internal Server Error */
    memcpy(out, summary, (size_t)slen);                          /* 앞부분: 요약 */
    if (req->body_len > 0)                                       /* 본문이 있으면 */
        memcpy(out + slen, req->body, req->body_len);            /* 뒷부분: 받은 본문 그대로 */

    http_response res = {                                        /* 응답 내용 */
        .status = 200,                                           /* 200 OK */
        .content_type = "text/plain; charset=utf-8",             /* 일반 텍스트 */
        .body = out,                                             /* 본문 */
        .body_len = total,                                       /* 본문 길이 */
        .keep_alive = 0,                                         /* keep-alive 는 동시성 단계에서 같이 다룸 → 지금은 close */
        .extra_headers = NULL,                                   /* 추가 헤더 없음 */
    };
    int rc = send_resp(cfd, peer, &res);                         /* 전송 + 로그 */
    free(out);                                                   /* 임시 버퍼 반납 */
    return rc;                                                   /* 0 = 성공, -1 = 전송 실패 */
}

/* 연결 하나 처리: 헤더 수신 → 파싱 → 본문 수신 → 응답
 * 반환: 응답까지 보냈으면 0, 타임아웃·오류·빈 연결이면 -1 */
static int handle_client(int cfd, const char *peer) /* cfd: 클라이언트 연결 소켓, peer: 로그용 "IP:포트" */
{
    char buf[RECV_BUF_SIZE];  /* 요청을 받을 버퍼 (스택에 8KB). 작은 본문도 여기 이어서 받음 */
    size_t header_len;        /* 헤더 길이를 tcp_recv_request 가 채워 줌 (못 찾으면 0) */
    ssize_t n = tcp_recv_request(cfd, buf, sizeof(buf), CLIENT_TIMEOUT_MS, &header_len); /* 헤더 끝까지 받기 */
    if (n < 0) {                                                             /* 수신 실패 */
        if (errno == ETIMEDOUT)                                              /* 마감 시간 안에 헤더가 다 안 옴 */
            fprintf(stderr, "[%s] recv timeout\n", peer);                    /* 타임아웃 로그 */
        else                                                                 /* 그 밖의 오류 */
            fprintf(stderr, "[%s] recv error: %s\n", peer, strerror(errno)); /* 오류 이유 로그 */
        return -1;                                                           /* 응답 없이 포기 */
    }
    if (n == 0) {                                          /* 아무것도 안 보내고 연결을 닫은 클라이언트 */
        if (!g_quiet)                                      /* 조용 모드가 아니면 */
            printf("[%s] closed without data\n", peer);    /* 로그만 남김 */
        return -1;                                         /* 응답할 대상이 없음 */
    }

    /* 헤더 끝(빈 줄)을 못 찾은 경우: 잘린 요청을 정상으로 처리하지 않는다 */
    if (header_len == 0) {                                 /* 헤더 끝을 못 찾고 수신이 끝남 */
        if ((size_t)n >= sizeof(buf) - 1) { /* 버퍼가 꽉 참 = 헤더가 너무 큼 */
            fprintf(stderr, "[%s] header too large (> %zu bytes)\n", peer, sizeof(buf) - 1); /* 로그 */
            return send_error(cfd, peer, 431);             /* 431 Request Header Fields Too Large */
        }
        /* 헤더가 끝나기 전에 상대가 송신을 닫음(half-close) */
        fprintf(stderr, "[%s] incomplete request (%zd bytes)\n", peer, n); /* 로그 */
        return send_error(cfd, peer, 400);                 /* 400 Bad Request */
    }

    /* 첫 줄(요청 라인)만 로그 — 파싱이 '\0' 을 써 넣기 전에 찍는다 */
    if (!g_quiet) {                                  /* 조용 모드가 아니면 */
        size_t line_len = strcspn(buf, "\r\n");      /* 첫 \r 또는 \n 까지의 길이 = 요청 라인("GET / HTTP/1.1") 길이 */
        if (line_len > 200)                          /* 긴 URI 로 로그가 도배되지 않게 */
            line_len = 200;                          /* 200 글자까지만 */
        printf("[%s] %.*s\n", peer, (int)line_len, buf); /* %.*s: 문자열을 line_len 글자까지만 출력 */
    }

    http_request req;                                     /* 파싱 결과 (buf 안을 가리키는 포인터들) */
    int rc = http_parse_request(buf, header_len, &req);   /* 요청 라인 + 헤더 파싱 */
    if (rc != 0)                                          /* 문법 오류·미지원 등 */
        return send_error(cfd, peer, rc);                 /* 파서가 정한 상태 코드로 응답 */

    char *heap = NULL;                                    /* 본문이 버퍼에 안 들어가면 http_read_body 가 malloc 한 포인터 */
    rc = http_read_body(cfd, &req, buf, sizeof(buf), (size_t)n, header_len, CLIENT_TIMEOUT_MS, &heap); /* 본문 마저 받기 */
    if (rc < 0) {                                         /* 본문 수신 중 타임아웃·끊김 (heap 은 이미 반납됨) */
        fprintf(stderr, "[%s] body recv failed: %s\n", peer, strerror(errno)); /* 로그 */
        return -1;                                        /* 응답 없이 닫기 */
    }
    if (rc > 0)                                           /* 411 / 413 / 417 / 500 */
        return send_error(cfd, peer, rc);                 /* 오류 응답 (heap 은 NULL) */

    rc = send_echo(cfd, peer, &req);                      /* 정상 요청 → echo 응답 */
    free(heap);                                           /* 힙 본문 반납 (NULL 이면 아무 일 없음) */
    return rc;                                            /* 0 = 응답 보냄, -1 = 전송 실패 */
}

/* ---- accept() 실패 처리 ----
 * fd 가 바닥나면(EMFILE/ENFILE) 대기 중인 연결이 계속 남아 있어서 accept 가
 * 즉시 다시 실패한다 → 그냥 continue 하면 CPU 100% 로 헛돈다.
 * 예비 fd 하나를 미리 열어 두었다가, 이때 잠깐 닫고 accept → close 로 그 연결을
 * 정리한 뒤 다시 예비 fd 를 연다. (libev/nginx 계열에서 쓰는 방법) */
static int g_spare_fd = -1; /* 예비로 잡아 둔 fd. -1 = 아직 없음 */

static void handle_accept_error(int lfd) /* tcp_accept 가 EINTR 이외의 이유로 실패했을 때 호출 */
{
    int err = errno;                                         /* 아래 함수들이 errno 를 바꿀 수 있으니 먼저 저장 */
    if ((err == EMFILE || err == ENFILE) && g_spare_fd >= 0) { /* 프로세스(EMFILE) 또는 시스템 전체(ENFILE) fd 고갈이고 예비 fd 가 있으면 */
        close(g_spare_fd);                                   /* 예비 fd 를 반납해서 자리 하나 확보 */
        int cfd = accept(lfd, NULL, NULL);                   /* 대기 중인 연결 하나를 꺼냄 (주소는 필요 없어서 NULL) */
        if (cfd >= 0)                                        /* 꺼냈으면 */
            close(cfd); /* 처리할 여력이 없으니 바로 끊는다 (클라이언트는 연결 거부를 봄) */
        g_spare_fd = open("/dev/null", O_RDONLY);            /* 예비 fd 를 다시 확보 (/dev/null 은 아무 데나 쓸 수 있는 가짜 파일) */
        fprintf(stderr, "accept: %s -> dropped one connection\n", strerror(err)); /* 로그: 연결 하나를 버렸음 */
        return;                                              /* 메인 루프로 돌아감 */
    }
    fprintf(stderr, "accept: %s\n", strerror(err));          /* 그 밖의 accept 오류 로그 */
    /* ENOBUFS/ENOMEM 같은 일시적 자원 부족: 잠깐 쉬어서 헛돌지 않게 한다 */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000}; /* 0.1초 = 1억 나노초 */
    nanosleep(&ts, NULL);                                    /* 0.1초 잠듦 */
}

int main(int argc, char *argv[]) /* argc: 인자 개수, argv: 인자 문자열 배열 (argv[0] = 프로그램 이름) */
{
    uint16_t port = DEFAULT_PORT;              /* 기본 포트로 시작 */
    for (int i = 1; i < argc; i++) {           /* argv[1] 부터 인자를 하나씩 확인 */
        if (strcmp(argv[i], "-q") == 0) {      /* "-q" 면 */
            g_quiet = 1;                       /* 조용 모드 켬 */
        } else if (parse_port(argv[i], &port) < 0) {              /* 아니면 포트로 해석, 실패하면 */
            fprintf(stderr, "usage: %s [-q] [port]\n", argv[0]);  /* 사용법 출력 */
            return EXIT_FAILURE;                                  /* 종료 코드 1 */
        }
    }

    if (install_signal_handlers() < 0) { /* Ctrl+C·kill 처리, SIGPIPE 무시 등록 */
        perror("sigaction");             /* 실패 이유 출력 */
        return EXIT_FAILURE;             /* 종료 */
    }

    int lfd = tcp_listen(port, BACKLOG); /* socket → bind → listen 으로 리스닝 소켓 준비 */
    if (lfd < 0)                         /* 실패(포트 사용 중 등). 에러 메시지는 tcp_listen 이 이미 출력 */
        return EXIT_FAILURE;             /* 종료 */

    g_spare_fd = open("/dev/null", O_RDONLY); /* fd 고갈 대비용 예비 fd 를 미리 하나 잡아 둠 */

    /* stdout 버퍼링은 기본값 사용: 터미널이면 줄 단위, 파일·파이프면 블록 단위로
     * 모아서 쓴다(요청마다 write 시스템콜 X). 시작 메시지만 즉시 내보낸다. */
    printf("listening on 0.0.0.0:%u (Ctrl+C to stop)%s\n", (unsigned)port, /* 시작 메시지 */
           g_quiet ? " [quiet]" : "");                                      /* 조용 모드면 표시 추가 */
    fflush(stdout);                                                         /* 버퍼에 쌓인 시작 메시지를 즉시 출력 */

    while (g_running) {                              /* 종료 시그널이 올 때까지 반복 (한 번에 연결 하나 = 반복 서버) */
        char peer[64];                               /* 클라이언트 "IP:포트" 문자열 (로그용) */
        int cfd = tcp_accept(lfd, peer, sizeof(peer)); /* 연결 하나 받기. 대기 연결이 없으면 여기서 잠듦 */
        if (cfd < 0) {                               /* accept 실패 */
            if (errno == EINTR)                      /* 시그널 때문에 깨어남 (예: Ctrl+C) */
                continue; /* 시그널 → 루프 조건에서 g_running 확인 */
            handle_accept_error(lfd);                /* fd 고갈 등 나머지 오류 처리 */
            continue;                                /* 다음 연결로 */
        }
        /* 반복 서버라 클라이언트 하나가 멈추면 전체가 멈춘다.
         * recv/send 에 "전체" 마감 시간을 줘서 한 연결이 쓸 수 있는 시간을 묶는다. */
        if (handle_client(cfd, peer) == 0) /* 요청 받고 응답 보내기 */
            tcp_close_graceful(cfd); /* 응답을 보낸 경우만 RST 방지용 정상 종료 */
        else
            close(cfd); /* 이미 포기한 연결은 기다리지 않고 바로 닫음 */
    }

    close(lfd);                   /* 리스닝 소켓 닫기 → 더 이상 새 연결을 받지 않음 */
    if (g_spare_fd >= 0)          /* 예비 fd 가 열려 있으면 */
        close(g_spare_fd);        /* 반납 */
    printf("\nshutdown complete\n"); /* 종료 메시지 (exit 시 stdout 버퍼도 자동으로 비워짐) */
    return EXIT_SUCCESS;          /* 종료 코드 0 */
}
