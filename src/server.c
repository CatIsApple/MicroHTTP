/*
 * server.c — 웹 서버 본체 (main → 연결 수락 → 요청 처리)
 *
 * 전체 흐름
 *
 *   main()                       [main 스레드 = 생산자]
 *    ├─ parse_arguments()     : ./server -p 포트 -r 루트 -t 스레드수 ...
 *    ├─ install_signal_handlers(): SIGINT/SIGTERM → stop_requested = 1, SIGPIPE 무시
 *    ├─ create_listener()     : socket → setsockopt → bind → listen
 *    ├─ pthread_create × N    : worker_main 스레드 풀 시작
 *    ├─ 반복 (stop_requested가 될 때까지)
 *    │   ├─ accept()          : 연결 하나 수락 (3-way handshake가 끝난 연결)
 *    │   └─ queue_push(fd)    : 작업 큐에 넣음. 가득 찼으면 빈자리가 날 때까지 기다림
 *    └─ 종료: 리스너 닫기 → 큐 닫기 → pthread_join으로 작업 스레드 기다리기
 *
 *   worker_main()                [작업 스레드 = 소비자]
 *    └─ 반복: queue_pop() → handle_connection(fd) → close_socket(fd)
 *
 *   handle_connection()        : recv → 파싱 → 요청 처리 → (keep-alive면 반복)
 *
 *   handle_request()           : 요청 하나를 어디서 처리할지 결정
 *    ├─ proxy_find_route() 일치 → proxy_forward()   : 백엔드로 전달 (동적 요청, proxy.c)
 *    └─ 그 외              → serve_static_file()    : static_file_open() → 헤더 send → 파일 read/send
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI: Linux glibc에서 realpath() 선언에 필요 (Makefile과 같은 값) */
#endif

#include "http.h"
#include "net.h"
#include "proxy.h"
#include "static_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    DEFAULT_PORT = 8080,
    BACKLOG = 128,                    /* accept 전 대기 연결 큐 크기 (OS에 요청하는 값) */
    IDLE_TIMEOUT_SECONDS = 5,         /* 이 시간 동안 아무것도 오가지 않으면 연결을 정리 */
    REQUEST_TIMEOUT_SECONDS = 10,     /* 요청 하나를 다 받기까지의 총 한도 (slowloris 방지) */
    MAX_REQUESTS_PER_CONNECTION = 100,/* keep-alive 연결 하나가 서버를 독점하지 않도록 제한 */
    FILE_CHUNK_SIZE = 16384,          /* 파일을 이 크기만큼씩 읽어서 보냄 */
    DEFAULT_THREADS = 8,              /* 작업 스레드 수 = 동시에 처리할 수 있는 연결 수 */
    MAX_THREADS = 256,
    QUEUE_CAPACITY = 64               /* 작업 스레드를 기다리는 연결의 최대 수 */
};

/* 실행 설정. main에서 한 번 채운 뒤에는 읽기만 하므로 스레드끼리 잠금 없이 공유합니다. */
static struct {
    const char *bind_address; /* 기본 127.0.0.1: 내 컴퓨터에서만 접속 가능 */
    uint16_t port;
    char *root;               /* realpath로 정규화한 정적 파일 루트 (malloc) */
    int threads;
    int verbose;              /* 1이면 [parse]/[recv] 상세 로그 */
} config = { "127.0.0.1", DEFAULT_PORT, NULL, DEFAULT_THREADS, 0 };

/* Reverse Proxy 라우팅 규칙 (-R 옵션). 규칙 목록은 시작 후 바뀌지 않고, Round-Robin 순번만 lock으로 보호합니다. */
static ProxyTable proxy_table = { .lock = PTHREAD_MUTEX_INITIALIZER };

/*
 * SIGINT(Ctrl+C)/SIGTERM(kill)이 오면 1이 됩니다.
 * 시그널 핸들러 안에서는 안전하게 할 수 있는 일이 매우 적기 때문에(printf, malloc 금지),
 * 핸들러는 이 플래그만 세우고 실제 정리는 main 루프가 플래그를 보고 합니다.
 *
 * 왜 volatile sig_atomic_t가 아니라 atomic_int인가:
 *   sig_atomic_t는 "시그널 핸들러 ↔ 같은 스레드" 사이에서만 안전합니다. 이 플래그는 작업 스레드도
 *   읽기 때문에(handle_connection의 keep-alive 판단) C11 메모리 모델상 데이터 레이스였고,
 *   ThreadSanitizer가 보고했습니다(EXPERIMENT_REPORT STEP 0). atomic_int는 스레드 사이에서 안전하고,
 *   lock-free라면 시그널 핸들러 안에서 써도 안전합니다(C11 7.14.1.1) — 아래 검사로 보장합니다.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "stop_requested must be lock-free to be used in a signal handler");
static atomic_int stop_requested = 0;

/*
 * 작업 큐: main 스레드가 accept한 연결(fd)을 넣고, 작업 스레드가 꺼내 갑니다.
 * 여러 스레드가 동시에 만지므로 mutex로 보호하고, 두 개의 condition variable로 기다립니다.
 *   not_empty : 큐가 비면 작업 스레드(소비자)가 여기서 잠듦
 *   not_full  : 큐가 가득 차면 main 스레드(생산자)가 여기서 잠듦
 *               → 그동안 새 연결은 커널의 listen 대기열(BACKLOG)에 쌓여 기다립니다.
 */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int fds[QUEUE_CAPACITY];  /* 원형 배열: head부터 count개가 대기 중 */
    size_t head;
    size_t count;
    int closing;              /* 1이면 더 이상 새 연결을 받지 않음 (셧다운 중) */
} queue = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, 0, 0, 0 };

/*
 * 리스닝 소켓을 만듭니다: socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
 * 성공하면 fd, 실패하면 -1.
 */
static int create_listener(const char *address_text, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0); /* IPv4 + TCP */
    if (fd == -1) { perror("socket"); return -1; }

    /*
     * 서버를 껐다 바로 켜면 이전 연결이 TIME_WAIT 상태로 남아 bind가
     * "Address already in use"로 실패합니다. SO_REUSEADDR이 이를 허용합니다.
     */
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        goto fail;
    }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, address_text, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address: %s\n", address_text);
        goto fail;
    }
    /* 포트를 다른 프로그램이 쓰고 있거나(EADDRINUSE) 1024 미만 포트에 권한이 없으면(EACCES) 실패 */
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        goto fail;
    }
    /*
     * 이제 커널이 이 포트로 오는 연결의 3-way handshake를 대신 처리하고,
     * 완료된 연결을 BACKLOG 크기의 큐에 쌓아 둡니다. accept()는 그 큐에서 하나를 꺼냅니다.
     */
    if (listen(fd, BACKLOG) == -1) {
        perror("listen");
        goto fail;
    }
    return fd;
fail:
    close_socket(fd);
    return -1;
}

/*
 * 정적 파일 하나를 응답합니다.
 * 반환값: 보낸 상태 코드. 송신에 실패해 연결을 더 쓸 수 없으면 -1.
 */
static int serve_static_file(int client_fd, const HttpRequest *request,
                             int keep_alive, int omit_body)
{
    int file_fd = -1;
    size_t file_size = 0;
    const char *mime_type = NULL;
    int status = static_file_open(config.root, request->target, &file_fd, &file_size, &mime_type);
    if (status != 200)
        return send_error_response(client_fd, status, keep_alive, omit_body, NULL) == 0 ? status : -1;

    char header[512];
    int header_length = http_response_header(header, sizeof(header), 200, mime_type,
                                             file_size, keep_alive, NULL);
    int ok = header_length > 0 && send_all(client_fd, header, (size_t)header_length) == 0;

    /*
     * 파일 전체를 malloc으로 메모리에 올리지 않고 조각씩 읽어 보냅니다.
     * 큰 파일도 FILE_CHUNK_SIZE 만큼의 메모리만 쓰고, free할 것도 없습니다.
     */
    char chunk[FILE_CHUNK_SIZE];
    size_t remaining = omit_body ? 0 : file_size;
    while (ok && remaining > 0) {
        ssize_t count = read(file_fd, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk));
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) {
            /*
             * 헤더(200, Content-Length)는 이미 보냈으므로 이제 와서 500으로 바꿀 수 없습니다.
             * 연결을 끊어서 클라이언트가 "본문이 덜 왔다"는 것을 알게 합니다.
             */
            ok = 0;
            break;
        }
        ok = send_all(client_fd, chunk, (size_t)count) == 0;
        remaining -= (size_t)count;
    }
    close(file_fd); /* 정상/오류 경로 모두에서 파일을 닫습니다 */
    return ok ? 200 : -1;
}

/*
 * 완성된 요청 하나를 처리합니다.
 *   *keep_alive : 들어올 때는 "유지해도 되는지", 나갈 때는 "실제로 유지할지"
 * 반환값: 보낸 상태 코드, 송신 실패면 -1.
 */
static int handle_request(int client_fd, const HttpRequest *request, int *keep_alive)
{
    /*
     * 라우팅 규칙과 경로가 일치하면 백엔드로 전달합니다(동적 요청 처리).
     * 백엔드 응답은 "연결이 닫힐 때까지"로 끝을 판단해 그대로 중계하므로,
     * 응답 뒤 클라이언트 연결도 닫습니다(keep-alive 하지 않음).
     */
    ProxyRoute *route = proxy_find_route(&proxy_table, request->target);
    if (route) {
        *keep_alive = 0;
        return proxy_forward(client_fd, request, &proxy_table, route);
    }

    int is_head = strcmp(request->method, "HEAD") == 0;
    /* 정적 파일은 읽기 전용이므로 GET/HEAD만 허용합니다. */
    if (strcmp(request->method, "GET") != 0 && !is_head) {
        return send_error_response(client_fd, 405, *keep_alive, 0, "GET, HEAD") == 0 ? 405 : -1;
    }
    return serve_static_file(client_fd, request, *keep_alive, is_head);
}

/* 경과 시간 측정용 시계. 시스템 시각(time())과 달리 사용자가 시계를 바꿔도 되돌아가지 않습니다. */
static time_t monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

/*
 * 연결 하나를 처리합니다.
 *
 * TCP는 "바이트의 흐름"이라 recv 한 번이 요청 하나라는 보장이 없습니다.
 * 그래서 받은 바이트를 버퍼에 쌓으면서 매번 파서에게 "요청이 완성됐는지" 묻습니다.
 *   NEED_MORE → recv로 더 받기
 *   COMPLETE  → 응답하고, keep-alive면 버퍼에 남은 다음 요청부터 다시 파싱
 *   ERROR     → 오류 응답 후 연결 종료
 */
static void handle_connection(int client_fd)
{
    if (set_socket_timeout(client_fd, IDLE_TIMEOUT_SECONDS) == -1) return;

    /* 요청 하나의 최대 크기(헤더 상한 + 본문 상한)만큼만 확보합니다. */
    const size_t capacity = HTTP_MAX_HEADER + HTTP_MAX_BODY;
    unsigned char *buffer = malloc(capacity);
    if (!buffer) {
        send_error_response(client_fd, 500, 0, 0, NULL);
        return;
    }
    size_t used = 0;        /* 버퍼에 쌓인 바이트 수 */
    int handled = 0;        /* 이 연결에서 처리한 요청 수 */
    /*
     * 현재 요청의 첫 바이트가 도착한 시각 (0 = 아직 안 옴).
     * IDLE_TIMEOUT은 recv 한 번마다 다시 시작되므로, 1바이트씩 4초 간격으로 보내는 클라이언트(slowloris)는
     * 그것만으로는 끊을 수 없습니다. 그래서 요청 하나 전체에도 시간 한도를 둡니다.
     */
    time_t request_started = 0;

    for (;;) {
        HttpRequest request;
        size_t consumed;
        int status;
        HttpParseResult result = http_parse_request(buffer, used, &request, &consumed, &status);
        if (config.verbose)
            fprintf(stderr, "[parse] fd=%d used=%zu result=%s\n", client_fd, used,
                    result == HTTP_COMPLETE ? "COMPLETE" : result == HTTP_ERROR ? "ERROR" : "NEED_MORE");

        if (result == HTTP_ERROR) {
            /* 요청의 경계를 믿을 수 없으므로 연결을 유지하지 않습니다. */
            fprintf(stderr, "[request] fd=%d invalid request status=%d\n", client_fd, status);
            send_error_response(client_fd, status, 0, 0, NULL);
            break;
        }
        if (result == HTTP_COMPLETE) {
            ++handled;
            /* 셧다운 중이면 이 응답을 마지막으로 연결을 닫습니다 (처리 중인 요청은 끝까지 응답). */
            int keep_alive = request.keep_alive && handled < MAX_REQUESTS_PER_CONNECTION &&
                             !atomic_load(&stop_requested);
            int sent_status = handle_request(client_fd, &request, &keep_alive);
            fprintf(stderr, "[request] fd=%d \"%s %s %s\" status=%d keep_alive=%d\n",
                    client_fd, request.method, request.target, request.version,
                    sent_status, keep_alive && sent_status > 0);
            if (sent_status < 0 || !keep_alive) break;
            /*
             * keep-alive: 처리한 요청(consumed 바이트)을 버퍼에서 지우고,
             * 이미 함께 도착한 다음 요청(파이프라이닝)이 있으면 앞으로 당겨 이어서 파싱합니다.
             */
            memmove(buffer, buffer + consumed, used - consumed);
            used -= consumed;
            request_started = used > 0 ? monotonic_seconds() : 0; /* 다음 요청의 시계를 새로 시작 */
            continue;
        }

        /* NEED_MORE인데 요청을 받기 시작한 지 너무 오래됐다면 더 기다리지 않습니다. */
        if (request_started && monotonic_seconds() - request_started >= REQUEST_TIMEOUT_SECONDS) {
            fprintf(stderr, "[request] fd=%d request not complete within %d s\n", client_fd, REQUEST_TIMEOUT_SECONDS);
            send_error_response(client_fd, 408, 0, 0, NULL);
            break;
        }
        /* NEED_MORE: 버퍼 빈 곳에 이어서 받습니다. 데이터가 오거나 타임아웃이 될 때까지 블로킹. */
        ssize_t received = recv(client_fd, buffer + used, capacity - used, 0);
        if (received == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /*
                 * 요청을 보내다 멈췄다면 408을 알려줍니다.
                 * keep-alive로 다음 요청을 기다리다 시간이 지난 것이면 조용히 닫습니다.
                 */
                if (used > 0 || handled == 0) send_error_response(client_fd, 408, 0, 0, NULL);
            } else {
                perror("recv"); /* ECONNRESET: 클라이언트가 갑자기 끊음 */
            }
            break;
        }
        if (received == 0) {
            /* 0 = 상대가 연결을 닫음(EOF). 요청이 반쯤 왔다면 불완전한 요청입니다. */
            if (used > 0) send_error_response(client_fd, 400, 0, 0, NULL);
            break;
        }
        used += (size_t)received;
        if (config.verbose)
            fprintf(stderr, "[recv] fd=%d bytes=%zd accumulated=%zu\n", client_fd, received, used);
        if (request_started == 0) request_started = monotonic_seconds();
    }
    free(buffer); /* 모든 종료 경로가 이 한 곳을 지나갑니다 */
}

/*
 * 연결 fd를 작업 큐에 넣고 잠든 작업 스레드 하나를 깨웁니다.
 * 큐가 가득 찼으면 작업 스레드가 하나를 꺼내 갈 때까지 기다립니다(배압, backpressure).
 *
 * 가득 찼을 때 바로 503을 보내고 닫는 방법도 있지만, 아직 읽지 않은 요청 바이트가 남은 소켓을
 * close하면 커널이 RST를 보내 클라이언트가 503 응답조차 받지 못할 수 있습니다.
 * 기다리는 쪽이 더 단순하고, 대기 중인 연결은 커널 listen 대기열이 맡아 줍니다.
 */
static void queue_push(int client_fd)
{
    pthread_mutex_lock(&queue.lock);
    while (queue.count == QUEUE_CAPACITY)
        pthread_cond_wait(&queue.not_full, &queue.lock);
    queue.fds[(queue.head + queue.count) % QUEUE_CAPACITY] = client_fd;
    ++queue.count;
    pthread_cond_signal(&queue.not_empty);
    pthread_mutex_unlock(&queue.lock);
}

/*
 * 큐에서 연결 하나를 꺼냅니다. 비어 있으면 생길 때까지 잠들어 기다립니다.
 * 셧다운 중이고 큐도 비었으면 -1 (작업 스레드 종료 신호).
 * → 셧다운 때도 이미 큐에 들어온 연결은 끝까지 처리됩니다.
 */
static int queue_pop(void)
{
    pthread_mutex_lock(&queue.lock);
    /* while인 이유: 깨어났는데 다른 스레드가 먼저 가져갔거나, 이유 없이 깨어날 수(spurious wakeup) 있음 */
    while (queue.count == 0 && !queue.closing)
        pthread_cond_wait(&queue.not_empty, &queue.lock); /* lock을 풀고 잠들었다가, 깨면 다시 잡음 */
    int client_fd = -1;
    if (queue.count > 0) {
        client_fd = queue.fds[queue.head];
        queue.head = (queue.head + 1) % QUEUE_CAPACITY;
        --queue.count;
        pthread_cond_signal(&queue.not_full); /* 기다리던 main 스레드를 깨움 */
    }
    pthread_mutex_unlock(&queue.lock);
    return client_fd;
}

/* 작업 스레드: 큐에서 연결을 꺼내 처리하고 닫기를 반복합니다. */
static void *worker_main(void *unused)
{
    (void)unused;
    for (;;) {
        int client_fd = queue_pop();
        if (client_fd == -1) break;
        handle_connection(client_fd);
        close_socket(client_fd); /* 정상/오류/타임아웃 모든 경우에 여기서 한 번만 닫습니다 */
    }
    return NULL;
}

/* 시그널 핸들러: 플래그만 세웁니다 (async-signal-safe). */
static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    atomic_store(&stop_requested, 1);
}

/* SIGINT/SIGTERM → 안전한 셧다운, SIGPIPE → 무시. 성공 0, 실패 -1. */
static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    /* SA_RESTART를 주지 않아야 블로킹 중인 poll/accept가 EINTR로 깨어나 플래그를 확인합니다. */
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) == -1 || sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    /*
     * 클라이언트가 먼저 끊은 소켓에 send하면 커널이 SIGPIPE를 보내고, 기본 동작은 프로세스 종료입니다.
     * 무시하도록 바꿔서 send가 EPIPE 오류로 돌아오게 하고, 그 연결만 정리합니다.
     */
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("sigaction SIGPIPE");
        return -1;
    }
    return 0;
}

/*
 * 작업 스레드를 count개 만듭니다. 만든 개수를 돌려줍니다.
 * 작업 스레드에서는 SIGINT/SIGTERM을 막아 두어 시그널이 항상 main 스레드로 가게 합니다.
 * (그래야 main의 poll이 EINTR로 깨어나 바로 셧다운을 시작합니다.)
 */
static int start_workers(pthread_t *threads, int count)
{
    sigset_t stop_signals, previous;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, &previous); /* 새 스레드는 이 마스크를 물려받음 */
    int started = 0;
    for (; started < count; ++started) {
        int error = pthread_create(&threads[started], NULL, worker_main, NULL);
        if (error) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            break;
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);        /* main 스레드는 다시 받도록 복구 */
    return started;
}

/* 큐를 닫고 잠든 작업 스레드를 모두 깨운 뒤, 남은 작업을 끝내고 종료할 때까지 기다립니다. */
static void stop_workers(pthread_t *threads, int count)
{
    pthread_mutex_lock(&queue.lock);
    queue.closing = 1;
    pthread_cond_broadcast(&queue.not_empty);
    pthread_mutex_unlock(&queue.lock);
    for (int i = 0; i < count; ++i) pthread_join(threads[i], NULL);
}

static void print_usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [-p port] [-r root] [-t threads] [-R prefix=host:port[,host:port...]]... [-b bind_address] [-v]\n"
        "  -p port          listen port (default 8080)\n"
        "  -r root          static file root directory (default ./www)\n"
        "  -t threads       worker threads (default 8)\n"
        "  -R rule          reverse proxy rule, repeatable (e.g. -R /api/=127.0.0.1:9001,127.0.0.1:9002)\n"
        "  -b bind_address  IPv4 address to bind (default 127.0.0.1)\n"
        "  -v               verbose parse/recv logs\n", program);
}

/* 1..max 범위의 정수 인자를 읽습니다. 실패하면 -1. */
static long parse_number(const char *text, long max)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || *text == '\0' || *end || value < 1 || value > max) return -1;
    return value;
}

/* 명령줄 인자를 config에 채웁니다. 성공 0, 실패 -1. */
static int parse_arguments(int argc, char **argv)
{
    const char *root = "./www";
    int option;
    while ((option = getopt(argc, argv, "p:r:t:R:b:v")) != -1) {
        if (option == 'p') {
            long port = parse_number(optarg, 65535);
            if (port == -1) { fprintf(stderr, "invalid port: %s\n", optarg); return -1; }
            config.port = (uint16_t)port;
        } else if (option == 't') {
            long threads = parse_number(optarg, MAX_THREADS);
            if (threads == -1) { fprintf(stderr, "invalid thread count: %s\n", optarg); return -1; }
            config.threads = (int)threads;
        } else if (option == 'R') {
            if (proxy_add_route(&proxy_table, optarg) == -1) {
                fprintf(stderr, "invalid proxy rule: %s\n", optarg);
                return -1;
            }
        } else if (option == 'r') {
            root = optarg;
        } else if (option == 'b') {
            config.bind_address = optarg;
        } else if (option == 'v') {
            config.verbose = 1;
        } else {
            return -1;
        }
    }
    if (optind != argc) return -1;

    /* 루트를 절대 경로로 정규화해 두어야 static_file_open이 "루트 안인지" 비교할 수 있습니다. */
    config.root = realpath(root, NULL);
    struct stat info;
    if (!config.root || stat(config.root, &info) == -1 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "invalid root directory: %s\n", root);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (parse_arguments(argc, argv) == -1) {
        print_usage(argv[0]);
        free(config.root);
        return EXIT_FAILURE;
    }
    if (install_signal_handlers() == -1) {
        free(config.root);
        return EXIT_FAILURE;
    }
    int listener = create_listener(config.bind_address, config.port);
    if (listener == -1) {
        free(config.root);
        return EXIT_FAILURE;
    }
    pthread_t threads[MAX_THREADS];
    int started = start_workers(threads, config.threads);
    if (started == 0) {
        close_socket(listener);
        free(config.root);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "MicroHTTP listening on %s:%u root=%s threads=%d\n",
            config.bind_address, config.port, config.root, started);
    for (size_t i = 0; i < proxy_table.route_count; ++i) {
        const ProxyRoute *route = &proxy_table.routes[i];
        fprintf(stderr, "  proxy %s ->", route->prefix);
        for (size_t j = 0; j < route->backend_count; ++j)
            fprintf(stderr, " %s:%s", route->backends[j].host, route->backends[j].port);
        fputc('\n', stderr);
    }

    while (!atomic_load(&stop_requested)) {
        /*
         * accept()에 바로 들어가면, 플래그 검사 직후~accept 진입 직전에 시그널이 오는 경우
         * 다음 연결이 올 때까지 셧다운이 미뤄집니다. poll에 1초 제한을 두어
         * "새 연결이 있는지"를 확인하고, 최소 1초마다 플래그를 다시 봅니다.
         */
        struct pollfd listener_poll = { .fd = listener, .events = POLLIN };
        int ready = poll(&listener_poll, 1, 1000);
        if (ready == -1) {
            if (errno == EINTR) continue; /* 시그널 → 루프 조건에서 stop_requested 확인 */
            perror("poll");
            break;
        }
        if (ready == 0) continue;         /* 1초 동안 새 연결 없음 */

        /* 커널 큐에 완료된 연결이 있으므로 accept는 기다리지 않고 바로 돌아옵니다. */
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
            perror("accept");             /* EMFILE(fd 고갈) 등: 잠깐 쉬고 다시 시도 */
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000000 };
            nanosleep(&pause, NULL);
            continue;
        }
        queue_push(client_fd);
    }

    fprintf(stderr, "shutting down: closing listener and waiting for %d workers\n", started);
    close_socket(listener);           /* 새 연결 받기 중단 */
    stop_workers(threads, started);   /* 처리 중·대기 중인 연결을 마무리 */
    free(config.root);
    fprintf(stderr, "shutdown complete\n");
    return EXIT_SUCCESS;
}
