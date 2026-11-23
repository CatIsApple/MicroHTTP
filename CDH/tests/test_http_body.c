/* test_http_body.c — http_read_body 단위 테스트.
 * socketpair() 로 연결된 소켓 두 개를 만들고, fork() 한 자식이 "클라이언트" 역할로
 * 요청을 나눠 보낸다. 부모는 서버처럼 tcp_recv_request → http_parse_request → http_read_body. */

#define _POSIX_C_SOURCE 200809L /* socketpair, fork, kill, nanosleep 등 POSIX 함수 */

#include "http_body.h"  /* 테스트 대상 */
#include "tcp_server.h" /* tcp_recv_request */
#include "test.h"       /* CHECK, CHECK_INT, TEST_REPORT */
#include <errno.h>      /* errno, ETIMEDOUT, ECONNRESET */
#include <signal.h>     /* kill, SIGKILL */
#include <stdlib.h>     /* malloc, free, _Exit */
#include <string.h>     /* strlen, memcmp, memset, strcmp */
#include <sys/socket.h> /* socketpair, AF_UNIX, SOCK_STREAM */
#include <sys/wait.h>   /* waitpid */
#include <time.h>       /* nanosleep */
#include <unistd.h>     /* fork, read, write, close */

static void sleep_ms(long ms) /* 밀리초 단위로 잠들기 */
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
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

/* 한 케이스의 설정 */
typedef struct {
    const char *first;    /* 처음 보낼 바이트 (헤더 + 본문 일부) */
    const char *later;    /* 잠시 뒤(또는 100 Continue 를 받은 뒤) 보낼 바이트. NULL 이면 없음 */
    size_t later_len;     /* later 길이 (0 이면 strlen) */
    int wait_100;         /* 1 이면 later 를 보내기 전에 "100 Continue" 를 읽고 확인 */
    int stall;            /* 1 이면 다 보낸 뒤 닫지 않고 버팀 (타임아웃 유도) */
    size_t cap;           /* 서버 쪽 버퍼 크기 */
    int timeout_ms;       /* http_read_body 마감 시간 */
} scenario;

/* 결과 */
typedef struct {
    int rc;               /* http_read_body 반환 (파싱 실패면 그 코드) */
    int err;              /* rc == -1 일 때 errno */
    size_t body_len;      /* 받은 본문 길이 */
    int used_heap;        /* malloc 경로였는지 */
    int child_ok;         /* 자식이 100 Continue 확인 등에 성공했는지 */
    char body[64];        /* 본문 앞부분 (비교용) */
} outcome;

static outcome run(const scenario *s)
{
    outcome o;
    memset(&o, 0, sizeof(o));
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { /* sv[0] = 서버 쪽, sv[1] = 클라이언트 쪽 */
        o.rc = -999;
        return o;
    }
    pid_t pid = fork();
    if (pid == 0) { /* ---- 자식: 클라이언트 ---- */
        close(sv[0]);
        if (write_all(sv[1], s->first, strlen(s->first)) < 0)
            _Exit(1);
        if (s->wait_100) { /* 서버가 100 Continue 를 보낼 때까지 기다렸다가 확인 */
            char b[64];
            ssize_t n = read(sv[1], b, sizeof(b) - 1);
            b[n > 0 ? n : 0] = '\0';
            if (strcmp(b, "HTTP/1.1 100 Continue\r\n\r\n") != 0)
                _Exit(2);
        } else {
            sleep_ms(30); /* 서버가 첫 묶음만 먼저 받도록 잠깐 쉼 */
        }
        if (s->later) {
            size_t len = s->later_len ? s->later_len : strlen(s->later);
            if (write_all(sv[1], s->later, len) < 0)
                _Exit(1);
        }
        if (s->stall)
            sleep_ms(3000); /* 닫지 않고 버팀 → 부모가 타임아웃 후 kill */
        close(sv[1]);
        _Exit(0);
    }
    /* ---- 부모: 서버 ---- */
    close(sv[1]);
    char *buf = malloc(s->cap);
    size_t header_len = 0;
    ssize_t n = tcp_recv_request(sv[0], buf, s->cap, 1000, &header_len);
    http_request req;
    o.rc = (n > 0 && header_len > 0) ? http_parse_request(buf, header_len, &req) : -998;
    char *heap = NULL;
    if (o.rc == 0) {
        o.rc = http_read_body(sv[0], &req, buf, s->cap, (size_t)n, header_len, s->timeout_ms, &heap);
        o.err = errno;
        o.body_len = req.body_len;
        o.used_heap = heap != NULL;
        if (o.rc == 0 && req.body_len > 0) {
            size_t k = req.body_len < sizeof(o.body) - 1 ? req.body_len : sizeof(o.body) - 1;
            memcpy(o.body, req.body, k);
        }
    }
    free(heap);
    free(buf);
    close(sv[0]);
    kill(pid, SIGKILL); /* 버티는 자식 정리 (이미 끝났으면 무해) */
    int status = 0;
    waitpid(pid, &status, 0);
    o.child_ok = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return o;
}

#define BASE {NULL, NULL, 0, 0, 0, 8192, 1000} /* scenario 기본값 */

int main(void)
{
    outcome o;

    { /* 본문 없는 GET */
        scenario s = BASE; s.first = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_INT(o.body_len, 0);
    }
    { /* 본문이 첫 recv 에 다 옴 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_INT(o.body_len, 5); CHECK_STR(o.body, "hello"); CHECK_INT(o.used_heap, 0);
    }
    { /* 본문이 나중에 옴 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhel"; s.later = "lo world";
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_STR(o.body, "hello world"); CHECK_INT(o.used_heap, 0);
    }
    { /* Content-Length 뒤의 바이트(다음 요청)는 본문에 안 들어감 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nhiGET / HTTP/1.1\r\n";
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_INT(o.body_len, 2); CHECK_STR(o.body, "hi");
    }
    { /* Content-Length: 0 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n";
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_INT(o.body_len, 0);
    }
    { /* POST 인데 Content-Length 없음 → 411 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\n\r\nxx";
        CHECK_INT(run(&s).rc, 411);
    }
    { /* 1MB + 1 → 413 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1048577\r\n\r\n";
        CHECK_INT(run(&s).rc, 413);
    }
    { /* 모르는 Expect → 417 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nExpect: foo\r\nContent-Length: 3\r\n\r\n";
        CHECK_INT(run(&s).rc, 417);
    }
    { /* HTTP/1.0 의 Expect 는 무시 */
        scenario s = BASE; s.first = "POST / HTTP/1.0\r\nExpect: foo\r\nContent-Length: 3\r\n\r\nabc";
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_STR(o.body, "abc");
    }
    { /* Expect: 100-continue → 서버가 100 을 보낸 뒤에야 본문이 옴 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-Continue\r\nContent-Length: 3\r\n\r\n";
        s.later = "abc"; s.wait_100 = 1;
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_STR(o.body, "abc"); CHECK(o.child_ok);
    }
    { /* 413 이면 100 Continue 를 보내지 않음 (자식이 100 을 기다리면 실패해야 정상이므로 wait 안 함) */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 2000000\r\n\r\n";
        CHECK_INT(run(&s).rc, 413);
    }
    { /* 작은 버퍼 → 힙 경로 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 20\r\n\r\n0123";
        s.later = "456789abcdefghij"; s.cap = 64;
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_STR(o.body, "0123456789abcdefghij"); CHECK_INT(o.used_heap, 1);
    }
    { /* 본문 안에 NUL 바이트가 있어도 길이대로 받음 */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n\r\n";
        s.later = "a\0b\0"; s.later_len = 4;
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_INT(o.body_len, 4); CHECK(memcmp(o.body, "a\0b\0", 4) == 0);
    }
    { /* 다 받기 전에 끊김 → -1, ECONNRESET */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc";
        o = run(&s);
        CHECK_INT(o.rc, -1); CHECK_INT(o.err, ECONNRESET);
    }
    { /* 보내다 멈춤 → -1, ETIMEDOUT */
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nabc";
        s.stall = 1; s.timeout_ms = 200;
        o = run(&s);
        CHECK_INT(o.rc, -1); CHECK_INT(o.err, ETIMEDOUT);
    }
    { /* 정확히 1MB → 힙 경로로 전부 받음 */
        size_t L = HTTP_MAX_BODY_SIZE;
        char *big = malloc(L + 1);
        memset(big, 'z', L);
        big[L] = '\0';
        scenario s = BASE; s.first = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1048576\r\n\r\n";
        s.later = big; s.later_len = L; s.timeout_ms = 5000;
        o = run(&s);
        CHECK_INT(o.rc, 0); CHECK_INT(o.body_len, L); CHECK_INT(o.used_heap, 1);
        free(big);
    }

    return TEST_REPORT();
}
