/* test_tcp_server.c — tcp_recv_request / tcp_recv_exact / tcp_close_graceful 단위 테스트.
 *
 * 수신 쪽 경계 조건이 핵심이라 "데이터가 어떻게 쪼개져 도착하는지" 를 직접 만든다:
 *   - pre  : tcp_recv_request 호출 "전에" 부모가 미리 써 둠 → 첫 recv 는 정확히 이만큼 받는다
 *   - later: 자식이 delay_ms 뒤에 씀 → 두 번째 이후 recv 로 들어온다
 * 이렇게 하면 "\r\n\r\n" 이 recv 두 번에 걸쳐 쪼개지는 경우를 원하는 위치에서 만들 수 있다. */

#define _POSIX_C_SOURCE 200809L /* socketpair, fork, kill, nanosleep, clock_gettime 등 POSIX 함수 */

#include "tcp_server.h" /* 테스트 대상 */
#include "test.h"       /* CHECK, CHECK_INT, CHECK_STR, TEST_REPORT */
#include <arpa/inet.h>  /* inet_pton, htons, ntohs */
#include <errno.h>      /* errno, ETIMEDOUT, ECONNRESET */
#include <netinet/in.h> /* struct sockaddr_in */
#include <signal.h>     /* kill, SIGKILL, signal, SIGPIPE */
#include <stdlib.h>     /* malloc, free, _Exit */
#include <string.h>     /* strlen, memcmp, memset */
#include <sys/socket.h> /* socketpair, socket, connect, getsockname, setsockopt */
#include <sys/time.h>   /* struct timeval (SO_RCVTIMEO) */
#include <sys/wait.h>   /* waitpid */
#include <time.h>       /* nanosleep, clock_gettime */
#include <unistd.h>     /* fork, read, write, close */

static void sleep_ms(long ms) /* 밀리초 단위로 잠들기 */
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static long long now_ms(void) /* 경과 시간 측정용 */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all(int fd, const char *p, size_t len) /* len 바이트 전부 쓰기 */
{
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* 상대(클라이언트) 가 보낼 바이트 시나리오 */
typedef struct {
    const char *pre;      /* 호출 전에 미리 넣어 둘 바이트 (NULL 이면 없음) */
    size_t pre_len;       /* pre 길이 (0 이면 strlen) */
    const char *later;    /* delay_ms 뒤에 보낼 바이트 (NULL 이면 없음) */
    size_t later_len;     /* later 길이 (0 이면 strlen) */
    int delay_ms;         /* later 전 대기 / drip 이면 바이트 사이 간격 */
    int drip;             /* 1 이면 later 를 1바이트씩 delay_ms 간격으로 (slowloris) */
    int hold_ms;          /* 다 보낸 뒤 닫기 전에 버티는 시간 (0 이면 바로 닫음 = EOF) */
} feed;

/* 시나리오대로 보내는 자식을 띄우고, 서버 쪽 fd 를 돌려준다 */
static int start_feed(const feed *f, pid_t *pid)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (f->pre)                                               /* 부모가 미리 써 둠 → 첫 recv 가 받을 양이 확정됨 */
        write_all(sv[1], f->pre, f->pre_len ? f->pre_len : strlen(f->pre));
    *pid = fork();
    if (*pid == 0) {                                          /* 자식: 나머지를 보내는 클라이언트 */
        close(sv[0]);
        if (f->later) {
            size_t len = f->later_len ? f->later_len : strlen(f->later);
            if (f->drip) {                                    /* 1바이트씩 천천히 */
                for (size_t i = 0; i < len; i++) {
                    sleep_ms(f->delay_ms);
                    if (write_all(sv[1], f->later + i, 1) < 0)
                        _Exit(1);
                }
            } else {                                          /* 잠깐 뒤에 한 번에 */
                sleep_ms(f->delay_ms);
                if (write_all(sv[1], f->later, len) < 0)
                    _Exit(1);
            }
        }
        if (f->hold_ms > 0)                                   /* 닫지 않고 버팀 (타임아웃 유도) */
            sleep_ms(f->hold_ms);
        close(sv[1]);                                         /* EOF */
        _Exit(0);
    }
    close(sv[1]);                                             /* 부모는 서버 쪽만 */
    return sv[0];
}

static void end_feed(int fd, pid_t pid) /* 정리: 소켓 닫고 자식 회수 */
{
    close(fd);
    kill(pid, SIGKILL); /* 버티는 자식 정리 (이미 끝났으면 무해) */
    waitpid(pid, NULL, 0);
}

static const char REQ[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n"; /* 헤더만 있는 요청 */
#define REQ_LEN (sizeof(REQ) - 1)

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* 끊긴 소켓에 써도 죽지 않게 */
    char buf[256];
    size_t hl;
    ssize_t n;
    pid_t pid;
    int fd;

    /* ==== tcp_recv_request ==== */

    /* 한 번에 도착 + 뒤에 본문 일부 → 헤더 길이는 빈 줄까지, 반환은 받은 전체 */
    {
        feed f = {.pre = "GET / HTTP/1.1\r\nHost: a\r\n\r\nBODY"};
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN + 4);
        CHECK_INT(hl, REQ_LEN);
        CHECK_INT(buf[n], '\0');                              /* 항상 NUL 종료 */
        end_feed(fd, pid);
    }

    /* "\r\n\r\n" 이 recv 두 번에 걸쳐 쪼개짐: "\r" | "\n\r\n", "\r\n" | "\r\n", "\r\n\r" | "\n" */
    for (size_t k = REQ_LEN - 3; k < REQ_LEN; k++) {
        feed f = {.pre = REQ, .pre_len = k, .later = REQ + k, .delay_ms = 50};
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN);
        CHECK_INT(hl, REQ_LEN);                               /* 쪼개져도 헤더 끝을 찾아야 함 */
        end_feed(fd, pid);
    }

    /* 요청 라인만 먼저, 나머지는 1바이트씩 (재검색 시작 위치 계산 검증) */
    {
        feed f = {.pre = "GET / HTTP/1.1\r\n", .later = "Host: a\r\n\r\n", .delay_ms = 5, .drip = 1};
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 2000, &hl);
        CHECK_INT(n, (long long)REQ_LEN);
        CHECK_INT(hl, REQ_LEN);
        end_feed(fd, pid);
    }

    /* 헤더 중간의 "\r\n" 만으로는 끝이 아님 (빈 줄이 와야 끝) */
    {
        feed f = {.pre = "GET / HTTP/1.1\r\nHost: a\r\n"};      /* 빈 줄 없이 EOF */
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 1000, &hl);
        CHECK_INT(n, 25);                                     /* 받은 만큼은 돌려줌 */
        CHECK_INT(hl, 0);                                     /* 헤더 끝 못 찾음 → 호출자가 400 */
        end_feed(fd, pid);
    }

    /* 아무것도 안 보내고 닫음 → 0 */
    {
        feed f = {0};
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 1000, &hl);
        CHECK_INT(n, 0);
        CHECK_INT(hl, 0);
        end_feed(fd, pid);
    }

    /* 버퍼가 꽉 참: cap-1 바이트까지만 받고 header_len 0 (→ 호출자가 431) */
    {
        feed f = {.pre = "GET /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n", .hold_ms = 2000};
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, 16, 0, 1000, &hl);
        CHECK_INT(n, 15);                                     /* 마지막 1바이트는 '\0' 자리 */
        CHECK_INT(hl, 0);
        CHECK_INT(buf[15], '\0');
        end_feed(fd, pid);
    }

    /* 경계: 요청이 정확히 cap-1 이면 찾음, cap 이면 한 바이트 모자라서 못 찾음 */
    {
        feed f = {.pre = REQ, .hold_ms = 2000};
        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, REQ_LEN + 1, 0, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN);
        CHECK_INT(hl, REQ_LEN);
        end_feed(fd, pid);

        fd = start_feed(&f, &pid);
        n = tcp_recv_request(fd, buf, REQ_LEN, 0, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN - 1);
        CHECK_INT(hl, 0);
        end_feed(fd, pid);
    }

    /* 보내다 멈춤 → 마감 시간에 -1 / ETIMEDOUT */
    {
        feed f = {.pre = "GET / HTTP/1.1\r\n", .hold_ms = 3000};
        fd = start_feed(&f, &pid);
        long long t0 = now_ms();
        errno = 0;
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 200, &hl);
        long long dt = now_ms() - t0;
        CHECK_INT(n, -1);
        CHECK_INT(errno, ETIMEDOUT);
        CHECK(dt >= 150 && dt < 1000);                        /* 대략 마감 시간에 맞춰 끝남 */
        end_feed(fd, pid);
    }

    /* slowloris: 1바이트씩 계속 보내도 "전체" 마감 시간은 연장되지 않음 */
    {
        feed f = {.pre = "GET / HTTP/1.1\r\n", .later = "X-Slow: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                  .delay_ms = 20, .drip = 1, .hold_ms = 3000};
        fd = start_feed(&f, &pid);
        long long t0 = now_ms();
        errno = 0;
        n = tcp_recv_request(fd, buf, sizeof(buf), 0, 300, &hl);
        long long dt = now_ms() - t0;
        CHECK_INT(n, -1);
        CHECK_INT(errno, ETIMEDOUT);
        CHECK(dt < 800);                                      /* 바이트가 계속 와도 300ms 근처에서 끊음 */
        end_feed(fd, pid);
    }

    /* cap 0 → -1 */
    CHECK_INT(tcp_recv_request(-1, buf, 0, 0, 100, &hl), -1);

    /* ==== tcp_recv_request: 이미 받은 바이트(파이프라이닝 잔여) 이어받기 ==== */

    /* 잔여 안에 헤더가 통째로 있음 → 상대가 아무것도 안 보내고 버텨도 즉시 반환 (뒤의 꼬리도 그대로) */
    {
        feed f = {.hold_ms = 2000};                           /* 아무것도 안 보내고 연결만 유지 */
        fd = start_feed(&f, &pid);
        memcpy(buf, REQ, REQ_LEN);                            /* 잔여 = 요청 하나 + 다음 요청 앞부분 "GE" */
        memcpy(buf + REQ_LEN, "GE", 2);
        long long t0 = now_ms();
        n = tcp_recv_request(fd, buf, sizeof(buf), REQ_LEN + 2, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN + 2);                 /* 받은 것 없음: 잔여 그대로 */
        CHECK_INT(hl, REQ_LEN);
        CHECK_INT(buf[n], '\0');
        CHECK(now_ms() - t0 < 100);                           /* 기다리지 않음 */
        end_feed(fd, pid);
    }

    /* 잔여에 '\0' 이 섞여 있어도 그 뒤의 헤더 끝을 찾는다 (strstr 이면 멈춤) */
    {
        feed f = {.hold_ms = 2000};
        fd = start_feed(&f, &pid);
        memcpy(buf, "X\0Y\r\n\r\n", 7);
        n = tcp_recv_request(fd, buf, sizeof(buf), 7, 1000, &hl);
        CHECK_INT(n, 7);
        CHECK_INT(hl, 7);
        end_feed(fd, pid);
    }

    /* 잔여는 헤더 일부뿐 → 나머지를 받아 정확한 header_len */
    {
        feed f = {.later = "Host: a\r\n\r\n", .delay_ms = 50};
        fd = start_feed(&f, &pid);
        memcpy(buf, "GET / HTTP/1.1\r\n", 16);
        n = tcp_recv_request(fd, buf, sizeof(buf), 16, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN);
        CHECK_INT(hl, REQ_LEN);
        CHECK(memcmp(buf, REQ, REQ_LEN) == 0);                /* 잔여 뒤에 이어 붙음 */
        end_feed(fd, pid);
    }

    /* 잔여가 헤더 경계에 걸침: 잔여 "...\r\n\r" + 새로 "\n" (겹침 검색 검증) */
    {
        feed f = {.later = "\nBODY", .delay_ms = 50};
        fd = start_feed(&f, &pid);
        memcpy(buf, REQ, REQ_LEN - 1);                        /* 마지막 '\n' 만 빠짐 */
        n = tcp_recv_request(fd, buf, sizeof(buf), REQ_LEN - 1, 1000, &hl);
        CHECK_INT(n, (long long)REQ_LEN + 4);
        CHECK_INT(hl, REQ_LEN);
        end_feed(fd, pid);
    }

    /* 잔여만으로 꽉 참 (cap-1) + 헤더 끝 없음 → 수신 없이 header_len 0 (호출자가 431) */
    {
        feed f = {.hold_ms = 2000};
        fd = start_feed(&f, &pid);
        memset(buf, 'a', 15);
        long long t0 = now_ms();
        n = tcp_recv_request(fd, buf, 16, 15, 1000, &hl);
        CHECK_INT(n, 15);
        CHECK_INT(hl, 0);
        CHECK_INT(buf[15], '\0');
        CHECK(now_ms() - t0 < 100);
        end_feed(fd, pid);
    }

    /* 잔여가 cap-1 을 넘으면 잘못된 인자 */
    errno = 0;
    CHECK_INT(tcp_recv_request(-1, buf, 16, 16, 100, &hl), -1);
    CHECK_INT(errno, EINVAL);

    /* 잔여 일부 + 상대가 닫음 → 잔여 길이 반환, header_len 0 (호출자가 400) */
    {
        feed f = {0};
        fd = start_feed(&f, &pid);
        memcpy(buf, "GET /", 5);
        n = tcp_recv_request(fd, buf, sizeof(buf), 5, 1000, &hl);
        CHECK_INT(n, 5);
        CHECK_INT(hl, 0);
        end_feed(fd, pid);
    }

    /* ==== tcp_recv_exact ==== */

    /* 나눠서 와도 정확히 len 바이트 */
    {
        feed f = {.pre = "abc", .later = "defg", .delay_ms = 30};
        fd = start_feed(&f, &pid);
        memset(buf, 0, sizeof(buf));
        CHECK_INT(tcp_recv_exact(fd, buf, 7, 1000), 0);
        CHECK(memcmp(buf, "abcdefg", 7) == 0);
        end_feed(fd, pid);
    }

    /* 요청한 만큼만 읽고 뒤(다음 요청)는 남겨 둠 */
    {
        feed f = {.pre = "abcdefXYZ", .hold_ms = 2000};
        fd = start_feed(&f, &pid);
        CHECK_INT(tcp_recv_exact(fd, buf, 6, 1000), 0);
        CHECK(memcmp(buf, "abcdef", 6) == 0);
        CHECK_INT(tcp_recv_exact(fd, buf, 3, 1000), 0);       /* 남은 3바이트가 그대로 있음 */
        CHECK(memcmp(buf, "XYZ", 3) == 0);
        end_feed(fd, pid);
    }

    /* 다 받기 전에 닫힘 → -1 / ECONNRESET */
    {
        feed f = {.pre = "abc"};
        fd = start_feed(&f, &pid);
        errno = 0;
        CHECK_INT(tcp_recv_exact(fd, buf, 10, 1000), -1);
        CHECK_INT(errno, ECONNRESET);
        end_feed(fd, pid);
    }

    /* 멈춤 → -1 / ETIMEDOUT */
    {
        feed f = {.pre = "abc", .hold_ms = 3000};
        fd = start_feed(&f, &pid);
        errno = 0;
        CHECK_INT(tcp_recv_exact(fd, buf, 10, 200), -1);
        CHECK_INT(errno, ETIMEDOUT);
        end_feed(fd, pid);
    }

    /* ==== tcp_close_graceful (진짜 TCP: RST 는 TCP 에서만 생김) ==== */
    {
        int lfd = tcp_listen(0, 8);                           /* 포트 0 = 커널이 빈 포트를 골라 줌 */
        CHECK(lfd >= 0);
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        getsockname(lfd, (struct sockaddr *)&a, &alen);       /* 실제로 받은 포트 */
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

        for (int unread = 0; unread <= 1; unread++) {         /* 0: 남은 데이터 없음, 1: 읽지 않은 데이터 32KB */
            int cfd = socket(AF_INET, SOCK_STREAM, 0);         /* 클라이언트 */
            struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); /* 테스트가 멈추지 않게 */
            CHECK_INT(connect(cfd, (struct sockaddr *)&a, sizeof(a)), 0);
            int sfd = tcp_accept(lfd, NULL, 0);               /* 서버 쪽 */
            CHECK(sfd >= 0);

            size_t extra = unread ? 32 * 1024 : 0;            /* 서버가 안 읽고 남길 양 */
            char *msg = malloc(3 + extra);
            memcpy(msg, "REQ", 3);
            memset(msg + 3, 'x', extra);
            CHECK_INT(write_all(cfd, msg, 3 + extra), 0);
            free(msg);
            sleep_ms(50);                                     /* 루프백이라도 도착할 시간 */

            CHECK_INT(tcp_recv_exact(sfd, buf, 3, 1000), 0);  /* 서버는 "REQ" 만 읽음 */
            CHECK_INT(tcp_send_all(sfd, "RESP", 4, 1000), 0); /* 응답 */
            long long t0 = now_ms();
            tcp_close_graceful(sfd);                          /* 남은 32KB 를 비우고 닫아야 RST 가 안 나감 */
            long long dt = now_ms() - t0;
            if (unread)
                CHECK(dt < 900);                              /* drain 상한 0.5s 근처 (클라가 FIN 을 안 보내도 붙잡히지 않음) */
            else
                CHECK(dt < 100);                              /* 남은 게 없으면 즉시 닫음 */

            /* 클라이언트: 응답 전부 + 정상 EOF (ECONNRESET 아님) */
            char rb[16];
            size_t got = 0;
            ssize_t r;
            while ((r = read(cfd, rb + got, sizeof(rb) - got)) > 0)
                got += (size_t)r;
            CHECK_INT(r, 0);                                  /* -1(ECONNRESET) 이 아니라 0(FIN) */
            CHECK(got == 4 && memcmp(rb, "RESP", 4) == 0);
            close(cfd);
        }
        close(lfd);
    }

    return TEST_REPORT();
}
