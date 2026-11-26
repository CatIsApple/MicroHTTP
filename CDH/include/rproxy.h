/* rproxy.h — Reverse Proxy 의 "목차"(선언부). 구현은 src/rproxy.c. 공통 부품은 proxy_core.h.
 *
 *   클라이언트 ──▶ microhttp ──(경로 prefix 로 라우트 선택)──▶ 백엔드 1..N (Round-Robin)
 *
 * 라우팅 규칙: "-P <prefix>=<host:port>[,<host:port>...]" (여러 번 줄 수 있음). 예)
 *   -P /api/=127.0.0.1:8001,127.0.0.1:8002   → /api/... 는 두 백엔드에 번갈아
 *   -P /static/=localhost:8003                → /static/... 는 8003
 *   매칭 안 되는 경로는 기존처럼 문서 루트의 정적 파일 / POST echo 로 처리한다.
 * 가장 긴 prefix 가 이긴다. URI 는 바꾸지 않고 그대로 전달한다 (nginx 의 "proxy_pass http://up;" 와 같음).
 * 목적지는 운영자가 정한다 (클라이언트가 고르는 것은 Forward Proxy).
 *
 * 백엔드 연결은 요청마다 새로 맺고 "Connection: close" 로 보낸다 (백엔드 keep-alive 풀 없음 → 단순·안전).
 * 클라이언트 쪽 keep-alive 는 응답 길이를 알 때(Content-Length / 본문 없음) 유지한다.
 * 접두사: rproxy_ */

#ifndef RPROXY_H /* 인클루드 가드 */
#define RPROXY_H

#include "http_parser.h" /* http_request */
#include <netinet/in.h>  /* struct sockaddr_in */
#include <stdatomic.h>   /* atomic_uint (라운드 로빈 카운터: 여러 워커가 동시에 증가) */
#include <stddef.h>      /* size_t */

#define RPROXY_MAX_ROUTES 32             /* 라우트(-P) 최대 개수 */
#define RPROXY_MAX_BACKENDS 16           /* 라우트 하나의 백엔드 최대 개수 */
#define RPROXY_PREFIX_MAX 256            /* prefix 최대 길이 ('\0' 포함) */
#define RPROXY_NAME_MAX 300              /* "host:port" 원문 최대 길이 ('\0' 포함) */
#define RPROXY_DEFAULT_TIMEOUT_MS 10000  /* 백엔드 응답 기다리는 시간 기본값 (-T 로 변경) */
#define RPROXY_DOWN_MS 5000              /* connect 에 실패한 백엔드를 건너뛰는 시간 (그 뒤 다시 시도) */

typedef struct {
    char name[RPROXY_NAME_MAX]; /* 규칙에 적힌 "host:port" 그대로 (로그, Host 헤더가 없는 요청의 Host 값) */
    struct sockaddr_in addr;    /* 시작할 때 한 번 해석해 둔 주소 (요청마다 DNS 조회 안 함) */
    atomic_llong down_until;    /* 이 시각(단조 시계 ms)까지는 죽은 것으로 보고 건너뜀. 0 = 정상 (여러 워커가 원자적으로 읽고 씀) */
} rproxy_backend;

typedef struct {
    char prefix[RPROXY_PREFIX_MAX];               /* 경로 prefix ("/api/") */
    size_t prefix_len;                            /* strlen(prefix) */
    rproxy_backend backends[RPROXY_MAX_BACKENDS]; /* 이 라우트의 백엔드들 */
    int nbackends;                                /* 실제 개수 (1 이상) */
    atomic_uint rr;                               /* 다음에 고를 백엔드 번호 (라운드 로빈, 락 없이 증가) */
} rproxy_route;

typedef struct {
    rproxy_route routes[RPROXY_MAX_ROUTES]; /* 라우트 목록 (등록 순서) */
    int nroutes;                            /* 실제 개수 */
    int timeout_ms;                         /* 백엔드 응답 타임아웃: 머리가 올 때까지 / 본문 조각 사이의 최대 무진척 시간 */
} rproxy_table;

/* 빈 표로 초기화 (timeout = RPROXY_DEFAULT_TIMEOUT_MS) */
void rproxy_init(rproxy_table *t);

/* 규칙 "prefix=host:port[,host:port...]" 를 해석해 표에 더한다 (host 는 getaddrinfo 로 IPv4 해석).
 * 반환: 0 성공, -1 실패 (*why 에 이유 문자열 — 정적 상수) */
int rproxy_add_route(rproxy_table *t, const char *spec, const char **why);

/* uri 에 맞는 라우트 (가장 긴 prefix). 없으면 NULL.
 * prefix 가 '/' 로 끝나면 단순 접두사, 아니면 경계까지 일치해야 함: "/api" 는 "/api", "/api/x", "/api?q" 에 맞고 "/apix" 에는 안 맞음.
 * 경로에 "." / ".." 세그먼트가 있으면 NULL (백엔드 정규화로 라우팅 경계를 넘는 것 방지) */
rproxy_route *rproxy_match(rproxy_table *t, const char *uri);

/* 요청 하나를 라우트의 백엔드로 전달하고 응답을 클라이언트(cfd)에 중계한다 (워커 스레드에서 호출).
 * 백엔드 선택: 라운드 로빈, connect 가 실패하면 다음 백엔드로 (모두 실패하면 502, 모두 타임아웃이면 504).
 * connect 에 실패한 백엔드는 RPROXY_DOWN_MS 동안 건너뛴다 (모든 요청이 죽은 백엔드의 connect 시간을 물지 않게).
 * 라우트의 백엔드가 전부 "죽음" 표시면 표시를 무시하고 다 시도한다 (되살아났는지 확인).
 * 나머지(*keep, *status, 반환값)는 pxy_exchange 와 같음: 0 = 다 중계, >0 = 이 코드로 오류 응답, -1 = 연결 끊기 */
int rproxy_handle(const rproxy_table *t, rproxy_route *rt, int cfd, const http_request *req,
                  const char *client_ip, int *keep, int *status, int client_timeout_ms);

#endif /* RPROXY_H */
