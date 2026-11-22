#define _POSIX_C_SOURCE 200809L

#include "tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int tcp_listen(uint16_t port, int backlog)
{
    /* 1. socket(): IPv4 TCP 소켓 생성 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 서버 재시작 시 TIME_WAIT 상태 포트에 바로 bind 할 수 있도록 */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    /* 2. bind(): 모든 인터페이스(0.0.0.0)의 port 에 바인딩 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* 3. listen(): 커널이 3-way handshake 를 마친 연결을 backlog 큐에 쌓는다 */
    if (listen(fd, backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int tcp_accept(int listen_fd, char *client_addr_buf, size_t buf_len)
{
    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);

    /* 4. accept(): 완료 큐에서 연결 하나를 꺼낸다 (없으면 블로킹) */
    int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
    if (cfd < 0)
        return -1;

    if (client_addr_buf && buf_len > 0) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        snprintf(client_addr_buf, buf_len, "%s:%u", ip, (unsigned)ntohs(cli.sin_port));
    }
    return cfd;
}

/* ---- 전체 마감 시간(deadline) 도우미 ----
 * SO_RCVTIMEO 는 "recv 한 번" 의 대기 시간만 제한한다. 1바이트씩 천천히
 * 보내는 클라이언트(slowloris)는 매번 시간 안에 도착하므로 끝없이 버틴다.
 * 그래서 시작 시각 기준으로 남은 시간을 계산해 poll() 에 넘긴다. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* deadline 까지 fd 에 events 가 생기길 기다린다.
 * 1 = 준비됨, 0 = 마감 초과(errno = ETIMEDOUT), -1 = 오류 */
static int wait_until(int fd, short events, long long deadline)
{
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return 0;
        }
        struct pollfd pfd = {.fd = fd, .events = events};
        int r = poll(&pfd, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r == 0) {
            errno = ETIMEDOUT;
            return 0;
        }
        return r < 0 ? -1 : 1;
    }
}

int tcp_send_all(int fd, const void *buf, size_t len, int timeout_ms)
{
    const char *p = buf;
    long long deadline = now_ms() + timeout_ms;
    while (len > 0) {
        if (wait_until(fd, POLLOUT, deadline) <= 0)
            return -1;
        /* MSG_DONTWAIT: 송신 버퍼가 일부만 비어 있어도 블로킹하지 않고
         * 들어간 만큼만 복사한다 → 마감 시간을 넘겨 붙잡히지 않는다 */
        ssize_t n = send(fd, p, len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1; /* EPIPE 등: 상대가 끊음 (SIGPIPE 는 main 에서 무시) */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

ssize_t tcp_recv_request(int fd, char *buf, size_t cap, int timeout_ms, size_t *header_len)
{
    *header_len = 0;
    if (cap == 0)
        return -1;

    size_t used = 0;
    buf[0] = '\0';
    long long deadline = now_ms() + timeout_ms;

    /* TCP 는 바이트 스트림이라 한 번의 recv 로 요청이 다 온다는 보장이 없다.
     * 헤더 끝 표시(빈 줄)가 보일 때까지 누적해서 읽되, 전체 마감 시간을 지킨다. */
    while (used < cap - 1) {
        if (wait_until(fd, POLLIN, deadline) <= 0)
            return -1;
        ssize_t n = recv(fd, buf + used, cap - 1 - used, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (n == 0) /* 상대가 연결을 닫음 */
            break;

        /* 새로 받은 구간만 검사한다. 단 "\r\n\r\n" 이 이전 데이터와 걸칠 수
         * 있으므로 3바이트 앞부터. (매번 처음부터 strstr 하면 O(n^2)) */
        size_t scan_from = used >= 3 ? used - 3 : 0;
        used += (size_t)n;
        buf[used] = '\0';
        char *end = strstr(buf + scan_from, "\r\n\r\n");
        if (end != NULL) {
            *header_len = (size_t)(end - buf) + 4; /* 빈 줄까지 포함한 헤더 길이 */
            break;
        }
    }
    return (ssize_t)used;
}

#define DRAIN_TIMEOUT_MS 500
#define DRAIN_MAX_BYTES (64 * 1024)

void tcp_close_graceful(int fd)
{
    /* 읽지 않은 수신 데이터가 없으면 그냥 close 해도 RST 가 나가지 않는다.
     * 이때는 상대의 FIN 을 기다릴 이유가 없으므로 바로 닫는다. */
    char peek;
    if (recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {
        close(fd);
        return;
    }

    /* 1. 더 보낼 게 없다고 알림(FIN). 상대는 응답을 다 읽고 연결을 닫는다. */
    shutdown(fd, SHUT_WR);

    /* 2. 남은 데이터를 읽어 버린다. 시간 상한은 호출당이 아니라 "전체" 기준이고,
     *    바이트 상한도 함께 둬서 악의적 클라이언트에 붙잡히지 않게 한다. */
    long long deadline = now_ms() + DRAIN_TIMEOUT_MS;
    char junk[4096];
    size_t total = 0;
    while (total < DRAIN_MAX_BYTES && wait_until(fd, POLLIN, deadline) > 0) {
        ssize_t n = recv(fd, junk, sizeof(junk), MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0) /* 상대가 닫음(FIN) 또는 오류 */
            break;
        total += (size_t)n;
    }
    close(fd);
}
