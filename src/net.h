/*
 * net.h — 소켓 송수신 도우미
 *
 * server.c(클라이언트 쪽)와 proxy.c(백엔드 쪽)가 함께 쓰는 소켓 함수만 모았습니다.
 */
#ifndef MICROHTTP_NET_H
#define MICROHTTP_NET_H

#include <stddef.h>

/* 소켓을 닫습니다. 실패하면 로그만 남기고 재시도하지 않습니다. */
void close_socket(int fd);

/* data의 length 바이트를 전부 보낼 때까지 send()를 반복합니다. 성공 0, 실패 -1. */
int send_all(int fd, const void *data, size_t length);

/* 수신/송신이 seconds초 이상 진행되지 않으면 recv/send가 EAGAIN으로 돌아오게 합니다. 성공 0, 실패 -1. */
int set_socket_timeout(int fd, int seconds);

/*
 * "404 Not Found\n" 같은 짧은 텍스트 오류 응답을 보냅니다.
 *   keep_alive : 응답 뒤 연결 유지 여부를 Connection 헤더로 알림
 *   omit_body  : HEAD 요청이면 1 (헤더만 보냄)
 *   allow      : 405일 때 Allow 헤더 값, 아니면 NULL
 * 성공 0, 실패 -1.
 */
int send_error_response(int fd, int status, int keep_alive, int omit_body, const char *allow);

#endif
