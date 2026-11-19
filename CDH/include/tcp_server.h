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

/* len 바이트를 모두 보낼 때까지 반복 send(). 성공 0, 실패 -1 */
int tcp_send_all(int fd, const void *buf, size_t len);

/* 요청 헤더 끝("\r\n\r\n") 또는 EOF 또는 버퍼가 찰 때까지 수신.
 * 반환: 읽은 바이트 수(>=0), 오류 시 -1. buf 는 항상 NUL 종료된다. */
ssize_t tcp_recv_request(int fd, char *buf, size_t cap);

#endif
