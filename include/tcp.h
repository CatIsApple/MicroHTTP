/* tcp.h — L1: TCP 소켓 레이어
 *
 * 이 레이어는 "바이트를 주고받는 것" 까지만 책임진다. HTTP 는 전혀 모른다.
 *
 * 규약
 *   - 실패하면 -1 을 돌려주고 errno 에 원인을 남긴다. 로그는 찍지 않는다(호출자가 결정).
 *   - 시간 제한은 "이 호출에 몇 ms" 가 아니라 "몇 시까지" (절대 마감 시각, ms) 로 받는다.
 *     요청 하나를 받으려고 recv 를 여러 번 불러도 마감은 하나라서,
 *     1바이트씩 찔끔찔끔 보내는 클라이언트(slowloris)가 시간을 늘릴 수 없다.
 *     마감 시각 = tcp_now_ms() + 제한 시간
 *   - 마감이 지나면 -1, errno = ETIMEDOUT.
 */
#ifndef MICROHTTP_TCP_H
#define MICROHTTP_TCP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 단조 증가 시계(CLOCK_MONOTONIC) 기준 현재 시각, ms. 시스템 시간을 바꿔도 뒤로 가지 않는다. */
long long tcp_now_ms(void);

/* 0.0.0.0:port 에서 연결을 기다리는 소켓을 만든다. 성공: fd, 실패: -1 */
int tcp_listen(uint16_t port, int backlog);

/* 연결 하나를 받는다. 성공: 새 fd, 실패: -1 (시그널로 깨어나면 errno = EINTR).
 * peer 가 NULL 이 아니면 "ip:port" 문자열을 채운다. */
int tcp_accept(int lfd, char *peer, size_t peer_len);

/* 마감 시각까지 기다렸다가, 지금 도착해 있는 만큼(최대 cap 바이트) 읽는다.
 * 반환: >0 읽은 바이트 수, 0 상대가 연결을 닫음(EOF), -1 오류/마감 초과.
 * 요청이 어디서 끝나는지는 판단하지 않는다(그건 L2 파서의 일). buf 를 NUL 로 끝내지도 않는다. */
ssize_t tcp_recv_some(int fd, void *buf, size_t cap, long long deadline_ms);

/* len 바이트를 마감 시각 안에 전부 보낸다. 성공: 0, 실패: -1 */
int tcp_send_all(int fd, const void *buf, size_t len, long long deadline_ms);

/* 연결을 정상적으로 닫는다.
 * 클라이언트가 보낸 데이터를 다 읽지 않은 채 close() 하면 커널이 RST 를 보내고,
 * 그러면 방금 보낸 응답이 클라이언트에 도착하지 못할 수 있다.
 * 그래서 먼저 FIN(shutdown) 을 보내고, 남은 입력을 잠깐(상한 있음) 읽어 버린 뒤 닫는다. */
void tcp_close_graceful(int fd);

#endif
