/* tcp.c — L1: TCP 소켓 레이어 구현. 인터페이스 설명은 include/tcp.h */
#define _POSIX_C_SOURCE 200809L

#include "tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Linux 는 send 에 MSG_NOSIGNAL 을 주면 끊긴 연결에 써도 SIGPIPE 가 안 온다.
 * macOS 에는 이 플래그가 없으므로 0 으로 두고, 서버가 SIGPIPE 를 무시하는 것에 기댄다. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define CLOSE_DRAIN_MS 500             /* 닫기 전에 남은 입력을 비우는 최대 시간 */
#define CLOSE_DRAIN_BYTES (64 * 1024)  /* 비우면서 버릴 최대 바이트 수 */

long long tcp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 실패 처리 도우미: fd 를 닫되 errno 는 원래 원인 그대로 남긴다 (close 가 errno 를 바꿀 수 있음) */
static int fail_close(int fd)
{
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/* exec 할 때 이 fd 가 자식 프로세스로 넘어가지 않게 표시 (나중에 CGI 를 fork/exec 할 때 대비) */
static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* fd 가 events(POLLIN/POLLOUT) 상태가 될 때까지 마감 시각까지 기다린다.
 * 반환: 0 준비됨, -1 오류 또는 마감 초과(ETIMEDOUT) */
static int wait_ready(int fd, short events, long long deadline_ms)
{
    for (;;) {
        long long left = deadline_ms - tcp_now_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int n = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (n > 0)
            return 0; /* 읽기/쓰기 가능, 또는 오류/끊김 (실제 recv/send 에서 드러남) */
        if (n == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR) /* 시그널로 깨어난 경우만 다시 기다린다 */
            return -1;
    }
}

int tcp_listen(uint16_t port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0)
        return fail_close(fd);

    /* 서버를 껐다 켰을 때 TIME_WAIT 때문에 bind 가 실패하지 않도록 */
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0)
        return fail_close(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 모든 네트워크 인터페이스 */
    addr.sin_port = htons(port);              /* 네트워크 바이트 순서(빅엔디언)로 */
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        return fail_close(fd);
    if (listen(fd, backlog) < 0)
        return fail_close(fd);
    return fd;
}

int tcp_accept(int lfd, char *peer, size_t peer_len)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof addr;
    int fd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0)
        return fail_close(fd);

    if (peer && peer_len > 0) {
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip))
            strcpy(ip, "?");
        snprintf(peer, peer_len, "%s:%u", ip, (unsigned)ntohs(addr.sin_port));
    }
    return fd;
}

ssize_t tcp_recv_some(int fd, void *buf, size_t cap, long long deadline_ms)
{
    if (cap == 0) { /* 0 바이트 읽기는 결과 0 이 EOF 와 구분되지 않으므로 막는다 */
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        if (wait_ready(fd, POLLIN, deadline_ms) < 0)
            return -1;
        /* MSG_DONTWAIT: poll 이 깨웠는데 막상 데이터가 없는 경우에도 붙잡히지 않게 */
        ssize_t n = recv(fd, buf, cap, MSG_DONTWAIT);
        if (n >= 0)
            return n;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1; /* ECONNRESET 등 */
    }
}

int tcp_send_all(int fd, const void *buf, size_t len, long long deadline_ms)
{
    const char *p = buf;
    while (len > 0) {
        if (wait_ready(fd, POLLOUT, deadline_ms) < 0)
            return -1;
        /* send 는 송신 버퍼에 들어간 만큼만 보내고 돌아올 수 있다 → 남은 만큼 반복 */
        ssize_t n = send(fd, p, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1; /* EPIPE, ECONNRESET: 상대가 끊음 */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void tcp_close_graceful(int fd)
{
    /* 1. "더 보낼 것 없음" (FIN). 클라이언트는 응답을 끝까지 읽고 자기도 닫는다. */
    if (shutdown(fd, SHUT_WR) < 0) {
        close(fd); /* 이미 끊긴 연결 */
        return;
    }
    /* 2. 클라이언트가 닫을 때까지(EOF) 남은 입력을 읽어 버린다. 시간·바이트 상한을 둔다. */
    long long deadline = tcp_now_ms() + CLOSE_DRAIN_MS;
    char junk[4096];
    size_t total = 0;
    while (total < CLOSE_DRAIN_BYTES) {
        ssize_t n = tcp_recv_some(fd, junk, sizeof junk, deadline);
        if (n <= 0)
            break; /* EOF, 오류, 마감 */
        total += (size_t)n;
    }
    close(fd);
}
