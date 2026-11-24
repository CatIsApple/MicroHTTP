/* bench.c — 간단한 HTTP 부하 생성기 (wrk / hey 와 같은 방식, 외부 도구 없이 POSIX 소켓 + pthread 만).
 *
 * 스레드 c 개가 d 초 동안 각자 "연결 → GET 보내기 → 응답을 EOF 까지 받기 → 닫기" 를 반복한다.
 * 서버가 지금은 응답마다 Connection: close 라서 요청 하나 = 연결 하나 (keep-alive 는 동시성 단계).
 *
 * 사용: bench [-c 동시연결] [-d 초] [-p 포트] [-h IPv4] 경로
 *   예) bench -c 50 -d 5 -p 8080 /index.html
 * 출력 (한 줄, 표로 모으기 쉽게):
 *   path conc req/s MB/s avg_ms p50_ms p99_ms max_ms ok fail
 * 실패 = 연결 실패, 200 이 아닌 상태, Content-Length 와 받은 본문 길이가 다름. */

#define _POSIX_C_SOURCE 200809L /* clock_gettime, getopt */

#include <arpa/inet.h>  /* inet_pton, htons */
#include <netinet/in.h> /* struct sockaddr_in */
#include <pthread.h>    /* pthread_create, pthread_join */
#include <stdio.h>      /* printf, fprintf, snprintf */
#include <stdlib.h>     /* atoi, malloc, free, qsort */
#include <string.h>     /* strstr, strlen, memset */
#include <sys/socket.h> /* socket, connect, send, recv */
#include <time.h>       /* clock_gettime */
#include <unistd.h>     /* close, getopt */

#define MAX_SAMPLES (1 << 18) /* 스레드당 지연 시간 기록 상한 (넘으면 기록만 멈추고 계속 셈) */

typedef struct {
    /* 입력 */
    struct sockaddr_in addr;
    const char *req;
    size_t req_len;
    double end;              /* 끝낼 시각 (초) */
    /* 출력 */
    long ok, fail;
    unsigned long long bytes;
    double *lat;             /* 성공한 요청의 지연 시간 (ms) */
    long nlat;
} worker;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 요청 하나: 성공이면 받은 전체 바이트 수, 실패면 -1 */
static long long one_request(const worker *w)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (const struct sockaddr *)&w->addr, sizeof(w->addr)) < 0) {
        close(fd);
        return -1;
    }
    size_t off = 0;
    while (off < w->req_len) {
        ssize_t n = send(fd, w->req + off, w->req_len - off, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }

    /* 머리는 앞 4KB 안에 있다고 보고 거기서 상태·Content-Length 를 확인, 본문은 세기만 */
    char head[4096];
    size_t hl = 0;
    long long total = 0;
    char buf[64 * 1024];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;                                  /* 서버가 닫음 = 응답 끝 */
        if (hl < sizeof(head) - 1) {
            size_t c = (size_t)n < sizeof(head) - 1 - hl ? (size_t)n : sizeof(head) - 1 - hl;
            memcpy(head + hl, buf, c);
            hl += c;
        }
        total += n;
    }
    close(fd);
    head[hl] = '\0';

    if (strncmp(head, "HTTP/1.1 200 ", 13) != 0)
        return -1;
    const char *end = strstr(head, "\r\n\r\n");
    const char *cl = strstr(head, "\r\nContent-Length: ");
    if (end == NULL || cl == NULL || cl > end)
        return -1;
    long long want = atoll(cl + 18);
    if (total - (long long)(end + 4 - head) != want) /* 본문이 덜 오거나 더 옴 */
        return -1;
    return total;
}

static void *run(void *arg)
{
    worker *w = arg;
    while (now_s() < w->end) {
        double t0 = now_s();
        long long got = one_request(w);
        double ms = (now_s() - t0) * 1000.0;
        if (got < 0) {
            w->fail++;
            continue;
        }
        w->ok++;
        w->bytes += (unsigned long long)got;
        if (w->nlat < MAX_SAMPLES)
            w->lat[w->nlat++] = ms;
    }
    return NULL;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char *argv[])
{
    int conc = 10, secs = 5, port = 8080;
    const char *host = "127.0.0.1";
    int opt;
    while ((opt = getopt(argc, argv, "c:d:p:h:")) != -1) {
        switch (opt) {
        case 'c': conc = atoi(optarg); break;
        case 'd': secs = atoi(optarg); break;
        case 'p': port = atoi(optarg); break;
        case 'h': host = optarg; break;
        default: goto usage;
        }
    }
    if (optind != argc - 1 || conc < 1 || secs < 1 || port < 1 || port > 65535)
        goto usage;
    const char *path = argv[optind];

    char req[1024];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: microhttp-bench\r\n\r\n", path, host);
    if (rl < 0 || (size_t)rl >= sizeof(req))
        goto usage;

    worker *ws = calloc((size_t)conc, sizeof(worker));
    pthread_t *ts = calloc((size_t)conc, sizeof(pthread_t));
    double start = now_s();
    for (int i = 0; i < conc; i++) {
        ws[i].addr.sin_family = AF_INET;
        ws[i].addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &ws[i].addr.sin_addr) != 1) {
            fprintf(stderr, "bad host %s\n", host);
            return 1;
        }
        ws[i].req = req;
        ws[i].req_len = (size_t)rl;
        ws[i].end = start + secs;
        ws[i].lat = malloc(MAX_SAMPLES * sizeof(double));
        pthread_create(&ts[i], NULL, run, &ws[i]);
    }

    long ok = 0, fail = 0, n = 0;
    unsigned long long bytes = 0;
    for (int i = 0; i < conc; i++) {
        pthread_join(ts[i], NULL);
        ok += ws[i].ok;
        fail += ws[i].fail;
        bytes += ws[i].bytes;
        n += ws[i].nlat;
    }
    double elapsed = now_s() - start;

    double *all = malloc((size_t)(n > 0 ? n : 1) * sizeof(double)); /* 모든 스레드의 지연 시간 → 백분위 */
    long k = 0;
    double sum = 0;
    for (int i = 0; i < conc; i++)
        for (long j = 0; j < ws[i].nlat; j++) {
            all[k++] = ws[i].lat[j];
            sum += ws[i].lat[j];
        }
    qsort(all, (size_t)n, sizeof(double), cmp_double);
    double avg = n ? sum / n : 0, p50 = n ? all[n / 2] : 0, p99 = n ? all[(long)(n * 0.99)] : 0, mx = n ? all[n - 1] : 0;

    printf("%-14s %5d %10.0f %9.1f %8.2f %8.2f %8.2f %8.2f %8ld %6ld\n", path, conc, ok / elapsed,
           bytes / elapsed / (1024.0 * 1024.0), avg, p50, p99, mx, ok, fail);

    for (int i = 0; i < conc; i++)
        free(ws[i].lat);
    free(all);
    free(ws);
    free(ts);
    return fail > 0 ? 2 : 0;

usage:
    fprintf(stderr, "usage: %s [-c conns] [-d secs] [-p port] [-h ipv4] path\n", argv[0]);
    return 1;
}
