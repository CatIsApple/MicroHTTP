/*
 * net.c — 소켓 송수신 도우미
 */
#include "net.h"
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>

void close_socket(int fd)
{
    /*
     * close()가 실패해도 재시도하지 않습니다. 실패 시점에 fd 번호가 이미 반납되어
     * 다른 스레드가 새 소켓에 같은 번호를 받았을 수 있어서, 재시도하면 남의 소켓을 닫게 됩니다.
     */
    if (close(fd) == -1) perror("close");
}

int send_all(int fd, const void *data, size_t length)
{
    /* send()는 요청한 바이트 중 일부만 보낼 수 있습니다(커널 송신 버퍼가 찼을 때 등). 그래서 반복합니다. */
    const unsigned char *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, bytes + sent, length - sent, 0);
        if (count == -1) {
            if (errno == EINTR) continue; /* 시그널로 잠깐 끊긴 것 → 다시 시도 */
            return -1;                    /* EPIPE(상대가 끊음), EAGAIN(송신 타임아웃) 등 */
        }
        if (count == 0) { errno = EIO; return -1; } /* 진행이 없으면 무한 반복 대신 실패 */
        sent += (size_t)count;
    }
    return 0;
}

int set_socket_timeout(int fd, int seconds)
{
    /*
     * 블로킹 소켓의 recv/send는 상대가 아무것도 하지 않으면 영원히 기다립니다.
     * 타임아웃을 걸어 두면 일정 시간 뒤 EAGAIN으로 돌아오므로, 느리거나 멈춘 상대 때문에
     * 스레드가 영원히 묶이는 일을 막을 수 있습니다.
     */
    struct timeval timeout = { .tv_sec = seconds, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt timeout");
        return -1;
    }
    return 0;
}

int send_error_response(int fd, int status, int keep_alive, int omit_body, const char *allow)
{
    const char *reason = http_reason(status);
    if (!reason) reason = http_reason(status = 500);
    char body[64];
    int body_length = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    char header[256];
    int header_length = http_response_header(header, sizeof(header), status,
        "text/plain; charset=utf-8", (size_t)body_length, keep_alive, allow);
    if (body_length < 0 || header_length < 0) return -1;
    if (send_all(fd, header, (size_t)header_length) == -1) return -1;
    return omit_body ? 0 : send_all(fd, body, (size_t)body_length);
}

int connect_with_timeout(const char *host, const char *port, int timeout_ms)
{
    /* "localhost" 같은 이름도 쓸 수 있도록 getaddrinfo로 주소를 찾습니다. */
    struct addrinfo hints, *addresses;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(host, port, &hints, &addresses);
    if (error) {
        fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(error));
        errno = EHOSTUNREACH;
        return -1;
    }
    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == -1) { saved_errno = errno; continue; }
        /*
         * 블로킹 connect는 응답 없는 주소에서 OS 기본값(수십 초~수 분)만큼 멈춥니다.
         * 논블로킹으로 바꾸면 connect가 즉시 EINPROGRESS로 돌아오고,
         * poll로 "쓰기 가능(=연결 완료)"이 될 때까지 timeout_ms만 기다릴 수 있습니다.
         */
        int flags = fcntl(fd, F_GETFL, 0);
        int result = flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (result == 0) result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result == -1 && errno == EINPROGRESS) {
            struct pollfd waiting = { .fd = fd, .events = POLLOUT };
            int ready = poll(&waiting, 1, timeout_ms);
            if (ready == 1) {
                /* 연결 결과(성공 0 / ECONNREFUSED 등)는 SO_ERROR로 확인합니다. */
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
                if (result == 0 && socket_error) { errno = socket_error; result = -1; }
            } else {
                if (ready == 0) errno = ETIMEDOUT;
                result = -1;
            }
        }
        /* 이후 recv/send는 SO_RCVTIMEO를 쓰는 블로킹 방식으로 하므로 원래 모드로 되돌립니다. */
        if (result == 0 && fcntl(fd, F_SETFL, flags) == 0) break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd == -1) errno = saved_errno;
    return fd;
}
