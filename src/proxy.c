/*
 * proxy.c — Reverse Proxy
 *
 * proxy_forward()의 흐름
 *   1) Round-Robin으로 백엔드를 고르고 연결 (실패하면 다음 백엔드, 모두 실패하면 502)
 *   2) 요청 헤더를 다시 만들어 보내고, 본문은 그대로 보냄
 *   3) 백엔드 응답을 받는 대로 클라이언트에게 중계 (첫 응답이 늦으면 504, 기다리는 중 클라이언트가 떠나면 중단)
 */
#include "proxy.h"
#include "net.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

enum {
    CONNECT_TIMEOUT_MS = 2000,    /* 백엔드 연결(3-way handshake) 대기 한도 */
    BACKEND_TIMEOUT_SECONDS = 5,  /* 백엔드가 이 시간 동안 아무것도 보내지 않으면 포기 */
    RELAY_CHUNK_SIZE = 16384
};

/* "host:port" 한 개를 backend에 채웁니다. 성공 0, 실패 -1. */
static int parse_backend(const char *text, size_t length, Backend *backend)
{
    const char *colon = NULL;
    for (size_t i = 0; i < length; ++i)
        if (text[i] == ':') colon = text + i;   /* 마지막 ':'로 나눔 */
    if (!colon) return -1;
    size_t host_length = (size_t)(colon - text);
    size_t port_length = length - host_length - 1;
    if (host_length == 0 || host_length >= sizeof(backend->host) ||
        port_length == 0 || port_length >= sizeof(backend->port)) return -1;
    long port = 0;
    for (size_t i = 0; i < port_length; ++i) {
        if (colon[1 + i] < '0' || colon[1 + i] > '9') return -1;
        port = port * 10 + (colon[1 + i] - '0');
    }
    if (port < 1 || port > 65535) return -1;
    memcpy(backend->host, text, host_length);
    backend->host[host_length] = '\0';
    memcpy(backend->port, colon + 1, port_length);
    backend->port[port_length] = '\0';
    return 0;
}

int proxy_add_route(ProxyTable *table, const char *rule)
{
    if (table->route_count == PROXY_MAX_ROUTES) return -1;
    const char *equals = strchr(rule, '=');
    if (!equals || rule[0] != '/') return -1;
    ProxyRoute route;
    memset(&route, 0, sizeof(route));
    size_t prefix_length = (size_t)(equals - rule);
    if (prefix_length >= sizeof(route.prefix)) return -1;
    memcpy(route.prefix, rule, prefix_length);
    route.prefix[prefix_length] = '\0';

    /* "=" 뒤의 "host:port,host:port,..."를 하나씩 읽습니다. */
    const char *item = equals + 1;
    for (;;) {
        size_t length = strcspn(item, ",");
        if (route.backend_count == PROXY_MAX_BACKENDS ||
            parse_backend(item, length, &route.backends[route.backend_count]) == -1) return -1;
        ++route.backend_count;
        if (item[length] == '\0') break;
        item += length + 1;
    }
    table->routes[table->route_count++] = route;
    return 0;
}

ProxyRoute *proxy_find_route(ProxyTable *table, const char *target)
{
    /* "/api/users"는 "/api/"와 "/"가 모두 맞을 수 있으므로 가장 긴(구체적인) 규칙을 고릅니다. */
    size_t path_length = strcspn(target, "?");
    ProxyRoute *best = NULL;
    size_t best_length = 0;
    for (size_t i = 0; i < table->route_count; ++i) {
        size_t length = strlen(table->routes[i].prefix);
        if (length <= path_length && strncmp(target, table->routes[i].prefix, length) == 0 &&
            length > best_length) {
            best = &table->routes[i];
            best_length = length;
        }
    }
    return best;
}

size_t proxy_next_backend(ProxyTable *table, ProxyRoute *route)
{
    /* 두 스레드가 동시에 읽고 증가시키면 같은 순번을 받을 수 있으므로 잠급니다. */
    pthread_mutex_lock(&table->lock);
    size_t index = route->next_backend % route->backend_count;
    route->next_backend = index + 1;
    pthread_mutex_unlock(&table->lock);
    return index;
}

/*
 * hop-by-hop 헤더인지 검사합니다.
 * 이 헤더들은 "클라이언트 ↔ 프록시" 한 구간에만 의미가 있어서 다음 구간(백엔드)으로 넘기지 않습니다.
 * 예: 클라이언트가 프록시와 keep-alive를 원해도, 프록시와 백엔드 연결은 별개로 정합니다.
 */
static int is_hop_by_hop(const char *name, size_t length)
{
    static const char *const names[] = {
        "connection", "keep-alive", "proxy-connection", "te", "trailer", "upgrade", "transfer-encoding"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strlen(names[i]) == length && strncasecmp(name, names[i], length) == 0) return 1;
    return 0;
}

int proxy_build_request(const HttpRequest *request, const Backend *backend,
                        char *output, size_t capacity)
{
    size_t used = 0;
    int written = snprintf(output, capacity, "%s %s HTTP/1.1\r\n", request->method, request->target);
    if (written < 0 || (size_t)written >= capacity) return -1;
    used = (size_t)written;

    /* 파서가 검증한 헤더 원문을 한 줄씩 복사합니다. */
    const char *line = (const char *)request->headers;
    const char *end = line + request->headers_length;
    while (line < end) {
        /* 헤더 원문은 '\0'으로 끝나지 않으므로 end를 넘지 않도록 memchr로 찾습니다. 파서가 "\r\n" 줄 끝과 ':'을 보장합니다. */
        const char *newline = memchr(line, '\n', (size_t)(end - line));
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (!newline || !colon || colon > newline) return -1;
        size_t name_length = (size_t)(colon - line);
        size_t line_length = (size_t)(newline - line) + 1;   /* "\r\n" 포함 */
        const char *line_end = newline - 1;
        if (!is_hop_by_hop(line, name_length)) {
            if (used + line_length >= capacity) return -1;
            memcpy(output + used, line, line_length);
            used += line_length;
        }
        line = line_end + 2;
    }
    written = snprintf(output + used, capacity - used, "%s%s%s%s%sConnection: close\r\n\r\n",
                       request->host[0] ? "" : "Host: ",
                       request->host[0] ? "" : backend->host,
                       request->host[0] ? "" : ":",
                       request->host[0] ? "" : backend->port,
                       request->host[0] ? "" : "\r\n");
    if (written < 0 || (size_t)written >= capacity - used) return -1;
    return (int)(used + (size_t)written);
}

/* 응답 첫 줄 "HTTP/1.1 200 OK"에서 상태 코드를 읽습니다. 알 수 없으면 0 (로그용). */
static int response_status(const char *data, size_t length)
{
    if (length < 12 || strncmp(data, "HTTP/1.", 7) != 0 || data[8] != ' ') return 0;
    int status = 0;
    for (int i = 9; i < 12; ++i) {
        if (data[i] < '0' || data[i] > '9') return 0;
        status = status * 10 + (data[i] - '0');
    }
    return status;
}

/* 아직 클라이언트에게 아무것도 보내지 않았을 때 쓰는 502/504 오류 응답 */
static int reply_gateway_error(int client_fd, int status, const HttpRequest *request)
{
    int is_head = strcmp(request->method, "HEAD") == 0;
    return send_error_response(client_fd, status, 0, is_head, NULL) == 0 ? status : -1;
}

/*
 * 백엔드 응답을 기다리는 동안 클라이언트도 함께 지켜봅니다.
 * 이것이 없으면 클라이언트가 이미 떠났어도 백엔드 타임아웃(5초)까지 작업 스레드가 묶여 있습니다.
 * 반환값: 1 = 백엔드에서 읽을 것이 있음, 0 = 시간 초과, -1 = 클라이언트가 떠남
 */
static int wait_backend_or_client_gone(int backend_fd, int client_fd)
{
    struct pollfd fds[2] = {
        {.fd = backend_fd, .events = POLLIN},
        {.fd = client_fd, .events = POLLIN},
    };
    for (;;) {
        int ready = poll(fds, 2, BACKEND_TIMEOUT_SECONDS * 1000);
        if (ready == -1 && errno == EINTR) continue;
        if (ready == -1) return 1;          /* poll 자체 실패: 뒤의 recv가 오류를 알려주게 둡니다 */
        if (ready == 0) return 0;
        if (fds[0].revents) return 1;
        /*
         * 클라이언트 쪽에 이벤트가 있음. MSG_PEEK로 데이터를 꺼내지 않고 상태만 엿봅니다.
         * poll이 "읽을 수 있음"이라 했으므로 이 recv는 기다리지 않습니다.
         *   0  → 클라이언트가 연결을 닫음(EOF),  -1 → RST 등 오류  ⇒ 떠난 것
         *   >0 → 다음 요청(파이프라이닝)이 먼저 도착 ⇒ 떠난 게 아니므로 더는 감시하지 않음
         */
        char byte;
        ssize_t peeked = recv(client_fd, &byte, 1, MSG_PEEK);
        if (peeked == 0 || (peeked == -1 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return -1;
        fds[1].fd = -1; /* poll은 음수 fd를 무시합니다 */
    }
}

int proxy_forward(int client_fd, const HttpRequest *request, ProxyTable *table, ProxyRoute *route)
{
    /* ── 1) 백엔드 고르기 + 연결: 연결이 안 되는 백엔드는 건너뛰고 다음 순번 시도 ── */
    size_t first = proxy_next_backend(table, route);
    const Backend *backend = NULL;
    int backend_fd = -1;
    for (size_t attempt = 0; attempt < route->backend_count && backend_fd == -1; ++attempt) {
        backend = &route->backends[(first + attempt) % route->backend_count];
        backend_fd = connect_with_timeout(backend->host, backend->port, CONNECT_TIMEOUT_MS);
        if (backend_fd == -1)
            fprintf(stderr, "[proxy] backend %s:%s unavailable: %s\n", backend->host, backend->port, strerror(errno));
    }
    if (backend_fd == -1) return reply_gateway_error(client_fd, 502, request);

    /* ── 2) 요청 전달: 헤더를 다시 만들고 본문은 그대로 ── */
    char header[HTTP_MAX_HEADER + 512];
    int header_length = proxy_build_request(request, backend, header, sizeof(header));
    if (header_length < 0 ||
        set_socket_timeout(backend_fd, BACKEND_TIMEOUT_SECONDS) == -1 ||
        send_all(backend_fd, header, (size_t)header_length) == -1 ||
        send_all(backend_fd, request->body, request->body_length) == -1) {
        close_socket(backend_fd);
        return reply_gateway_error(client_fd, 502, request);
    }

    /* ── 3) 응답 중계: 받은 만큼 바로 클라이언트에게. 백엔드가 닫으면(EOF) 끝 ── */
    char chunk[RELAY_CHUNK_SIZE];
    size_t relayed = 0;
    int status = 0;
    for (;;) {
        int ready = wait_backend_or_client_gone(backend_fd, client_fd);
        if (ready == -1) {
            fprintf(stderr, "[proxy] client left while waiting for %s:%s, dropping request\n",
                    backend->host, backend->port);
            status = -1;
            break;
        }
        ssize_t received;
        if (ready == 0) {
            received = -1;
            errno = EAGAIN;   /* 시간 초과 → 아래에서 504 (아직 보낸 것이 없을 때) */
        } else {
            received = recv(backend_fd, chunk, sizeof(chunk), 0);
        }
        if (received == -1 && errno == EINTR) continue;
        if (received <= 0) {
            if (relayed == 0) {
                /* 응답이 한 바이트도 오지 않음: 시간 초과면 504, 그냥 끊겼으면 502 */
                int timed_out = received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
                fprintf(stderr, "[proxy] backend %s:%s %s\n", backend->host, backend->port,
                        timed_out ? "timed out" : "closed without response");
                status = reply_gateway_error(client_fd, timed_out ? 504 : 502, request);
            }
            /* 이미 일부를 보냈다면 상태를 바꿀 수 없으므로 연결을 닫아 끝을 알립니다. */
            break;
        }
        if (relayed == 0) status = response_status(chunk, (size_t)received);
        if (send_all(client_fd, chunk, (size_t)received) == -1) {
            status = -1; /* 클라이언트가 끊김 */
            break;
        }
        relayed += (size_t)received;
    }
    close_socket(backend_fd); /* 모든 경로에서 백엔드 소켓을 닫습니다 */
    return status;
}
