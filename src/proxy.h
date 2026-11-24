/*
 * proxy.h — Reverse Proxy: 경로 기반 라우팅 + Round-Robin + 백엔드 전달
 *
 *   클라이언트 → [MicroHTTP] → 백엔드 1..N
 *
 * 실행 예: ./server -R /api/=127.0.0.1:9001,127.0.0.1:9002
 *   "/api/"로 시작하는 요청은 9001, 9002에 번갈아 전달하고, 나머지는 정적 파일로 응답합니다.
 */
#ifndef MICROHTTP_PROXY_H
#define MICROHTTP_PROXY_H

#include "http.h"

#include <pthread.h>
#include <stddef.h>

enum { PROXY_MAX_ROUTES = 8, PROXY_MAX_BACKENDS = 8 };

typedef struct {
    char host[256];  /* "127.0.0.1" 또는 "localhost" */
    char port[6];    /* "9001" */
} Backend;

/* 라우팅 규칙 하나: prefix로 시작하는 요청 → backends 중 하나 */
typedef struct {
    char prefix[128];
    Backend backends[PROXY_MAX_BACKENDS];
    size_t backend_count;
    size_t next_backend; /* Round-Robin으로 다음에 쓸 백엔드 순번 (table->lock으로 보호) */
} ProxyRoute;

typedef struct {
    ProxyRoute routes[PROXY_MAX_ROUTES];
    size_t route_count;
    pthread_mutex_t lock; /* 여러 작업 스레드가 next_backend를 동시에 바꾸지 않도록 */
} ProxyTable;

/* "/api/=127.0.0.1:9001,127.0.0.1:9002" 형식의 규칙을 추가합니다. 성공 0, 형식 오류 -1. */
int proxy_add_route(ProxyTable *table, const char *rule);

/* target의 경로가 가장 길게 일치하는 규칙. 없으면 NULL (→ 정적 파일). */
ProxyRoute *proxy_find_route(ProxyTable *table, const char *target);

/* Round-Robin: 이번 요청에 먼저 시도할 백엔드 순번을 돌려주고 순번을 하나 넘깁니다. */
size_t proxy_next_backend(ProxyTable *table, ProxyRoute *route);

/*
 * 백엔드로 보낼 요청 헤더를 만듭니다 (본문은 별도로 그대로 전달).
 *   - 요청 줄은 "METHOD target HTTP/1.1"
 *   - 클라이언트 헤더는 그대로 복사하되 hop-by-hop 헤더(Connection, Keep-Alive 등)는 뺍니다.
 *   - Host가 없던 요청(HTTP/1.0)에는 백엔드 주소로 Host를 붙입니다.
 *   - "Connection: close"를 붙여 백엔드가 응답 후 연결을 닫게 합니다(응답 끝 = EOF).
 * 반환값: 만든 바이트 수, 공간이 모자라면 -1.
 */
int proxy_build_request(const HttpRequest *request, const Backend *backend,
                        char *output, size_t capacity);

/*
 * 요청을 백엔드로 전달하고 응답을 클라이언트에게 그대로 중계합니다.
 * 백엔드에 연결할 수 없으면 502, 응답이 시간 안에 오지 않으면 504를 클라이언트에게 보냅니다.
 * 반환값: 클라이언트에게 보낸 상태 코드(알 수 없으면 0), 클라이언트 송신 실패면 -1.
 * 중계가 끝나면 클라이언트 연결은 닫아야 합니다(백엔드 응답의 끝을 EOF로 판단하므로).
 */
int proxy_forward(int client_fd, const HttpRequest *request, ProxyTable *table, ProxyRoute *route);

#endif
