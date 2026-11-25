/* tcp_server.h — TCP 소켓 저수준 함수 모음의 "목차"(선언부).
 * 구현은 src/tcp_server.c 에 있고, main.c 는 이 헤더만 보고 함수를 호출한다. */

#ifndef TCP_SERVER_H /* 인클루드 가드: 이 헤더가 아직 포함된 적 없을 때만 아래 내용을 읽는다 */
#define TCP_SERVER_H /* "이미 포함됨" 표시. 같은 파일을 두 번 include 해도 선언이 중복되지 않음 */

#include <netinet/in.h> /* struct sockaddr_in (tcp_connect 의 대상 주소) */
#include <stddef.h>    /* size_t (바이트 수·길이를 나타내는 부호 없는 정수 타입) */
#include <stdint.h>    /* uint16_t (포트 번호용 16비트 부호 없는 정수) */
#include <sys/types.h> /* ssize_t (바이트 수 또는 -1 에러를 담는 부호 있는 정수, POSIX), off_t (파일 위치) */
#include <sys/uio.h>   /* struct iovec (여러 덩어리 송신용 { 시작 주소, 길이 }) */

/* 리스닝 소켓 생성: socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
 * 성공 시 리스닝 fd, 실패 시 -1 */
int tcp_listen(uint16_t port, int backlog); /* port: 열 포트 번호, backlog: 접속 대기 큐 크기 */

/* accept() 래퍼. EINTR 시 -1 반환(errno 유지) — 호출자가 종료 플래그 확인.
 * 돌려주는 연결 소켓은 논블로킹 + CLOEXEC (이 모듈의 송수신 함수는 모두 마감 시간 안에서 poll 로 기다린다).
 * client_addr_buf 가 NULL 이 아니면 "ip:port" 문자열을 채운다. */
int tcp_accept(int listen_fd, char *client_addr_buf, size_t buf_len); /* 반환: 새 연결 fd 또는 -1 */

/* len 바이트를 모두 보낼 때까지 반복 send().
 * timeout_ms 는 호출 "전체" 의 마감 시간. 성공 0, 실패·초과 -1 (초과 시 errno = ETIMEDOUT) */
int tcp_send_all(int fd, const void *buf, size_t len, int timeout_ms); /* buf 의 len 바이트를 fd 로 전부 전송 */

/* 여러 덩어리 iov[0..iovcnt) 를 순서대로 sendmsg() 로 이어서 보낸다 (응답 머리 + 본문 조각들).
 * 따로 send 하면 작은 패킷으로 갈라져 Nagle + delayed ACK 로 40ms 지연이 생길 수 있고,
 * 하나로 합치려고 복사하면 큰 본문에서 메모리가 낭비된다. 복사 없이 한 번에 보낸다.
 * timeout_ms 는 전체 마감 시간. 성공 0, 실패·초과 -1 (초과 시 errno = ETIMEDOUT).
 * 길이 0 인 덩어리는 건너뜀. 주의: 진행 상황 기록용으로 iov 배열 내용을 바꾼다 (재사용 금지). */
int tcp_sendv_all(int fd, struct iovec *iov, int iovcnt, int timeout_ms);

/* 파일 송신의 최저 평균 속도 (바이트/초). 이보다 느리게 받는 클라이언트는 끊는다 (tcp_sendfile_all 참고).
 * 16 KiB/s = 128 kbit/s: 아주 느린 모바일 회선도 통과하지만, 바이트를 찔끔찔끔 받아
 * 한 연결씩 처리하는 서버를 몇 시간씩 붙잡는 클라이언트는 막는다. */
#ifndef TCP_SEND_MIN_RATE
#define TCP_SEND_MIN_RATE (16 * 1024)
#endif

/* 머리(head) 를 보낸 뒤 파일 file_fd 의 [offset, offset+len) 구간을 보낸다 (정적 파일 응답용).
 *   리눅스: 머리를 MSG_MORE 로 맡겨 두고 sendfile() → 커널 안에서 바로 복사 (zero-copy), 머리와 본문이 같은 패킷.
 *   macOS : sendfile() 의 hdtr 로 머리까지 시스템콜 하나에 (zero-copy). TCP 가 아닌 소켓이면 아래 경로로.
 *   그 밖  : pread + sendmsg 64KB 씩, 첫 조각은 머리와 같이 (작은 파일 = 시스템콜·패킷 하나).
 * 마감은 두 개 중 이른 쪽 (큰 파일도 느린 회선에서 끝까지 가도록 "전체 고정" 마감은 쓰지 않는다):
 *   무진척   : 마지막으로 바이트가 나간 뒤 timeout_ms 동안 진척 없음
 *   최저 속도: 시작 후 timeout_ms + (보낸 바이트 / TCP_SEND_MIN_RATE) 초가 지남
 * 성공 0, 실패 -1 (초과 ETIMEDOUT, 파일이 len 보다 짧으면 EIO). 파일 위치(lseek)는 바꾸지 않는다. */
int tcp_sendfile_all(int sock, const void *head, size_t head_len,
                     int file_fd, off_t offset, size_t len, int timeout_ms);

/* 요청 헤더 끝("\r\n\r\n") 또는 EOF 또는 버퍼가 찰 때까지 수신.
 * used: buf[0..used) 에 이미 있는 바이트 수 (keep-alive 에서 이전 요청 뒤에 딸려 온 파이프라이닝 잔여. 새 연결은 0).
 *   먼저 그 구간에서 헤더 끝을 찾고, 있으면 시스템콜 없이 바로 반환한다 (상대가 더 안 보내도 기다리지 않음).
 *   없으면 그 뒤에 이어 받는다 (검색은 새로 받은 구간 + 앞 3바이트 겹침만).
 *   used >= cap-1 (이미 꽉 참) 인데 헤더 끝이 없으면 받지 않고 header_len 0 으로 반환 (→ 호출자가 431).
 * timeout_ms 는 호출 "전체" 의 마감 시간(천천히 보내는 클라이언트 대비).
 * 반환: buf 에 든 총 바이트 수(잔여 포함, >=0. 잔여 없이 상대가 바로 닫으면 0),
 *       오류·초과 시 -1 (초과 시 errno = ETIMEDOUT, cap 0 이거나 used > cap-1 이면 EINVAL).
 * *header_len: 헤더 끝(빈 줄 포함)까지의 길이. 헤더 끝을 못 찾았으면 0
 *   → 버퍼가 가득 찼다면 헤더가 너무 큰 것, 아니면 상대가 도중에 닫은 것.
 * buf 는 항상 NUL 종료된다 (buf[반환값] == '\0'). */
ssize_t tcp_recv_request(int fd, char *buf, size_t cap, size_t used, int timeout_ms, size_t *header_len); /* cap: buf 크기 */

/* 정확히 len 바이트를 받을 때까지 반복 recv() (본문 수신용).
 * timeout_ms 는 호출 "전체" 의 마감 시간. 성공 0, 실패 -1
 * (초과 시 errno = ETIMEDOUT, 다 받기 전에 상대가 닫으면 errno = ECONNRESET) */
int tcp_recv_exact(int fd, void *buf, size_t len, int timeout_ms); /* buf 에 len 바이트를 채움 */

/* 지금 와 있는 만큼(최대 cap) 받는다. 아무것도 없으면 timeout_ms 까지 기다린다 (프록시 본문 중계용).
 * 반환: 받은 바이트 수(>0), 상대가 닫음 0, 실패·초과 -1 (초과 시 errno = ETIMEDOUT) */
ssize_t tcp_recv_some(int fd, void *buf, size_t cap, int timeout_ms);

/* addr 로 TCP 연결 (논블로킹 connect + poll 로 timeout_ms 안에). 돌려주는 소켓은 논블로킹 + CLOEXEC.
 * 성공 시 fd, 실패 -1 (거절 ECONNREFUSED, 초과 ETIMEDOUT 등 errno) */
int tcp_connect(const struct sockaddr_in *addr, int timeout_ms);

/* 정상 종료. 읽지 않은 수신 데이터가 없으면 바로 close().
 * 남아 있으면 shutdown(SHUT_WR) 로 FIN 을 먼저 보내고 최대 0.5초·64KB 비운 뒤 close().
 * 읽지 않은 데이터가 남은 채 close 하면 커널이 RST 를 보내 응답이 유실될 수 있기 때문. */
void tcp_close_graceful(int fd); /* 응답을 보낸 연결을 RST 없이 닫는다 */

#endif /* TCP_SERVER_H — 인클루드 가드 끝 */
