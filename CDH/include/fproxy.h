/* fproxy.h — Forward Proxy 의 "목차"(선언부). 구현은 src/fproxy.c. 공통 부품은 proxy_core.h.
 *
 *   클라이언트 ──"http://example.com/a 가져와"──▶ microhttp ──▶ example.com:80
 *   클라이언트 ──"CONNECT example.com:443"────▶ microhttp ══(바이트 터널)══▶ example.com:443 (HTTPS)
 *
 * 목적지는 클라이언트가 고른다 (운영자가 정하는 것은 Reverse Proxy). 켜는 법: "-F local" 또는 "-F any".
 * 브라우저·curl 에서 이 서버를 프록시로 지정하면 된다: curl -x http://127.0.0.1:8080 http://example.com/
 *
 * 두 가지 요청:
 *   1. absolute-form  "GET http://host[:port]/path HTTP/1.1"
 *      → host 에 연결, 요청 줄을 "GET /path" 로, Host 를 "host[:port]" 로 바꿔 전달 (나머지는 proxy_core)
 *   2. CONNECT        "CONNECT host:port HTTP/1.1"  (HTTPS 용. 내용은 암호화돼 있어 프록시는 못 봄)
 *      → host 에 연결, "200 Connection Established" 를 보낸 뒤 양쪽 바이트를 그대로 넘겨 준다 (터널)
 *
 * 보안: 아무나 쓰는 프록시(open proxy)는 스팸·공격의 경유지가 된다 → 기본은 로컬(127.x) 클라이언트만 허용.
 * 접두사: fproxy_ */

#ifndef FPROXY_H /* 인클루드 가드 */
#define FPROXY_H

#include "http_parser.h" /* http_request, HTTP_MAX_URI_LEN */
#include <stddef.h>      /* size_t */

#define FPROXY_HOST_MAX 256              /* 호스트 이름 최대 길이 ('\0' 포함) */
#define FPROXY_DEFAULT_TIMEOUT_MS 10000  /* 목적지 응답 기다리는 시간 기본값 (-T 로 변경) */
#define FPROXY_TUNNEL_IDLE_MS 60000      /* CONNECT 터널에서 양쪽 다 조용하면 닫는 시간 */

typedef struct {
    int any_client;     /* 1 = 모든 클라이언트 허용 (-F any) / 0 = 127.x 만 (-F local) */
    int timeout_ms;     /* 목적지 응답 타임아웃 (머리가 올 때까지 / 본문 조각 사이) */
    int tunnel_idle_ms; /* 터널 무활동 타임아웃 */
} fproxy_config;

/* 요청 대상을 쪼갠 결과 */
typedef struct {
    char host[FPROXY_HOST_MAX];          /* "example.com" (연결할 이름) */
    char port[6];                        /* "80" (생략하면 80) */
    char authority[FPROXY_HOST_MAX + 8]; /* Host 헤더 값: "example.com" 또는 "example.com:8080" */
    char path[HTTP_MAX_URI_LEN + 2];     /* origin-form: "/a?b" (경로가 없으면 "/") */
} fproxy_target;

/* 기본 설정으로 초기화: 로컬 클라이언트만, 타임아웃 기본값 */
void fproxy_init(fproxy_config *cfg);

/* uri 가 absolute-form ("http://...", 대소문자 무시) 인지 */
int fproxy_is_absolute(const char *uri);

/* "http://host[:port][/path][?query]" → t. 0 성공, -1 형식 오류 (→ 400)
 * 거절: https:// (TLS 는 CONNECT 로), userinfo("user@host"), IPv6 리터럴, 빈 호스트, 이상한 글자, 포트 범위 밖 */
int fproxy_parse_absolute(const char *uri, fproxy_target *t);

/* CONNECT 의 authority-form "host:port" → t (포트 필수, path 는 비움). 0 성공, -1 형식 오류 (→ 400) */
int fproxy_parse_authority(const char *uri, fproxy_target *t);

/* 이 클라이언트 IP 가 프록시를 써도 되는지 (안 되면 403) */
int fproxy_client_allowed(const fproxy_config *cfg, const char *client_ip);

/* absolute-form 요청 하나를 목적지로 전달하고 응답을 중계한다 (워커 스레드에서 호출).
 * 반환·*keep·*status: pxy_exchange 와 같음 (0 = 다 중계, >0 = 이 코드로 오류 응답(400/502/504), -1 = 연결 끊기) */
int fproxy_handle(const fproxy_config *cfg, int cfd, const http_request *req, const char *client_ip, int *keep,
                  int *status, int client_timeout_ms);

/* CONNECT 터널. 목적지에 연결되면 "200 Connection Established" 를 보내고, 어느 한쪽이 끝낼 때까지 바이트를 넘긴다.
 * pre[0..pre_len): CONNECT 머리 뒤에 이미 같이 들어온 클라이언트 바이트 (먼저 목적지로 보냄)
 * should_stop(arg) 이 1 이면 (서버 종료 중) 터널을 닫는다. NULL 이면 무시
 * *bytes: 양방향으로 넘긴 총 바이트 (로그용)
 * 반환: 0  = 터널을 열었고 끝남 (연결은 닫아야 함)
 *       >0 = 아직 아무것도 안 보냄 → 이 상태 코드로 오류 응답 (400 / 502 / 504)
 *       -1 = 200 을 보내지 못함 (클라이언트 끊김) */
int fproxy_tunnel(const fproxy_config *cfg, int cfd, const http_request *req, const char *pre, size_t pre_len,
                  int client_timeout_ms, int (*should_stop)(void *), void *arg, unsigned long long *bytes);

#endif /* FPROXY_H */
