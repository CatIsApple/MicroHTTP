/* main.c — 서버 프로그램의 진입점.
 * 명령줄 인자 처리, 시그널 설정, 연결을 받아 워커 풀에 넘기는 accept 루프, 종료 정리만 맡는다.
 * 요청 처리(파싱·본문·정적 파일·echo·keep-alive 반복)는 server.c 가 워커 스레드에서 한다.
 *
 *   메인 스레드: accept ──tp_submit──▶ 스레드 풀(워커 N개: serve_connection)
 *                                        ↕ 쉬는 keep-alive 연결
 *                                     idle poller (스레드 1개)
 *
 * 워커·poller 는 SIGINT/SIGTERM 을 막은 채 만들어지므로 종료 시그널은 메인 스레드로만 온다
 * → 블로킹 accept 가 EINTR 로 깨어나 루프를 빠져나간다. */

#define _POSIX_C_SOURCE 200809L /* POSIX 2008 함수(sigaction, nanosleep 등)를 헤더에서 보이게 함. include 보다 먼저 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* macOS: _POSIX_C_SOURCE 를 켜면 숨겨지는 MSG_DONTWAIT 를 다시 보이게 */
#endif

#include "conn_job.h"    /* conn_job (워커에 넘기는 연결 단위) */
#include "idle_poller.h" /* idle_create, idle_stop */
#include "server.h"      /* server_ctx, serve_connection, server_on_idle_ready */
#include "static_file.h" /* static_open_root */
#include "tcp_server.h"  /* tcp_listen, tcp_accept */
#include "thread_pool.h" /* tp_create, tp_submit, tp_shutdown, tp_destroy */

#include <errno.h>      /* errno, EINTR, EMFILE, ENFILE */
#include <fcntl.h>      /* open, O_RDONLY (예비 fd 용) */
#include <signal.h>     /* sigaction, SIGINT/SIGTERM/SIGPIPE, sig_atomic_t, SIG_IGN */
#include <stdint.h>     /* uint16_t */
#include <stdio.h>      /* printf, fprintf, perror, fflush */
#include <stdlib.h>     /* strtol, EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* memset, strcmp, strerror (메인 스레드에서만) */
#include <sys/socket.h> /* accept, send, MSG_DONTWAIT, MSG_NOSIGNAL */
#include <time.h>       /* nanosleep, struct timespec */
#include <unistd.h>     /* close */

#define DEFAULT_PORT 8080      /* 포트 인자를 안 주면 쓰는 기본 포트 */
#define DEFAULT_ROOT "./www"   /* -r 을 안 주면 쓰는 문서 루트 (정적 파일 디렉터리) */
#define BACKLOG 128            /* listen() 대기 큐 크기: handshake 끝나고 accept 를 기다리는 연결 최대 수 */
#define DEFAULT_THREADS 16     /* -t 를 안 주면 쓰는 워커 수 */
#define MAX_THREADS 1024       /* -t 상한 */
#define DEFAULT_IDLE_MS 5000   /* -i 를 안 주면 쓰는 keep-alive 쉬는 시간 (이만큼 아무것도 안 오면 닫음) */
#define QUEUE_CAP 1024         /* 워커 풀 작업 큐 크기 (넘치면 503) */
#define IDLE_MAX_CONNS 10000   /* idle poller 가 동시에 맡을 최대 연결 수 (넘치면 그 연결은 닫음) */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS 등 없는 곳: SIGPIPE 는 이미 무시하므로 0 이어도 안전 */
#endif

/* 큐가 가득 찼을 때 보내는 고정 응답 (만들 필요 없이 그대로 한 번 send) */
static const char RESP_503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

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

/* 문자열 → [lo, hi] 범위의 정수 (-t, -i 인자용). 성공 0, 잘못된 값 -1 */
static int parse_range(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;                  /* 숫자 다음 위치 */
    errno = 0;                         /* 범위 초과 감지용 */
    long v = strtol(s, &end, 10);      /* 10진수로 변환 */
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) /* 숫자 아님 / 뒤에 글자 / 범위 밖 */
        return -1;
    *out = v;
    return 0;
}

/* 큐가 가득 차서 워커에게 못 넘긴 연결: 고정 503 을 한 번만 시도하고 닫는다.
 * MSG_DONTWAIT: 소켓 송신 버퍼가 차 있어도 기다리지 않음 (accept 스레드는 절대 막히면 안 됨) */
static void reject_busy(int cfd)
{
    ssize_t r = send(cfd, RESP_503, sizeof(RESP_503) - 1, MSG_DONTWAIT | MSG_NOSIGNAL); /* 실패해도 상관없음 */
    (void)r;                                                                             /* 결과는 안 씀 */
    close(cfd);
}

int main(int argc, char *argv[]) /* argc: 인자 개수, argv: 인자 문자열 배열 (argv[0] = 프로그램 이름) */
{
    uint16_t port = DEFAULT_PORT;              /* 기본 포트로 시작 */
    const char *root = DEFAULT_ROOT;           /* 기본 문서 루트 */
    long threads = DEFAULT_THREADS;            /* 워커 수 */
    long idle_ms = DEFAULT_IDLE_MS;            /* keep-alive 쉬는 시간 */
    int quiet = 0;                             /* -q */
    for (int i = 1; i < argc; i++) {           /* argv[1] 부터 인자를 하나씩 확인 */
        int ok = 1;                            /* 이 인자를 해석했는지 */
        if (strcmp(argv[i], "-q") == 0)                               /* "-q": 조용 모드 */
            quiet = 1;
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)          /* "-r <디렉터리>" */
            root = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)          /* "-t <워커 수>" 1~1024 */
            ok = parse_range(argv[++i], 1, MAX_THREADS, &threads) == 0;
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)          /* "-i <쉬는 시간 ms>" 1~1시간 */
            ok = parse_range(argv[++i], 1, 3600L * 1000, &idle_ms) == 0;
        else                                                          /* 아니면 포트 */
            ok = parse_port(argv[i], &port) == 0;
        if (!ok) {
            fprintf(stderr, "usage: %s [-q] [-r docroot] [-t threads(1-%d, default %d)] "
                            "[-i keepalive_idle_ms(default %d)] [port]\n",
                    argv[0], MAX_THREADS, DEFAULT_THREADS, DEFAULT_IDLE_MS);
            return EXIT_FAILURE;                                      /* 종료 코드 1 */
        }
    }

    if (install_signal_handlers() < 0) { /* Ctrl+C·kill 처리, SIGPIPE 무시 등록 (스레드를 만들기 전에) */
        perror("sigaction");
        return EXIT_FAILURE;
    }

    int root_fd = static_open_root(root);      /* 문서 루트를 한 번 열어 둠 (요청마다 경로를 다시 해석하지 않게) */
    if (root_fd < 0) {
        fprintf(stderr, "docroot %s: %s\n", root, strerror(errno)); /* 없는 디렉터리 등 */
        return EXIT_FAILURE;
    }

    int lfd = tcp_listen(port, BACKLOG); /* socket → bind → listen 으로 리스닝 소켓 준비 */
    if (lfd < 0) {                       /* 실패(포트 사용 중 등). 에러 메시지는 tcp_listen 이 이미 출력 */
        close(root_fd);
        return EXIT_FAILURE;
    }

    g_spare_fd = open("/dev/null", O_RDONLY); /* fd 고갈 대비용 예비 fd 를 미리 하나 잡아 둠 */

    /* 워커·poller 가 함께 읽는 설정. pool/idle 은 아래에서 채운다 (스레드는 작업이 와야 ctx 를 읽음) */
    server_ctx ctx = {
        .root_fd = root_fd,
        .quiet = quiet,
        .keepalive_max = SERVER_KEEPALIVE_MAX,
        .pool = NULL,
        .idle = NULL,
    };
    ctx.pool = tp_create((int)threads, QUEUE_CAP, 0, serve_connection, &ctx); /* 워커 N개 (스택 기본 256KB) */
    if (ctx.pool == NULL) {
        perror("tp_create");
        close(lfd);
        close(root_fd);
        return EXIT_FAILURE;
    }
    ctx.idle = idle_create((int)idle_ms, IDLE_MAX_CONNS, server_on_idle_ready, &ctx); /* 쉬는 연결 감시 스레드 */
    if (ctx.idle == NULL) {
        perror("idle_create");
        tp_shutdown(ctx.pool);         /* 아직 작업이 없으니 바로 끝남 */
        tp_destroy(ctx.pool);
        close(lfd);
        close(root_fd);
        return EXIT_FAILURE;
    }

    /* stdout 버퍼링은 기본값 사용: 터미널이면 줄 단위, 파일·파이프면 블록 단위로
     * 모아서 쓴다(요청마다 write 시스템콜 X). 시작 메시지만 즉시 내보낸다. */
    printf("listening on 0.0.0.0:%u, docroot %s, %ld threads, keep-alive idle %ld ms (Ctrl+C to stop)%s\n",
           (unsigned)port, root, threads, idle_ms, quiet ? " [quiet]" : "");
    fflush(stdout);

    while (g_running) {                              /* 종료 시그널이 올 때까지 반복 */
        conn_job job = {.fd = -1, .nreq = 0};        /* 새 연결 하나 (요청 수 0 부터) */
        job.fd = tcp_accept(lfd, job.peer, sizeof(job.peer)); /* 연결 하나 받기. 대기 연결이 없으면 여기서 잠듦 */
        if (job.fd < 0) {                            /* accept 실패 */
            if (errno == EINTR)                      /* 시그널 때문에 깨어남 (예: Ctrl+C) */
                continue;                            /* 루프 조건에서 g_running 확인 */
            handle_accept_error(lfd);                /* fd 고갈 등 나머지 오류 처리 */
            continue;
        }
        if (tp_submit(ctx.pool, &job) != 0) {        /* 큐가 가득 참 (fd 는 아직 우리 것) */
            if (!quiet)
                fprintf(stderr, "[%s] queue full -> 503\n", job.peer);
            reject_busy(job.fd);                     /* 503 한 번 시도 후 닫음 (기다리지 않음) */
        }
    }

    /* 종료 순서: 새 연결 막기 → 워커 정지(처리 중인 요청은 끝까지, 큐에 남은 연결은 닫힘)
     * → poller 정지(쉬던 연결 닫힘, 그 사이 poller 의 tp_submit 은 ECANCELED 로 거절) → 풀 메모리 해제 */
    close(lfd);                   /* 리스닝 소켓 닫기 → 더 이상 새 연결을 받지 않음 */
    tp_shutdown(ctx.pool);        /* 워커 join + 큐에 남은 fd close */
    idle_stop(ctx.idle);          /* 감시 스레드 join + 쉬던 fd close + free */
    tp_destroy(ctx.pool);         /* 이제 아무도 pool 을 안 씀 */
    if (g_spare_fd >= 0)          /* 예비 fd 가 열려 있으면 */
        close(g_spare_fd);        /* 반납 */
    close(root_fd);               /* 문서 루트 */
    printf("\nshutdown complete\n"); /* 종료 메시지 (exit 시 stdout 버퍼도 자동으로 비워짐) */
    return EXIT_SUCCESS;          /* 종료 코드 0 */
}
