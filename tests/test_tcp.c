/* test_tcp.c — L1 단위 테스트.
 * socketpair() 로 서로 연결된 소켓 한 쌍을 만들어 네트워크 없이 L1 함수를 확인한다.
 *
 * 구성: 테스트 함수 하나가 한 가지 성질을 확인한다 (읽기, cap, EOF, 마감 시각, 보내기 실패, errno).
 * EXPECT 가 실패하면 위치를 찍고 failures 를 늘리며, main 은 실패가 하나라도 있으면 1 을 돌려준다. */
#define _POSIX_C_SOURCE 200809L

#include "tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;
#define EXPECT(c)                                                        \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

/* 서로 연결된 소켓 한 쌍을 만든다 (sv[0] ↔ sv[1]) */
static void pair(int sv[2]) { EXPECT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0); }
/* pair 로 만든 소켓 두 개를 닫는다 */
static void unpair(int sv[2]) { close(sv[0]); close(sv[1]); }

/* 보낸 바이트가 그대로 읽힌다 */
static void send_then_recv(void)
{
    int sv[2]; pair(sv);
    EXPECT(tcp_send_all(sv[0], "hello", 5, tcp_now_ms() + 1000) == 0);
    char b[16];
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, tcp_now_ms() + 1000) == 5);
    EXPECT(memcmp(b, "hello", 5) == 0);
    unpair(sv);
}

/* 요청이 끝났는지와 상관없이 "와 있는 만큼" 곧바로 돌려준다 */
static void returns_what_has_arrived(void)
{
    int sv[2]; pair(sv);
    EXPECT(tcp_send_all(sv[0], "GET / HT", 8, tcp_now_ms() + 1000) == 0);
    char b[64];
    long long t0 = tcp_now_ms();
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, t0 + 2000) == 8);
    EXPECT(tcp_now_ms() - t0 < 500);
    unpair(sv);
}

/* cap 을 넘게 읽지 않고, 나머지는 다음 호출에서 나온다 */
static void respects_cap(void)
{
    int sv[2]; pair(sv);
    EXPECT(tcp_send_all(sv[0], "abcdefg", 7, tcp_now_ms() + 1000) == 0);
    char b[4];
    EXPECT(tcp_recv_some(sv[1], b, 4, tcp_now_ms() + 1000) == 4);
    EXPECT(memcmp(b, "abcd", 4) == 0);
    EXPECT(tcp_recv_some(sv[1], b, 4, tcp_now_ms() + 1000) == 3);
    EXPECT(memcmp(b, "efg", 3) == 0);
    unpair(sv);
}

/* 상대가 닫으면 0 */
static void eof_is_zero(void)
{
    int sv[2]; pair(sv);
    shutdown(sv[0], SHUT_WR);
    char b[4];
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, tcp_now_ms() + 1000) == 0);
    unpair(sv);
}

/* 아무것도 안 오면 마감에서 ETIMEDOUT */
static void recv_timeout(void)
{
    int sv[2]; pair(sv);
    char b[4];
    long long t0 = tcp_now_ms();
    errno = 0;
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, t0 + 100) == -1);
    EXPECT(errno == ETIMEDOUT);
    long long took = tcp_now_ms() - t0;
    EXPECT(took >= 90 && took < 1000);
    unpair(sv);
}

/* 마감은 절대 시각이라 여러 번 나눠 불러도 늘어나지 않는다 (slowloris 방어의 핵심) */
static void deadline_is_shared(void)
{
    int sv[2]; pair(sv);
    long long t0 = tcp_now_ms(), deadline = t0 + 150;
    char b[4];
    EXPECT(tcp_send_all(sv[0], "x", 1, deadline) == 0);
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, deadline) == 1);
    errno = 0;
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, deadline) == -1);
    EXPECT(errno == ETIMEDOUT);
    EXPECT(tcp_now_ms() - t0 < 1000);
    /* 이미 지난 마감이면 기다리지 않는다 */
    errno = 0;
    EXPECT(tcp_recv_some(sv[1], b, sizeof b, tcp_now_ms() - 1) == -1 && errno == ETIMEDOUT);
    unpair(sv);
}

/* 상대가 읽지 않아 송신 버퍼가 차면 send_all 도 마감에서 멈춘다 */
static void send_timeout(void)
{
    int sv[2]; pair(sv);
    static char big[8 << 20];
    long long t0 = tcp_now_ms();
    errno = 0;
    EXPECT(tcp_send_all(sv[0], big, sizeof big, t0 + 200) == -1);
    EXPECT(errno == ETIMEDOUT);
    EXPECT(tcp_now_ms() - t0 < 1500);
    unpair(sv);
}

/* 상대가 이미 닫은 소켓에 보내면 프로세스가 죽지 않고 -1 */
static void send_to_closed_peer(void)
{
    int sv[2]; pair(sv);
    close(sv[1]);
    char b[1024] = {0};
    int r = 0;
    for (int i = 0; i < 10 && r == 0; i++)
        r = tcp_send_all(sv[0], b, sizeof b, tcp_now_ms() + 500);
    EXPECT(r == -1);
    close(sv[0]);
}

/* cap 이 0 이면 EOF 와 헷갈리지 않도록 -1, errno = EINVAL */
static void zero_cap_is_einval(void)
{
    int sv[2]; pair(sv);
    char b[1];
    errno = 0;
    EXPECT(tcp_recv_some(sv[1], b, 0, tcp_now_ms() + 100) == -1 && errno == EINVAL);
    unpair(sv);
}

/* listen 실패 원인이 errno 에 그대로 남는다 (L1 은 로그 대신 errno 로만 알린다) */
static void listen_keeps_errno(void)
{
    int a = tcp_listen(0, 8); /* 포트 0 = 커널이 빈 포트 배정 */
    EXPECT(a >= 0);
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    EXPECT(getsockname(a, (struct sockaddr *)&addr, &len) == 0);
    errno = 0;
    EXPECT(tcp_listen(ntohs(addr.sin_port), 8) == -1);
    EXPECT(errno == EADDRINUSE);
    close(a);
}

/* 모든 테스트를 차례로 실행하고, 실패가 있으면 개수를 찍고 1 을 돌려준다 */
int main(void)
{
    send_then_recv();
    returns_what_has_arrived();
    respects_cap();
    eof_is_zero();
    recv_timeout();
    deadline_is_shared();
    send_timeout();
    send_to_closed_peer();
    zero_cap_is_einval();
    listen_keeps_errno();
    if (failures) {
        fprintf(stderr, "test_tcp: %d failed\n", failures);
        return 1;
    }
    printf("test_tcp: ok\n");
    return 0;
}
