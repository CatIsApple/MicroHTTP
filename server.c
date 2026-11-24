/*
 * server.c — TCP 소켓을 열고 HTTP 요청을 받아 응답하는 서버 본체
 *
 * 전체 흐름:
 *
 *   main()
 *    ├─ create_listener()     : 127.0.0.1:포트 에서 연결을 기다릴 소켓 준비
 *    └─ 무한 반복
 *        ├─ accept()          : 클라이언트 연결 하나를 받음
 *        ├─ handle_client()   : 요청 받기 → 해석(http.c) → 라우팅(football.c) → 응답 보내기
 *        └─ close_socket()    : 연결 닫기
 *
 * 한 번에 한 연결만 처리하는 단일 스레드·블로킹 서버입니다.
 * 구조를 이해하기 쉽게 하려는 학습용 설계이며, 동시 접속 성능을 위한 것은 아닙니다.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime 등 POSIX 함수를 쓰기 위한 선언 */

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "football.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    DEFAULT_PORT = 8080, /* 포트를 지정하지 않았을 때 쓸 포트 */
    BACKLOG = 128,       /* 아직 accept하지 않은 연결을 OS가 쌓아둘 대기열 크기(요청값) */
    IDLE_SECONDS = 5     /* 클라이언트가 이 시간 동안 아무것도 보내거나 받지 않으면 포기 */
};

/* 소켓(파일 디스크립터, fd)을 닫고, 실패하면 로그만 남깁니다. */
static void close_socket(int fd)
{
    /*
     * close()가 실패해도 다시 시도하지 않습니다.
     * 실패한 시점에 이미 fd 번호가 반납되어 다른 곳에서 재사용 중일 수 있어서,
     * 재시도하면 엉뚱한 파일/소켓을 닫을 위험이 있기 때문입니다.
     */
    if (close(fd) == -1) perror("close");
}

/*
 * 연결을 기다릴 "리스닝 소켓"을 만듭니다.
 * 순서: socket() 생성 → 옵션 설정 → bind()로 주소/포트 연결 → listen()으로 대기 시작
 * 성공하면 소켓 fd, 실패하면 -1을 돌려줍니다.
 */
static int create_listener(uint16_t port)
{
    /* 운영체제에게 TCP(SOCK_STREAM) + IPv4(AF_INET) 소켓을 만들어 달라고 요청합니다. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    /*
     * 서버를 껐다가 바로 다시 켜면, 이전 연결이 TIME_WAIT 상태로 남아 있어
     * "Address already in use"로 bind가 실패하곤 합니다. SO_REUSEADDR로 이를 줄입니다.
     */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        goto fail;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port) /* 포트 번호를 네트워크 바이트 순서(빅엔디언)로 변환 */
    };
    /*
     * 127.0.0.1(루프백 = 내 컴퓨터 자신)에만 바인딩합니다.
     * 같은 컴퓨터에서만 접속할 수 있어 외부에 노출되지 않습니다.
     */
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fprintf(stderr, "inet_pton: invalid address\n");
        goto fail;
    }
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        goto fail;
    }
    /*
     * 이제부터 연결 요청을 받습니다. BACKLOG(128)는 "대기열 크기 요청값"일 뿐,
     * 128개 연결을 동시에 처리한다는 뜻은 아닙니다. 처리 자체는 하나씩 합니다.
     */
    if (listen(fd, BACKLOG) == -1) {
        perror("listen");
        goto fail;
    }
    return fd;

fail:
    /* 중간에 실패하면 만들어 둔 소켓을 닫고 -1을 돌려줍니다. (goto로 정리 코드를 한곳에 모음) */
    close_socket(fd);
    return -1;
}

/*
 * data의 length 바이트를 "전부" 보낼 때까지 send()를 반복합니다.
 * send()는 한 번에 일부만 보낼 수도 있기 때문에 이런 반복이 필요합니다.
 * 성공하면 0, 실패하면 -1 (errno에 원인이 남음).
 */
static int send_all(int fd, const unsigned char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);
        if (count == -1) {
            /* EINTR: 시그널 때문에 잠깐 끊긴 것일 뿐이니 다시 시도합니다. */
            if (errno == EINTR) continue;
            /* 타임아웃(EAGAIN)이나 상대가 연결을 끊은 경우(EPIPE) 등은 실패로 알립니다. */
            return -1;
        }
        if (count == 0) {
            /* 하나도 못 보냈는데 오류도 아니라면, 무한 반복을 피하려고 실패로 처리합니다. */
            errno = EIO;
            return -1;
        }
        /*
         * 참고: send 성공은 "내 컴퓨터의 OS가 받아 갔다"는 뜻이지,
         * 상대방이 실제로 읽었다는 뜻은 아닙니다.
         */
        sent += (size_t)count;
    }
    return 0;
}

/*
 * 응답 하나를 보냅니다: 헤더를 만들어 보내고, 이어서 본문을 보냅니다.
 * omit_body가 1이면(HEAD 요청) 헤더만 보냅니다.
 */
static int send_response(int fd, const HttpResponse *response, int omit_body)
{
    char header[512];
    int length = http_response_header(header, sizeof(header), response->status,
        response->content_type, response->body_length, response->allow);
    if (length < 0) { errno = EINVAL; return -1; }
    if (send_all(fd, (const unsigned char *)header, (size_t)length) == -1) return -1;
    return omit_body ? 0 : send_all(fd, response->body, response->body_length);
}

/* "404 Not Found\n" 같은 간단한 텍스트 오류 응답을 보냅니다. */
static void send_error(int fd, int status, int omit_body)
{
    const char *reason = http_reason(status);
    if (!reason) reason = http_reason(status = 500); /* 모르는 코드면 500으로 대신 */
    char body[128];
    int length = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    HttpResponse response = {
        .status = status, .content_type = "text/plain; charset=utf-8",
        .body = (const unsigned char *)body, .body_length = (size_t)length
    };
    if (send_response(fd, &response, omit_body) == -1) perror("send error response");
}

/*
 * 시간 측정용 현재 시각(밀리초). CLOCK_MONOTONIC은 시스템 시계를 바꿔도
 * 뒤로 가지 않는 시계라서 "걸린 시간"을 잴 때 적합합니다.
 */
static double monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return 0;
    return (double)now.tv_sec * 1000 + (double)now.tv_nsec / 1000000;
}

/*
 * 연결 하나를 처리합니다.
 * 데이터를 조금씩 받아 버퍼에 쌓으면서, 받을 때마다 파서에게 "요청이 완성됐는지" 묻습니다.
 *   - 완성(COMPLETE) → 라우팅 후 응답 전송, 연결 종료
 *   - 오류(ERROR)    → 오류 응답 전송, 연결 종료
 *   - 미완성(NEED_MORE) → recv()로 더 받기
 * TCP는 데이터를 마음대로 쪼개 보낼 수 있어서, 한 번의 recv로 요청이 다 온다는 보장이 없습니다.
 */
static void handle_client(int fd)
{
    static unsigned long long sequence; /* 로그용 요청 번호 (static이라 호출 사이에 값이 유지됨) */
    unsigned long long request_id = ++sequence;
    double started = monotonic_ms();
    /*
     * 수신/송신 타임아웃을 설정합니다. 이게 없으면 아무것도 보내지 않는 클라이언트 하나 때문에
     * 서버 전체가 영원히 멈춰 버립니다(한 번에 한 연결만 처리하므로).
     */
    struct timeval timeout = { .tv_sec = IDLE_SECONDS, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt timeout");
        return;
    }
    /*
     * 헤더 상한 + 본문 상한만큼만 버퍼를 확보합니다.
     * 파서가 돌려주는 request.body는 이 버퍼 안을 가리키므로, 버퍼가 살아 있는 동안만 유효합니다.
     */
    const size_t capacity = HTTP_MAX_HEADER + HTTP_MAX_BODY;
    unsigned char *buffer = malloc(capacity);
    if (!buffer) { send_error(fd, 500, 0); return; }
    size_t used = 0; /* 버퍼에 지금까지 받은 바이트 수 */
    HttpRequest request;
    for (;;) {
        /* 지금까지 받은 데이터로 요청을 해석해 봅니다. (처음엔 used == 0이라 NEED_MORE) */
        size_t consumed;
        int status;
        HttpParseResult parsed = http_parse_request(buffer, used, &request, &consumed, &status);
        /*
         * 파서 상태 관찰용 로그. 요청을 나눠 보내면 NEED_MORE가 여러 번 찍히다가
         * 마지막 조각이 도착했을 때 COMPLETE가 한 번 찍혀야 합니다.
         * COMPLETE 줄의 body 값으로 본문을 몇 바이트로 해석했는지도 확인할 수 있습니다.
         */
        if (parsed == HTTP_NEED_MORE) {
            fprintf(stderr, "[parse] NEED_MORE used=%zu\n", used);
        }
        else if (parsed == HTTP_COMPLETE) {
            fprintf(stderr,
                    "[parse] COMPLETE used=%zu consumed=%zu body=%zu\n",
                    used, consumed, request.body_length);
        }
        else {
            fprintf(stderr, "[parse] ERROR status=%d\n", status);
        }
        /* HEAD 요청이면 본문 없이 헤더만 보내야 합니다. */
        int head = strcmp(request.method, "HEAD") == 0;
        if (parsed == HTTP_ERROR) {
            send_error(fd, status, head);
            break;
        }
        if (parsed == HTTP_COMPLETE) {
            /* 단계별 소요 시간을 재기 위해 시각을 기록해 둡니다. */
            double received_at = monotonic_ms();
            HttpResponse response;
            football_route(&request, &response);   /* 어떤 응답을 줄지 결정 */
            double routed_at = monotonic_ms();
            int sent = send_response(fd, &response, head);
            double sent_at = monotonic_ms();
            if (sent == -1) perror("send response");
            /*
             * 요청 하나에 대한 로그 한 줄. 각 구간의 의미:
             *   receive_ms : accept 직후 ~ 요청을 다 받을 때까지
             *   route_ms   : 응답을 만드는 데 걸린 시간
             *   send_ms    : 응답을 보내는 데 걸린 시간
             *   total_ms   : accept 이후 전체 처리 시간
             * total에는 accept 전 대기열에서 기다린 시간이나 브라우저 polling 간격은 포함되지 않습니다.
             */
            fprintf(stderr, "[http] request_id=%llu fd=%d method=%s target=%s request_bytes=%zu "
                "body_bytes=%zu status=%d receive_ms=%.3f route_ms=%.3f send_ms=%.3f "
                "total_ms=%.3f send_ok=%d\n", request_id, fd, request.method, request.target,
                consumed, request.body_length, response.status, received_at - started,
                routed_at - received_at, sent_at - routed_at, sent_at - started, sent == 0);
            football_response_release(&response);  /* 동적 할당된 본문이 있으면 해제 */
            /*
             * 연결 하나당 요청 하나만 처리하고 닫습니다 (응답에도 "Connection: close").
             * keep-alive나 파이프라이닝(한 연결로 여러 요청)은 구현하지 않았습니다.
             */
            break;
        }
        /* NEED_MORE인데 버퍼가 이미 가득 찼다면 요청이 너무 큰 것입니다. */
        if (used == capacity) { send_error(fd, 413, head); break; }
        /* 버퍼의 남은 공간에 이어서 받습니다. 데이터가 올 때까지(또는 타임아웃까지) 여기서 기다립니다. */
        ssize_t received = recv(fd, buffer + used, capacity - used, 0);
        if (received == -1) {
            if (errno == EINTR) continue; /* 시그널로 끊긴 것이면 다시 시도 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* IDLE_SECONDS 동안 아무것도 오지 않음 → 408 Request Timeout */
                fprintf(stderr, "[timeout] fd=%d: receive idle timeout\n", fd);
                send_error(fd, 408, head);
            } else perror("recv");
            break;
        }
        if (received == 0) {
            /*
             * 0은 상대가 연결을 닫았다(EOF)는 뜻입니다.
             * 아무것도 안 보내고 닫았다면 조용히 끝내고,
             * 요청을 보내다 만 채로 닫았다면 불완전한 요청이므로 400을 보냅니다.
             */
            if (used) send_error(fd, 400, head);
            break;
        }
        used += (size_t)received;
        fprintf(stderr, "[recv] fd=%d bytes=%zd accumulated=%zu\n", fd, received, used);
    }
    free(buffer);
}

int main(int argc, char **argv)
{
    /* ── 명령줄 인자로 포트 번호 받기: ./server [포트] ── */
    long port = DEFAULT_PORT;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port: 1-65535]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        port = strtol(argv[1], &end, 10);
        /* 숫자가 아니거나("abc"), 뒤에 문자가 붙었거나("80x"), 범위를 벗어나면 거절 */
        if (errno || argv[1][0] == '\0' || *end || port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }
    /*
     * 상대가 먼저 연결을 끊은 뒤 send하면 OS가 SIGPIPE 시그널을 보내는데,
     * 기본 동작은 "프로그램 종료"입니다. 이를 무시하도록 해서
     * 서버가 죽지 않고 send의 오류(EPIPE)로 처리하게 합니다.
     */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal SIGPIPE");
        return EXIT_FAILURE;
    }
    int listener = create_listener((uint16_t)port);
    if (listener == -1) return EXIT_FAILURE;

    fprintf(stderr, "Listening on 127.0.0.1:%ld (single thread, blocking, idle timeout %ds)\n",
            port, IDLE_SECONDS);
    for (;;) {
        /*
         * 새 연결이 올 때까지 여기서 기다립니다.
         * 돌려받은 client는 이 연결 전용 소켓으로, listener와는 별개입니다.
         * (listener는 계속 새 연결을 받는 용도로 남아 있습니다.)
         */
        int client = accept(listener, NULL, NULL);
        if (client == -1) {
            /* 시그널로 끊겼거나, 받기 전에 상대가 연결을 취소한 경우는 그냥 다음 연결로 */
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            break;
        }
        fprintf(stderr, "[accept] fd=%d\n", client);
        /* 이 연결 처리가 끝나야 다음 accept로 넘어가는 "순차" 구조입니다. */
        handle_client(client);
        /*
         * 소켓을 연 쪽(main)이 닫는 책임도 집니다. 정상 종료, 타임아웃,
         * 송수신 오류 등 handle_client가 어떻게 끝났든 여기서 한 번만 닫습니다.
         */
        close_socket(client);
        fprintf(stderr, "[close] fd=%d\n", client);
    }
    close_socket(listener);
    return EXIT_FAILURE; /* 정상이라면 위 반복은 끝나지 않으므로, 여기 왔다면 오류입니다. */
}
