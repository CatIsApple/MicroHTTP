/* test_server.c — server_reject_busy 단위 테스트 (실제 TCP 루프백: RST 동작은 TCP 에서만 재현됨).
 *
 * 핵심: 클라이언트가 요청을 보낸 뒤 서버가 바빠서 거절할 때, 클라이언트는
 *   503 응답을 끝까지 받고 → 그 다음 읽기에서 EOF(0) 를 봐야 한다 (ECONNRESET 이면 안 됨).
 * 요청을 읽지 않은 채 그냥 close 하면 커널이 RST 를 보내서 이 검사가 실패한다. */

#define _POSIX_C_SOURCE 200809L /* nanosleep 등 */

#include "server.h"     /* 테스트 대상: server_reject_busy */
#include "tcp_server.h" /* tcp_listen, tcp_accept */
#include "test.h"       /* CHECK, CHECK_INT, TEST_REPORT */
#include <arpa/inet.h>  /* inet_pton, htons */
#include <errno.h>      /* errno */
#include <netinet/in.h> /* struct sockaddr_in */
#include <signal.h>     /* signal, SIGPIPE */
#include <string.h>     /* strlen, strncmp, memset */
#include <sys/socket.h> /* socket, connect, getsockname */
#include <sys/time.h>   /* struct timeval (SO_RCVTIMEO) */
#include <time.h>       /* nanosleep */
#include <unistd.h>     /* read, write, close */

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* 127.0.0.1:port 로 접속 (읽기 타임아웃 2초) */
static int connect_to(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 접속 → (req 가 있으면 보냄) → 서버 쪽 accept 해서 server_reject_busy.
 * 클라이언트 쪽에서 EOF 까지 읽어 buf 에 담는다. 반환: 마지막 read 결과 (0 = EOF, -1 = 오류 → *err) */
static ssize_t reject_case(int lfd, unsigned short port, const char *req, int client_closes,
                           char *buf, size_t cap, size_t *got, int *err)
{
    *got = 0;
    *err = 0;
    int cfd = connect_to(port);
    if (cfd < 0) {
        *err = errno;
        return -1;
    }
    if (req) {
        ssize_t w = write(cfd, req, strlen(req));
        (void)w;
    }
    if (client_closes)
        shutdown(cfd, SHUT_WR);                    /* 요청 없이 송신을 닫음 (FIN) */
    sleep_ms(50);                                  /* 요청 바이트가 서버 수신 버퍼에 도착할 시간 */

    char peer[CONN_PEER_MAX];
    int sfd = -1;
    for (int i = 0; i < 100 && sfd < 0; i++) {     /* 리스닝 소켓은 블로킹 → 한 번이면 되지만 방어적으로 */
        sfd = tcp_accept(lfd, peer, sizeof(peer));
    }
    if (sfd < 0) {
        *err = errno;
        close(cfd);
        return -1;
    }
    server_reject_busy(sfd);                       /* 테스트 대상 */

    ssize_t r;
    for (;;) {                                     /* EOF 또는 오류까지 */
        r = read(cfd, buf + *got, cap - 1 - *got);
        if (r <= 0)
            break;
        *got += (size_t)r;
    }
    if (r < 0)
        *err = errno;
    buf[*got] = '\0';
    close(cfd);
    return r;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    int lfd = tcp_listen(0, 16);                   /* 포트 0 → 커널이 빈 포트를 고름 */
    CHECK(lfd >= 0);
    if (lfd < 0)
        return TEST_REPORT();
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    getsockname(lfd, (struct sockaddr *)&a, &al);
    unsigned short port = ntohs(a.sin_port);

    char buf[1024];
    size_t got;
    int err;

    /* 1. 요청이 이미 와 있는 연결을 거절 → 503 을 다 받고 EOF (RST 가 아님) */
    for (int round = 0; round < 20; round++) {     /* RST 는 타이밍에 따라 가끔만 보일 수 있어 여러 번 */
        ssize_t r = reject_case(lfd, port, "GET / HTTP/1.1\r\nHost: t\r\n\r\n", 0, buf, sizeof(buf), &got, &err);
        CHECK_INT(r, 0);                           /* 마지막 read = EOF */
        CHECK_INT(err, 0);                         /* ECONNRESET 이 아님 */
        CHECK(strncmp(buf, "HTTP/1.1 503 ", 13) == 0);
        if (r != 0 || strncmp(buf, "HTTP/1.1 503 ", 13) != 0) {
            fprintf(stderr, "round %d: r=%zd err=%d got=%zu\n", round, r, err, got);
            break;
        }
    }

    /* 2. 아직 아무것도 안 보낸 연결을 거절 → 그래도 503 (클라이언트는 곧 요청을 보낼 참이므로) */
    ssize_t r = reject_case(lfd, port, NULL, 0, buf, sizeof(buf), &got, &err);
    CHECK_INT(r, 0);
    CHECK(strncmp(buf, "HTTP/1.1 503 ", 13) == 0);

    /* 3. 아무것도 안 보내고 이미 닫은 연결 → 503 없이 그냥 닫힘 */
    r = reject_case(lfd, port, NULL, 1, buf, sizeof(buf), &got, &err);
    CHECK_INT(r, 0);
    CHECK_INT(got, 0);

    /* 4. 요청을 보내고 송신까지 닫은 연결 → 503 (요청은 받았으니 답한다) */
    r = reject_case(lfd, port, "GET / HTTP/1.1\r\n\r\n", 1, buf, sizeof(buf), &got, &err);
    CHECK_INT(r, 0);
    CHECK(strncmp(buf, "HTTP/1.1 503 ", 13) == 0);

    close(lfd);
    return TEST_REPORT();
}
