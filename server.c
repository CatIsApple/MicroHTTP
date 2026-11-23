#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum { DEFAULT_PORT = 8080, BACKLOG = 128, IDLE_SECONDS = 5 };

static void close_socket(int fd)
{
    /* close() 실패 후 재시도하면 이미 재사용된 다른 fd를 닫을 수 있다. */
    if (close(fd) == -1) perror("close");
}

static int create_listener(uint16_t port)
{
    /* 운영체제가 TCP 소켓을 만들고, 프로그램이 사용할 fd를 반환한다. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    /* 재시작 시 이전 연결의 TIME_WAIT 때문에 bind가 실패하는 경우를 줄인다. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        goto fail;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    /* 같은 컴퓨터에서 실험하도록 루프백 주소에만 바인딩한다. */
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fprintf(stderr, "inet_pton: invalid address\n");
        goto fail;
    }
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        goto fail;
    }
    /* 128은 대기열에 대한 요청값이다. 128개 연결을 동시에 처리한다는 뜻은 아니다. */
    if (listen(fd, BACKLOG) == -1) {
        perror("listen");
        goto fail;
    }
    return fd;

fail:
    close_socket(fd);
    return -1;
}

static int send_all(int fd, const unsigned char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);
        if (count == -1) {
            if (errno == EINTR) continue;
            /* 타임아웃이나 연결 끊김도 호출자에게 실패로 전달한다. */
            return -1;
        }
        if (count == 0) {
            /* 진행 없는 반환을 반복해 무한 루프에 빠지지 않는다. */
            errno = EIO;
            return -1;
        }
        /* send 성공은 상대의 읽기 완료가 아니라 로컬에서 받아들인 바이트 수다. */
        sent += (size_t)count;
    }
    return 0;
}

static void handle_client(int fd)
{
    struct timeval timeout = { .tv_sec = IDLE_SECONDS, .tv_usec = 0 };
    /* 유휴 클라이언트와 응답을 읽지 않는 클라이언트의 무한 대기를 제한한다. */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt timeout");
        return;
    }

    unsigned char buffer[4096];
    for (;;) {
        /* TCP에는 메시지 경계가 없다. 한 번의 recv로 입력 전체를 받았다고 가정하지 않는다. */
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                fprintf(stderr, "[timeout] fd=%d: no received data for about %d seconds\n", fd, IDLE_SECONDS);
            else perror("recv");
            return;
        }
        /* 상대가 송신을 정상 종료했다. 앞서 읽은 바이트는 이미 모두 응답했다. */
        if (received == 0) return;

        fprintf(stderr, "[recv] fd=%d bytes=%zd\n", fd, received);
        /* NUL이 포함될 수 있으므로 strlen 대신 실제 수신 길이만 송신한다. */
        if (send_all(fd, buffer, (size_t)received) == -1) {
            perror("send");
            return;
        }
    }
}

int main(int argc, char **argv)
{
    long port = DEFAULT_PORT;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port: 1-65535]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        port = strtol(argv[1], &end, 10);
        if (errno || argv[1][0] == '\0' || *end || port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }
    /* 상대가 먼저 닫아도 SIGPIPE로 서버가 종료되지 않고 send 오류를 처리한다. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal SIGPIPE");
        return EXIT_FAILURE;
    }
    int listener = create_listener((uint16_t)port);
    if (listener == -1) return EXIT_FAILURE;

    fprintf(stderr, "Listening on 127.0.0.1:%ld (single thread, blocking, idle timeout %ds)\n",
            port, IDLE_SECONDS);
    for (;;) {
        /* 연결이 없으면 여기서 기다린다. 반환된 fd는 listener와 별개인 연결 소켓이다. */
        int client = accept(listener, NULL, NULL);
        if (client == -1) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            break;
        }
        fprintf(stderr, "[accept] fd=%d\n", client);
        /* 이 처리가 끝나기 전에는 다음 accept로 돌아가지 않는 순차 구조다. */
        handle_client(client);
        /* 소유자인 main이 정상 종료·타임아웃·수신/송신 오류 모두에서 정리한다. */
        close_socket(client);
        fprintf(stderr, "[close] fd=%d\n", client);
    }
    close_socket(listener);
    return EXIT_FAILURE;
}
