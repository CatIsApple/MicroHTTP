/* conn_job.h — 스레드 풀 / idle poller 가 주고받는 "연결 하나" 단위.
 * 연결 fd 의 소유권이 이 구조체와 함께 넘어간다 (받은 쪽이 close 하거나 다른 곳에 넘겨야 함). */

#ifndef CONN_JOB_H /* 인클루드 가드 */
#define CONN_JOB_H

#define CONN_PEER_MAX 64 /* "IP:포트" 로그 문자열 버퍼 크기 */

typedef struct {
    int fd;                    /* 클라이언트 연결 소켓 (논블로킹 + CLOEXEC) */
    char peer[CONN_PEER_MAX];  /* 로그용 "IP:포트" */
    unsigned nreq;             /* 이 연결에서 지금까지 처리한 요청 수 (keep-alive 상한 판단용) */
} conn_job;

#endif /* CONN_JOB_H */
