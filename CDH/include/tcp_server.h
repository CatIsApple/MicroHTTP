#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 리스닝 소켓 생성: socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
 * 성공 시 리스닝 fd, 실패 시 -1 */
int tcp_listen(uint16_t port, int backlog);

/* accept() 래퍼. EINTR 시 -1 반환(errno 유지) — 호출자가 종료 플래그 확인.
 * client_addr_buf 가 NULL 이 아니면 "ip:port" 문자열을 채운다. */
int tcp_accept(int listen_fd, char *client_addr_buf, size_t buf_len);

/* len 바이트를 모두 보낼 때까지 반복 send().
 * timeout_ms 는 호출 "전체" 의 마감 시간. 성공 0, 실패·초과 -1 (초과 시 errno = ETIMEDOUT) */
int tcp_send_all(int fd, const void *buf, size_t len, int timeout_ms);

/* 요청 헤더 끝("\r\n\r\n") 또는 EOF 또는 버퍼가 찰 때까지 수신.
 * timeout_ms 는 호출 "전체" 의 마감 시간(천천히 보내는 클라이언트 대비).
 * 반환: 읽은 바이트 수(>=0), 오류·초과 시 -1 (초과 시 errno = ETIMEDOUT).
 * buf 는 항상 NUL 종료된다. */
ssize_t tcp_recv_request(int fd, char *buf, size_t cap, int timeout_ms);

/* 정상 종료. 읽지 않은 수신 데이터가 없으면 바로 close().
 * 남아 있으면 shutdown(SHUT_WR) 로 FIN 을 먼저 보내고 최대 0.5초·64KB 비운 뒤 close().
 * 읽지 않은 데이터가 남은 채 close 하면 커널이 RST 를 보내 응답이 유실될 수 있기 때문. */
void tcp_close_graceful(int fd);

#endif
