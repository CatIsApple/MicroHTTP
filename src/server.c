/*
 * server.c — 웹 서버 본체 (main → 연결 수락 → 요청 처리)
 *
 * 전체 흐름
 *
 *   main()
 *    ├─ parse_arguments()     : ./server -p 포트 -r 루트 ...
 *    ├─ create_listener()     : socket → setsockopt → bind → listen
 *    └─ 반복
 *        ├─ accept()          : 연결 하나 수락 (3-way handshake가 끝난 연결)
 *        ├─ handle_connection(): recv → 파싱 → 요청 처리 → (keep-alive면 반복)
 *        └─ close_socket()
 *
 *   handle_request()           : 요청 하나를 정적 파일로 응답
 *    └─ serve_static_file()    : static_file_open() → 헤더 send → 파일 read/send 반복
 */
#define _POSIX_C_SOURCE 200809L

#include "http.h"
#include "net.h"
#include "static_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    DEFAULT_PORT = 8080,
    BACKLOG = 128,                    /* accept 전 대기 연결 큐 크기 (OS에 요청하는 값) */
    IDLE_TIMEOUT_SECONDS = 5,         /* 이 시간 동안 아무것도 오가지 않으면 연결을 정리 */
    MAX_REQUESTS_PER_CONNECTION = 100,/* keep-alive 연결 하나가 서버를 독점하지 않도록 제한 */
    FILE_CHUNK_SIZE = 16384           /* 파일을 이 크기만큼씩 읽어서 보냄 */
};

/* 실행 설정. main에서 한 번 채운 뒤에는 읽기만 합니다. */
static struct {
    const char *bind_address; /* 기본 127.0.0.1: 내 컴퓨터에서만 접속 가능 */
    uint16_t port;
    char *root;               /* realpath로 정규화한 정적 파일 루트 (malloc) */
    int verbose;              /* 1이면 [parse]/[recv] 상세 로그 */
} config = { "127.0.0.1", DEFAULT_PORT, NULL, 0 };

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
    int is_head = strcmp(request->method, "HEAD") == 0;
    /* 정적 파일은 읽기 전용이므로 GET/HEAD만 허용합니다. */
    if (strcmp(request->method, "GET") != 0 && !is_head) {
        return send_error_response(client_fd, 405, *keep_alive, 0, "GET, HEAD") == 0 ? 405 : -1;
    }
    return serve_static_file(client_fd, request, *keep_alive, is_head);
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
            int keep_alive = request.keep_alive && handled < MAX_REQUESTS_PER_CONNECTION;
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
            continue;
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
    }
    free(buffer); /* 모든 종료 경로가 이 한 곳을 지나갑니다 */
}

static void print_usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s [-p port] [-r root] [-b bind_address] [-v]\n"
        "  -p port          listen port (default 8080)\n"
        "  -r root          static file root directory (default ./www)\n"
        "  -b bind_address  IPv4 address to bind (default 127.0.0.1)\n"
        "  -v               verbose parse/recv logs\n", program);
}

/* 명령줄 인자를 config에 채웁니다. 성공 0, 실패 -1. */
static int parse_arguments(int argc, char **argv)
{
    const char *root = "./www";
    int option;
    while ((option = getopt(argc, argv, "p:r:b:v")) != -1) {
        if (option == 'p') {
            char *end;
            errno = 0;
            long port = strtol(optarg, &end, 10);
            if (errno || *optarg == '\0' || *end || port < 1 || port > 65535) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return -1;
            }
            config.port = (uint16_t)port;
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
    /*
     * 클라이언트가 먼저 끊은 소켓에 send하면 커널이 SIGPIPE를 보내고, 기본 동작은 프로세스 종료입니다.
     * 무시하도록 바꿔서 send가 EPIPE 오류로 돌아오게 하고, 그 연결만 정리합니다.
     */
    signal(SIGPIPE, SIG_IGN);

    int listener = create_listener(config.bind_address, config.port);
    if (listener == -1) {
        free(config.root);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "MicroHTTP listening on %s:%u root=%s\n",
            config.bind_address, config.port, config.root);

    for (;;) {
        /* 완료된 연결이 큐에 생길 때까지 여기서 블로킹합니다. */
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            break;
        }
        handle_connection(client_fd);
        close_socket(client_fd); /* 연결을 받은 쪽(main)이 닫습니다 */
    }
    close_socket(listener);
    free(config.root);
    return EXIT_FAILURE;
}
