/*
 * session.h — TCP 연결과 독립된 서버 측 Session 저장소
 *
 * 왜 연결(fd)이 아니라 Session ID인가:
 *   TCP 연결은 요청 몇 개 뒤에 닫히고, 닫힌 fd 번호는 커널이 곧바로 다른 클라이언트에게 다시 줍니다.
 *   그래서 "로그인한 사람"은 연결이 아니라 클라이언트가 Cookie로 들고 다니는 Session ID로 식별합니다.
 *
 * 동시성 규칙 (STEP 2):
 *   - 저장소 전체를 mutex 하나로 보호합니다.
 *   - 조회 결과는 포인터가 아니라 SessionInfo **복사본**으로 돌려줍니다.
 *     포인터를 돌려주면 락을 푼 뒤 다른 스레드의 로그아웃/만료가 그 메모리를 free할 수 있습니다(UAF).
 *   - 응답 생성과 send()는 락 밖에서 합니다.
 *
 * 시간: 모든 함수는 현재 시각(now)을 인자로 받습니다. 서버는 session_now()(단조 시계)를 넘기고,
 *       단위 테스트는 원하는 시각을 직접 넘겨 만료를 기다리지 않고 검증합니다.
 */
#ifndef MICROHTTP_SESSION_H
#define MICROHTTP_SESSION_H

#include <stddef.h>
#include <time.h>

enum {
    SESSION_ID_BYTES = 32,                     /* 256비트 난수 */
    SESSION_ID_LENGTH = SESSION_ID_BYTES * 2,  /* 16진수 문자열 길이 */
    SESSION_USER_MAX = 32
};

/* 조회 결과 (저장소 안의 노드와 독립된 복사본) */
typedef struct {
    char user[SESSION_USER_MAX];
    time_t created;
    time_t last_seen;
} SessionInfo;

/* 단조 시계(초). 시스템 시각을 바꿔도 되돌아가지 않으므로 만료 계산에 씁니다. */
time_t session_now(void);

/* Idle Timeout(마지막 사용 후), Absolute Timeout(생성 후) — 초 단위 */
void session_set_timeouts(int idle_seconds, int absolute_seconds);

/* 새 Session을 만들고 ID(64자 16진수 + '\0')를 id_out에 씁니다. 성공 0, 실패 -1. */
int session_create(const char *user, time_t now, char id_out[SESSION_ID_LENGTH + 1]);

/*
 * ID로 Session을 찾아 복사본을 out에 씁니다. 찾으면 1, 없거나 만료면 0.
 * 만료된 Session은 그 자리에서 지우고(Passive Expiration), 만료된 것은 갱신하지 않습니다.
 * 유효하면 last_seen을 now로 갱신합니다.
 */
int session_lookup(const char *id, time_t now, SessionInfo *out);

/* 로그아웃: 서버 측에서 무효화. 지웠으면 1, 없었으면 0. */
int session_destroy(const char *id);

/* 만료된 Session을 모두 지우고 지운 개수를 돌려줍니다 (Active Expiration). */
size_t session_expire(time_t now);

/* 현재 저장된 Session 수 (실험·테스트용) */
size_t session_count(void);

/* 모두 해제 (서버 종료 시) */
void session_clear(void);

#ifdef SESSION_EXPERIMENTS
/*
 * ── STEP 2 실험 전용 (일반 빌드에는 없음) ──
 * session_lookup_unsafe : 노드 안의 사용자 이름 "포인터"를 돌려줍니다. 락을 푼 뒤에 쓰게 되므로,
 *                         그 사이 다른 스레드가 로그아웃/만료로 노드를 free하면 Use-After-Free.
 * session_lookup_hold   : 찾으면 락을 쥔 채로 돌아옵니다(넓은 락). 응답을 보낸 뒤 session_release().
 * session_lock_report   : 락 획득 횟수, 평균 대기·점유 시간을 stderr로 출력.
 */
const char *session_lookup_unsafe(const char *id, time_t now);
int session_lookup_hold(const char *id, time_t now, SessionInfo *out);
void session_release(void);
void session_lock_report(void);
#endif

#endif
