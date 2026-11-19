#define _POSIX_C_SOURCE 200809L

#include "tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

int tcp_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* EPIPE 등: 상대가 끊음 (SIGPIPE 는 main 에서 무시) */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

ssize_t tcp_recv_request(int fd, char *buf, size_t cap)
{
    if (cap == 0)
        return -1;

    size_t used = 0;
    buf[0] = '\0';

    /* TCP 는 바이트 스트림이라 한 번의 recv 로 요청이 다 온다는 보장이 없다.
     * 헤더 끝 표시(빈 줄)가 보일 때까지 누적해서 읽는다. */
    while (used < cap - 1) {
        ssize_t n = recv(fd, buf + used, cap - 1 - used, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) /* 상대가 연결을 닫음 */
            break;

        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL)
            break;
    }
    return (ssize_t)used;
}
