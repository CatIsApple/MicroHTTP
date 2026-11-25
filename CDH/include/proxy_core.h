/* proxy_core.h — 프록시 공통 부품 (Reverse Proxy 와 Forward Proxy 가 함께 씀). 구현은 src/proxy_core.c.
 *
 *   rproxy (리버스: -P 경로 규칙 → 정해진 백엔드)  ─┐
 *   fproxy (포워드: 클라이언트가 고른 목적지)      ─┴─▶ proxy_core: 요청 머리 다시 쓰기, 응답 머리 검사·다시 쓰기, 본문 중계
 *
 * "어디로 보낼지"(목적지 고르기·connect)는 각 모듈이, "연결된 상대와 요청 하나 주고받기"는 여기서 한다.
 * 접두사: pxy_ */

#ifndef PROXY_CORE_H /* 인클루드 가드 */
#define PROXY_CORE_H

#include "http_parser.h" /* http_request */
#include <stddef.h>      /* size_t */

#define PXY_CONNECT_TIMEOUT_MS 2000 /* 상대 서버 connect 최대 시간 (응답 타임아웃이 더 짧으면 그 값) */

/* 상대 서버로 보낼 요청을 어떻게 다시 쓸지 */
typedef struct {
    const char *target;    /* 요청 줄의 경로 (리버스: req->uri 그대로 / 포워드: absolute-form 을 origin-form 으로 바꾼 것) */
    const char *client_ip; /* X-Forwarded-For 에 덧붙일 클라이언트 IP */
    const char *host;      /* Host 값: 클라이언트가 Host 를 안 보냈을 때, 또는 replace_host 일 때 씀 */
    int replace_host;      /* 1 = 클라이언트 Host 를 버리고 host 로 (포워드, RFC 7230 5.4) / 0 = 클라이언트 것 유지 (리버스) */
} pxy_req_opts;

/* 상대 서버로 보낼 요청 머리를 out 에 쓴다 (본문은 따로 보냄):
 *   <METHOD> <o->target> HTTP/1.<클라이언트 버전>
 *   클라이언트 헤더들 — hop-by-hop(Connection, Keep-Alive, Proxy-Connection, TE, Trailer, Upgrade,
 *                      Transfer-Encoding, Expect, Connection 에 적힌 이름), Proxy-Authorization,
 *                      X-Forwarded-For/Proto 는 뺌 (replace_host 면 Host 도)
 *   Host: <o->host>               ← 클라이언트가 Host 를 안 보냈거나(HTTP/1.0) replace_host 일 때
 *   X-Forwarded-For: <기존 값, >client_ip
 *   X-Forwarded-Proto: http
 *   Connection: close
 * 반환: 쓴 길이, out 이 모자라면 -1 */
int pxy_build_request(char *out, size_t cap, const http_request *req, const pxy_req_opts *o);

/* 상대 서버 응답 머리(상태 줄 ~ 빈 줄, head_len 바이트)를 해석한 결과 */
typedef struct {
    int status;                 /* 상태 코드 (100~599) */
    int has_length;             /* Content-Length 가 있음 (Transfer-Encoding 이 있으면 무시하고 0) */
    unsigned long long length;  /* Content-Length 값 */
    int chunked;                /* Transfer-Encoding 의 마지막이 chunked */
    int has_te;                 /* Transfer-Encoding 이 있음 (chunked 아니면 본문 = 연결 끝까지) */
} pxy_resp_info;

/* 응답 머리 검사·해석. 0 성공, -1 형식 오류 (→ 502): 상태 줄 이상, 줄 안의 CR/LF/NUL, 콜론 없는 줄,
 * obs-fold, 숫자가 아니거나 서로 다른 Content-Length */
int pxy_parse_response_head(const char *buf, size_t head_len, pxy_resp_info *info);

/* 클라이언트로 보낼 응답 머리: 상태 줄은 "HTTP/1.1 <원래 코드·문구>", 상대 헤더에서 hop-by-hop 을 빼고
 * "Connection: keep-alive|close" 를 붙인다. 반환: 쓴 길이, 모자라면 -1 */
int pxy_rewrite_response_head(char *out, size_t cap, const char *buf, size_t head_len, int keep_alive);

/* 이미 연결된 상대 서버(bfd)에 요청 하나를 보내고 응답을 클라이언트(cfd)에 그대로 중계한다 (워커 스레드에서 호출).
 * bfd 는 성공·실패와 관계없이 이 함수가 닫는다.
 * o: 요청을 어떻게 다시 쓸지 (pxy_build_request 와 같음)
 * backend_timeout_ms: 상대 응답 대기 (머리가 올 때까지 / 본문 조각 사이의 최대 무진척 시간)
 * *keep: 들어올 때 keep-alive 로 답할지, 나갈 때 실제로 그렇게 답했는지 (길이를 모르는 본문이면 0 으로 바꿈)
 * *status: 클라이언트에 보낸 상태 코드 (로그용)
 * 반환: 0  = 응답을 다 중계함
 *       >0 = 클라이언트에 아직 아무것도 안 보냄 → 이 상태 코드로 오류 응답을 보내라 (502 / 504 / 500)
 *       -1 = 중계 도중 실패 (클라이언트 송신 실패, 상대가 본문 도중 끊김·멈춤) → 연결을 그냥 닫아라 */
int pxy_exchange(int bfd, int cfd, const http_request *req, const pxy_req_opts *o, int backend_timeout_ms,
                 int client_timeout_ms, int *keep, int *status);

#endif /* PROXY_CORE_H */
