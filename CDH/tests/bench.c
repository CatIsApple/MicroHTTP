/* bench.c — 간단한 HTTP 부하 생성기 (wrk / hey 와 같은 방식, 외부 도구 없이 POSIX 소켓 + pthread 만).
 *
 * 스레드 c 개가 d 초 동안 각자 "GET 보내기 → 응답을 Content-Length 만큼 받기" 를 반복한다.
 *   기본: 요청마다 "Connection: close" → 연결 → 요청 → 닫기 (요청 하나 = 연결 하나)
 *   -k  : keep-alive 로 한 연결을 계속 재사용 (서버가 close 로 답하면 그때만 다시 연결)
 *
 * 사용: bench [-k] [-c 동시연결] [-d 초] [-p 포트] [-h IPv4] 경로
 *   예) bench -k -c 50 -d 5 -p 8080 /index.html
 * 출력 (한 줄, 표로 모으기 쉽게):
 *   path conc req/s MB/s avg_ms p50_ms p99_ms max_ms ok fail
 * 실패 = 연결 실패, 200 이 아닌 상태, Content-Length 와 받은 본문 길이가 다름. */

#define _POSIX_C_SOURCE 200809L /* clock_gettime, getopt */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* macOS: MSG_NOSIGNAL 이 없으면 아래에서 0 으로 */
#endif

#include <arpa/inet.h>  /* inet_pton, htons */
#include <netinet/in.h> /* struct sockaddr_in */
#include <pthread.h>    /* pthread_create, pthread_join */
#include <stdio.h>      /* printf, fprintf, snprintf */
#include <stdlib.h>     /* atoi, malloc, free, qsort */
#include <string.h>     /* strstr, strlen, memset */
#include <sys/socket.h> /* socket, connect, send, recv */
#include <time.h>       /* clock_gettime */
#include <unistd.h>     /* close, getopt */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: 플래그 없음 (서버가 끊으면 SIGPIPE 가능 — 벤치용이라 감수) */
#endif

#define MAX_SAMPLES (1 << 18) /* 스레드당 지연 시간 기록 상한 (넘으면 기록만 멈추고 계속 셈) */

typedef struct {             /* 부하 스레드 하나의 입력·출력 (스레드끼리 공유 안 함 → 락 불필요) */
    /* 입력 */
    struct sockaddr_in addr; /* 서버 주소 (IPv4 + 포트) */
    const char *req;         /* 보낼 요청 문자열 (모든 스레드가 같은 것을 읽기만) */
    size_t req_len;          /* 요청 길이 */
    double end;              /* 끝낼 시각 (초) */
    /* 출력 */
    long ok, fail;           /* 성공·실패 요청 수 */
    unsigned long long bytes; /* 받은 총 바이트 (머리 + 본문) */
    double *lat;             /* 성공한 요청의 지연 시간 (ms) */
    long nlat;               /* lat 에 기록한 개수 */
} worker;                                                   /* 스레드마다 하나 */

static double now_s(void) /* 단조 시계 현재 시각 (초, 소수) */
{
    struct timespec ts;                         /* 초 + 나노초 */
    clock_gettime(CLOCK_MONOTONIC, &ts);        /* 시스템 시간 변경에 영향 없는 시계 */
    return ts.tv_sec + ts.tv_nsec / 1e9;        /* 초 단위 실수로 */
}

/* 요청 하나: 성공이면 받은 전체 바이트 수, 실패면 -1.
 * *fd 가 -1 이면 새로 연결. 응답은 Content-Length 만큼만 읽는다 (EOF 를 기다리지 않음 → keep-alive 에서도 동작).
 * 서버가 Connection: close 로 답했거나 keep 이 0 이거나 실패하면 연결을 닫고 *fd = -1. */
static long long one_request(const worker *w, int *fd, int keep) /* keep: 1 이면 응답 뒤에도 연결 유지 */
{
    if (*fd < 0) {                                      /* 연결이 없으면 (첫 요청, close 모드, 서버가 닫은 뒤) */
        *fd = socket(AF_INET, SOCK_STREAM, 0);          /* 새 TCP 소켓 */
        if (*fd < 0)                                        /* 소켓 생성 실패 */
            return -1;                                  /* fd 고갈 등 */
        if (connect(*fd, (const struct sockaddr *)&w->addr, sizeof(w->addr)) < 0) /* 3-way handshake */
            goto fail;                                  /* 연결 거부 (서버 backlog 가득 등) */
    }
    size_t off = 0;                                     /* 보낸 바이트 수 */
    while (off < w->req_len) {                          /* 요청 전체를 보낼 때까지 (send 는 일부만 보낼 수 있음) */
        ssize_t n = send(*fd, w->req + off, w->req_len - off, MSG_NOSIGNAL); /* NOSIGNAL: 서버가 끊었어도 SIGPIPE 로 죽지 않게 */
        if (n <= 0)                                         /* 보내기 실패 */
            goto fail;                                  /* 서버가 keep-alive 연결을 닫은 뒤 보낸 경우 등 */
        off += (size_t)n;                               /* 보낸 만큼 전진 */
    }

    /* 머리는 앞 4KB 안에 있다고 보고, 빈 줄이 보일 때까지 받는다 (본문 일부가 같이 올 수 있음) */
    char head[4096];                                    /* 응답 머리 (+ 같이 온 본문 앞부분) */
    size_t hl = 0;                                      /* head 에 받은 바이트 수 */
    char *end = NULL;                                   /* 빈 줄("\r\n\r\n") 위치 */
    while (end == NULL) {                               /* 머리 끝을 찾을 때까지 */
        if (hl >= sizeof(head) - 1)                         /* 버퍼가 다 찼는데 빈 줄이 없음 */
            goto fail;                                  /* 머리가 4KB 를 넘음 → 이 벤치에서는 실패로 침 */
        ssize_t n = recv(*fd, head + hl, sizeof(head) - 1 - hl, 0); /* 남은 자리만큼 받기 ('\0' 자리 남김) */
        if (n <= 0)                                         /* 0 = 상대가 닫음, -1 = 오류 */
            goto fail;                                  /* 응답 전에 닫힘 (keep-alive 연결이 서버 쪽에서 닫힌 경우 포함) */
        hl += (size_t)n;                                /* 받은 양 누적 */
        head[hl] = '\0';                                /* strstr 을 쓰려고 문자열로 */
        end = strstr(head, "\r\n\r\n");                 /* 머리 끝 찾기 */
    }
    if (strncmp(head, "HTTP/1.1 200 ", 13) != 0)        /* 200 이 아니면 */
        goto fail;                                      /* 실패로 셈 */
    const char *cl = strstr(head, "\r\nContent-Length: "); /* 본문 길이 헤더 */
    if (cl == NULL || cl > end)                         /* 없거나 머리 밖(본문 안)에서 찾은 것 */
        goto fail;                                          /* Content-Length 없이는 응답 끝을 알 수 없음 */
    long long want = atoll(cl + 18);                    /* "\r\nContent-Length: " 18글자 뒤의 숫자 */
    long long hdr = (long long)(end + 4 - head);        /* 머리 길이 (빈 줄 포함) */
    long long have = (long long)hl - hdr;               /* 머리와 함께 온 본문 */
    if (have > want)                                    /* 요청 하나만 보냈는데 더 옴 */
        goto fail;                                      /* 서버가 본문을 더 보냄 = 틀 오류 */
    char buf[64 * 1024];                                /* 본문은 저장하지 않고 세기만 → 버릴 버퍼 */
    while (have < want) {                               /* 본문 나머지: 정확히 want 까지만 */
        long long left = want - have;                   /* 남은 본문 */
        ssize_t n = recv(*fd, buf, left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf), 0); /* 남은 만큼만 (다음 응답을 먹지 않게) */
        if (n <= 0)                                         /* 0 = 상대가 닫음, -1 = 오류 */
            goto fail;                                  /* 본문 도중 끊김 */
        have += n;                                      /* 받은 양 누적 */
    }
    const char *conn = strstr(head, "\r\nConnection: close"); /* 서버가 닫겠다고 했는지 */
    if (!keep || (conn != NULL && conn < end)) {        /* 이번으로 끝 */
        close(*fd);                                     /* 우리도 닫음 */
        *fd = -1;                                       /* 다음 요청은 새 연결 */
    }
    return hdr + want;                                  /* 받은 총 바이트 */

fail:                                                   /* 실패: 연결을 버리고 다음 요청은 새 연결로 */
    close(*fd);                                             /* 연결 닫기 */
    *fd = -1;
    return -1;                                              /* 실패 */
}

static int g_keep = 0; /* -k: keep-alive 로 연결 재사용 (main 에서 스레드 만들기 전에만 씀 → 이후 읽기만) */

static void *run(void *arg) /* 부하 스레드 본체: 끝 시각까지 요청 반복 */
{
    worker *w = arg;                                    /* 이 스레드 몫 */
    int fd = -1;                                        /* keep-alive 면 요청 사이에 유지 */
    while (now_s() < w->end) {                          /* 측정 시간 동안 */
        double t0 = now_s();                            /* 요청 시작 시각 */
        long long got = one_request(w, &fd, g_keep);    /* 요청 하나 (필요하면 연결부터) */
        double ms = (now_s() - t0) * 1000.0;            /* 걸린 시간 (ms). 새 연결이면 connect 포함 */
        if (got < 0) {                                  /* 실패 */
            w->fail++;                                      /* 실패 수 */
            continue;                                   /* 실패는 지연 시간에 넣지 않음 */
        }
        w->ok++;                                        /* 성공 수 */
        w->bytes += (unsigned long long)got;            /* 받은 바이트 */
        if (w->nlat < MAX_SAMPLES)                      /* 기록 상한 전이면 */
            w->lat[w->nlat++] = ms;                     /* 지연 시간 기록 (백분위 계산용) */
    }
    if (fd >= 0)                                            /* 연결이 남아 있으면 */
        close(fd);                                      /* 남은 keep-alive 연결 닫기 */
    return NULL;                                            /* 스레드 끝 (결과는 w 에 들어 있음) */
}

static int cmp_double(const void *a, const void *b) /* qsort 비교 함수: 오름차순 */
{
    double x = *(const double *)a, y = *(const double *)b; /* void* → double */
    return (x > y) - (x < y);                           /* x>y → 1, x<y → -1, 같으면 0 (뺄셈은 넘침 위험이라 안 씀) */
}

int main(int argc, char *argv[])                            /* 인자 처리 → 스레드 c 개 실행 → 결과 합쳐 한 줄 출력 */
{
    int conc = 10, secs = 5, port = 8080;               /* 기본값: 동시 연결 10, 5초, 8080 */
    const char *host = "127.0.0.1";                     /* 기본 대상: 이 PC */
    int opt;                                            /* getopt 가 돌려준 옵션 글자 */
    while ((opt = getopt(argc, argv, "c:d:p:h:k")) != -1) { /* "c:" = 값이 있는 옵션, "k" = 값 없는 옵션 */
        switch (opt) {                                      /* 옵션 글자별 처리 */
        case 'k': g_keep = 1; break;                    /* keep-alive 모드 */
        case 'c': conc = atoi(optarg); break;           /* 동시 연결(스레드) 수 */
        case 'd': secs = atoi(optarg); break;           /* 측정 시간 (초) */
        case 'p': port = atoi(optarg); break;           /* 포트 */
        case 'h': host = optarg; break;                 /* IPv4 주소 */
        default: goto usage;                            /* 모르는 옵션 */
        }
    }
    if (optind != argc - 1 || conc < 1 || secs < 1 || port < 1 || port > 65535) /* 경로 인자가 정확히 하나 + 값 범위 */
        goto usage;                                         /* 사용법 출력 후 종료 */
    const char *path = argv[optind];                    /* 요청할 경로 (예: /index.html) */

    char req[1024];                                     /* 요청 문자열 (모든 스레드가 같은 것을 보냄) */
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: microhttp-bench\r\n%s\r\n", path, host, /* GET 요청 한 개 (빈 줄로 끝) */
                      g_keep ? "" : "Connection: close\r\n"); /* -k 아니면 요청마다 새 연결 */
    if (rl < 0 || (size_t)rl >= sizeof(req))            /* 경로가 너무 길어 잘림 */
        goto usage;                                         /* 사용법 출력 후 종료 */

    worker *ws = calloc((size_t)conc, sizeof(worker));  /* 스레드별 입력·출력 (0 으로 초기화) */
    pthread_t *ts = calloc((size_t)conc, sizeof(pthread_t)); /* 스레드 핸들 */
    double start = now_s();                             /* 측정 시작 */
    for (int i = 0; i < conc; i++) {                    /* 스레드마다 */
        ws[i].addr.sin_family = AF_INET;                /* IPv4 */
        ws[i].addr.sin_port = htons((uint16_t)port);    /* 포트 (네트워크 바이트 순서) */
        if (inet_pton(AF_INET, host, &ws[i].addr.sin_addr) != 1) { /* "127.0.0.1" → 4바이트 주소 */
            fprintf(stderr, "bad host %s\n", host);         /* IPv4 문자열이 아님 */
            return 1;                                       /* 종료 코드 1 */
        }
        ws[i].req = req;                                /* 공용 요청 문자열 */
        ws[i].req_len = (size_t)rl;                         /* 요청 길이 */
        ws[i].end = start + secs;                       /* 모두 같은 시각에 끝냄 */
        ws[i].lat = malloc(MAX_SAMPLES * sizeof(double)); /* 지연 시간 기록 공간 */
        pthread_create(&ts[i], NULL, run, &ws[i]);      /* 부하 스레드 시작 */
    }

    long ok = 0, fail = 0, n = 0;                       /* 합계: 성공, 실패, 지연 시간 표본 수 */
    unsigned long long bytes = 0;                       /* 합계: 받은 바이트 */
    for (int i = 0; i < conc; i++) {                    /* 모든 스레드가 끝나길 기다리며 합침 */
        pthread_join(ts[i], NULL);                          /* 이 스레드가 끝날 때까지 기다림 */
        ok += ws[i].ok;                                     /* 성공 합계 */
        fail += ws[i].fail;                                 /* 실패 합계 */
        bytes += ws[i].bytes;                               /* 바이트 합계 */
        n += ws[i].nlat;                                    /* 지연 시간 표본 수 합계 */
    }
    double elapsed = now_s() - start;                   /* 실제 걸린 시간 (마지막 요청이 끝난 시각까지) */

    double *all = malloc((size_t)(n > 0 ? n : 1) * sizeof(double)); /* 모든 스레드의 지연 시간 → 백분위 */
    long k = 0;                                         /* all 에 채운 개수 */
    double sum = 0;                                     /* 평균용 합 */
    for (int i = 0; i < conc; i++)                      /* 스레드별 기록을 한 배열로 모음 */
        for (long j = 0; j < ws[i].nlat; j++) {             /* 이 스레드의 기록 전부 */
            all[k++] = ws[i].lat[j];                        /* 한 배열로 복사 */
            sum += ws[i].lat[j];                            /* 평균 계산용 합 */
        }
    qsort(all, (size_t)n, sizeof(double), cmp_double);  /* 정렬해야 백분위를 바로 꺼낼 수 있음 */
    double avg = n ? sum / n : 0, p50 = n ? all[n / 2] : 0, p99 = n ? all[(long)(n * 0.99)] : 0, mx = n ? all[n - 1] : 0; /* 평균, 중앙값, 99%, 최대 */

    printf("%-14s %5d %10.0f %9.1f %8.2f %8.2f %8.2f %8.2f %8ld %6ld\n", path, conc, ok / elapsed, /* 표 한 줄 (bench.sh 가 머리줄을 찍음) */
           bytes / elapsed / (1024.0 * 1024.0), avg, p50, p99, mx, ok, fail); /* MB/s = 초당 바이트 / 2^20 */

    for (int i = 0; i < conc; i++)                      /* 정리 */
        free(ws[i].lat);                                    /* 스레드별 지연 시간 기록 */
    free(all);                                              /* 합친 배열 */
    free(ws);                                               /* 스레드 입력·출력 */
    free(ts);                                               /* 스레드 핸들 */
    return fail > 0 ? 2 : 0;                            /* 실패 요청이 있으면 종료 코드 2 (bench.sh 가 봄) */

usage:                                                  /* 잘못된 인자 */
    fprintf(stderr, "usage: %s [-k] [-c conns] [-d secs] [-p port] [-h ipv4] path\n", argv[0]); /* 사용법 */
    return 1;                                               /* 종료 코드 1 */
}
