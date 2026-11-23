/* tcp_server.h — TCP 소켓 저수준 함수 모음의 "목차"(선언부).
 * 구현은 src/tcp_server.c 에 있고, main.c 는 이 헤더만 보고 함수를 호출한다. */

#ifndef TCP_SERVER_H /* 인클루드 가드: 이 헤더가 아직 포함된 적 없을 때만 아래 내용을 읽는다 */
#define TCP_SERVER_H /* "이미 포함됨" 표시. 같은 파일을 두 번 include 해도 선언이 중복되지 않음 */

#include <stddef.h>    /* size_t (바이트 수·길이를 나타내는 부호 없는 정수 타입) */
#include <stdint.h>    /* uint16_t (포트 번호용 16비트 부호 없는 정수) */
#include <sys/types.h> /* ssize_t (바이트 수 또는 -1 에러를 담는 부호 있는 정수, POSIX) */

/* 리스닝 소켓 생성: socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
 * 성공 시 리스닝 fd, 실패 시 -1 */
int tcp_listen(uint16_t port, int backlog); /* port: 열 포트 번호, backlog: 접속 대기 큐 크기 */

/* accept() 래퍼. EINTR 시 -1 반환(errno 유지) — 호출자가 종료 플래그 확인.
 * client_addr_buf 가 NULL 이 아니면 "ip:port" 문자열을 채운다. */
int tcp_accept(int listen_fd, char *client_addr_buf, size_t buf_len); /* 반환: 새 연결 fd 또는 -1 */

/* len 바이트를 모두 보낼 때까지 반복 send().
 * timeout_ms 는 호출 "전체" 의 마감 시간. 성공 0, 실패·초과 -1 (초과 시 errno = ETIMEDOUT) */
int tcp_send_all(int fd, const void *buf, size_t len, int timeout_ms); /* buf 의 len 바이트를 fd 로 전부 전송 */

/* 요청 헤더 끝("\r\n\r\n") 또는 EOF 또는 버퍼가 찰 때까지 수신.
 * timeout_ms 는 호출 "전체" 의 마감 시간(천천히 보내는 클라이언트 대비).
 * 반환: 읽은 바이트 수(>=0), 오류·초과 시 -1 (초과 시 errno = ETIMEDOUT).
 * *header_len: 헤더 끝(빈 줄 포함)까지의 길이. 헤더 끝을 못 찾았으면 0
 *   → 버퍼가 가득 찼다면 헤더가 너무 큰 것, 아니면 상대가 도중에 닫은 것.
 * buf 는 항상 NUL 종료된다. */
ssize_t tcp_recv_request(int fd, char *buf, size_t cap, int timeout_ms, size_t *header_len); /* cap: buf 크기 */

/* 정상 종료. 읽지 않은 수신 데이터가 없으면 바로 close().
 * 남아 있으면 shutdown(SHUT_WR) 로 FIN 을 먼저 보내고 최대 0.5초·64KB 비운 뒤 close().
 * 읽지 않은 데이터가 남은 채 close 하면 커널이 RST 를 보내 응답이 유실될 수 있기 때문. */
void tcp_close_graceful(int fd); /* 응답을 보낸 연결을 RST 없이 닫는다 */

#endif /* TCP_SERVER_H — 인클루드 가드 끝 */
