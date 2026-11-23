/* main.c — 서버 프로그램의 진입점.
 * 명령줄 인자 처리, 시그널 설정, 연결을 하나씩 받아 처리하는 메인 루프,
 * 그리고 지금은 고정된 HTTP 응답(200/400/431)을 보내는 일을 맡는다. */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 함수(sigaction, nanosleep 등)를 헤더에서 보이게 함. include 보다 먼저 */

#include "tcp_server.h" /* tcp_listen, tcp_accept, tcp_send_all, tcp_recv_request, tcp_close_graceful */

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

/* ---- 고정 응답: 내용이 바뀌지 않으므로 컴파일 시점에 완성해 둔다 ----
 * (요청마다 snprintf 로 다시 만들 필요 없음. 길이도 sizeof 로 구함) */
#define OK_BODY "Hello from MicroHTTP TCP server\n" /* 200 응답 본문 */
static const char RESP_200[] =                     /* 200 응답 전체. 인접한 문자열 리터럴은 컴파일러가 하나로 이어 붙임 */
    "HTTP/1.1 200 OK\r\n"                          /* 상태 줄: 버전, 상태 코드, 이유 문구. 줄 끝은 HTTP 규칙대로 CRLF */
    "Content-Type: text/plain; charset=utf-8\r\n"  /* 본문 종류: UTF-8 일반 텍스트 */
    "Content-Length: 32\r\n" /* = sizeof(OK_BODY) - 1, 아래 _Static_assert 로 검증 */
    "Connection: close\r\n"                        /* 응답 후 연결을 닫겠다고 알림 (keep-alive 미지원) */
    "\r\n" OK_BODY;                                /* 빈 줄 = 헤더 끝, 그 뒤가 본문 */
_Static_assert(sizeof(OK_BODY) - 1 == 32, "Content-Length 와 OK_BODY 길이가 다름"); /* 컴파일 시점 검사: 본문을 바꾸고 숫자를 안 바꾸면 빌드 실패 */

/* 본문 없는 오류 응답 */
#define ERROR_RESP(status) \
    "HTTP/1.1 " status "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" /* 상태 코드만 바꿔 끼우는 응답 틀 */
static const char RESP_400[] = ERROR_RESP("400 Bad Request");                     /* 요청이 잘못됨(헤더가 끝나기 전에 끊김) */
static const char RESP_431[] = ERROR_RESP("431 Request Header Fields Too Large"); /* 헤더가 버퍼보다 큼 */

static int g_quiet = 0; /* -q: 요청마다 찍는 로그 끔 (부하 테스트용) */

static int send_resp(int cfd, const char *peer, const char *resp, size_t len) /* 응답 전송 + 실패 로그. 성공 0, 실패 -1 */
{
    if (tcp_send_all(cfd, resp, len, CLIENT_TIMEOUT_MS) < 0) {           /* 마감 시간 안에 응답 전체 전송 시도 */
        fprintf(stderr, "[%s] send error: %s\n", peer, strerror(errno)); /* 실패 이유 출력 (EPIPE, ETIMEDOUT 등) */
        return -1;                                                       /* 실패 */
    }
    return 0; /* 성공 */
}

/* 연결 하나 처리: 요청 수신 → 응답 송신
 * (HTTP 파싱/응답 생성은 다음 단계에서 구현. 지금은 고정 응답)
 * 반환: 응답까지 보냈으면 0, 타임아웃·오류·빈 연결이면 -1 */
static int handle_client(int cfd, const char *peer) /* cfd: 클라이언트 연결 소켓, peer: 로그용 "IP:포트" */
{
    char buf[RECV_BUF_SIZE];  /* 요청을 받을 버퍼 (스택에 8KB) */
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
            fprintf(stderr, "[%s] header too large (> %zu bytes) -> 431\n", peer, sizeof(buf) - 1); /* 로그 */
            return send_resp(cfd, peer, RESP_431, sizeof(RESP_431) - 1); /* 431 응답. sizeof - 1 = 끝의 '\0' 제외 길이 */
        }
        /* 헤더가 끝나기 전에 상대가 송신을 닫음(half-close) */
        fprintf(stderr, "[%s] incomplete request (%zd bytes) -> 400\n", peer, n); /* 로그 */
        return send_resp(cfd, peer, RESP_400, sizeof(RESP_400) - 1);             /* 400 응답 */
    }

    /* 첫 줄(요청 라인)만 로그 */
    if (!g_quiet) {                                  /* 조용 모드가 아니면 */
        size_t line_len = strcspn(buf, "\r\n");      /* 첫 \r 또는 \n 까지의 길이 = 요청 라인("GET / HTTP/1.1") 길이 */
        printf("[%s] %zu bytes header | %.*s\n", peer, header_len, (int)line_len, buf); /* %.*s: 문자열을 line_len 글자까지만 출력 */
    }

    return send_resp(cfd, peer, RESP_200, sizeof(RESP_200) - 1); /* 정상 요청 → 고정 200 응답 */
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
