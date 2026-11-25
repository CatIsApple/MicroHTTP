/*
 * test_session.c — Session 저장소 단위 테스트
 *
 * 시각(now)을 인자로 넘기므로 실제로 기다리지 않고 만료를 검증합니다.
 * 마지막 테스트는 여러 스레드가 동시에 생성·조회·삭제·만료를 해서 TSan/ASan이 볼 수 있게 합니다.
 */
#include "session.h"

#include <assert.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

static void test_create_lookup_destroy(void)
{
    session_clear();
    session_set_timeouts(60, 600);
    char a[SESSION_ID_LENGTH + 1], b[SESSION_ID_LENGTH + 1];
    assert(session_create("alice", 1000, a) == 0);
    assert(session_create("alice", 1000, b) == 0);
    assert(strlen(a) == SESSION_ID_LENGTH && strcmp(a, b) != 0); /* 같은 사용자라도 매번 다른 난수 ID */
    assert(session_count() == 2);

    SessionInfo info;
    assert(session_lookup(a, 1010, &info) == 1);
    assert(strcmp(info.user, "alice") == 0 && info.created == 1000 && info.last_seen == 1010);

    assert(session_destroy(a) == 1);
    assert(session_lookup(a, 1011, &info) == 0);                /* 로그아웃한 ID는 다시 못 씀 */
    assert(session_destroy(a) == 0);
    assert(session_count() == 1);
}

static void test_invalid_ids(void)
{
    SessionInfo info;
    char forged[SESSION_ID_LENGTH + 1];
    memset(forged, 'a', SESSION_ID_LENGTH);
    forged[SESSION_ID_LENGTH] = '\0';
    assert(session_lookup(forged, 1000, &info) == 0);           /* 형식은 맞지만 없는 ID */
    assert(session_lookup("alice", 1000, &info) == 0);          /* 사용자 이름을 ID처럼 */
    assert(session_lookup("", 1000, &info) == 0);
    forged[10] = 'G';
    assert(session_lookup(forged, 1000, &info) == 0);           /* 16진수가 아닌 문자 */
    char too_long[SESSION_ID_LENGTH + 2];
    memset(too_long, 'a', SESSION_ID_LENGTH + 1);
    too_long[SESSION_ID_LENGTH + 1] = '\0';
    assert(session_lookup(too_long, 1000, &info) == 0);
}

static void test_idle_and_absolute_timeout(void)
{
    session_clear();
    session_set_timeouts(60, 300);
    char id[SESSION_ID_LENGTH + 1];
    SessionInfo info;

    /* Idle: 마지막 사용 후 60초 */
    assert(session_create("bob", 1000, id) == 0);
    assert(session_lookup(id, 1059, &info) == 1);                /* 59초 뒤 사용 → 갱신 */
    assert(session_lookup(id, 1118, &info) == 1);                /* 갱신 덕분에 아직 유효 */
    assert(session_lookup(id, 1178, &info) == 0);                /* 60초 동안 사용 안 함 → 만료 */
    assert(session_count() == 0);                                /* Passive Expiration: 조회하면서 지움 */

    /* Absolute: 계속 써도 생성 후 300초면 끝 */
    assert(session_create("bob", 2000, id) == 0);
    for (time_t t = 2050; t < 2300; t += 50) assert(session_lookup(id, t, &info) == 1);
    assert(session_lookup(id, 2300, &info) == 0);
}

static void test_expired_session_is_not_renewed(void)
{
    session_clear();
    session_set_timeouts(60, 600);
    char id[SESSION_ID_LENGTH + 1];
    SessionInfo info;
    assert(session_create("carol", 1000, id) == 0);
    assert(session_lookup(id, 1060, &info) == 0);                /* 만료 시점의 조회가 수명을 늘리면 안 됨 */
    assert(session_lookup(id, 1061, &info) == 0);
}

static void test_active_expiration(void)
{
    session_clear();
    session_set_timeouts(60, 600);
    char id[SESSION_ID_LENGTH + 1];
    for (int i = 0; i < 5; ++i) assert(session_create("old", 1000, id) == 0);
    for (int i = 0; i < 3; ++i) assert(session_create("new", 1050, id) == 0);
    assert(session_count() == 8);
    assert(session_expire(1059) == 0);
    assert(session_expire(1060) == 5);                           /* 요청 없이도 정리 */
    assert(session_count() == 3);
    assert(session_expire(1110) == 3);
    assert(session_count() == 0);
}

/* ── 동시 접근: 스레드마다 생성 → 조회 → (절반은) 로그아웃, 그 사이 만료 스레드가 계속 훑음 ── */
enum { THREADS = 8, ROUNDS = 2000 };
static volatile int stop_sweeper;

static void *client_thread(void *arg)
{
    long index = (long)arg;
    char id[SESSION_ID_LENGTH + 1];
    SessionInfo info;
    for (int i = 0; i < ROUNDS; ++i) {
        assert(session_create(index % 2 ? "odd" : "even", 5000, id) == 0);
        assert(session_lookup(id, 5000, &info) == 1);
        assert(strcmp(info.user, index % 2 ? "odd" : "even") == 0);
        if (i % 2) assert(session_destroy(id) == 1);
    }
    return NULL;
}

static void *sweeper_thread(void *unused)
{
    (void)unused;
    /* 실제 서버는 1초마다 훑습니다. 쉬지 않고 돌면 Valgrind(스레드를 하나씩 실행)에서 다른 스레드가 굶습니다. */
    struct timespec pause = { 0, 50000 };   /* 50us */
    while (!__atomic_load_n(&stop_sweeper, __ATOMIC_SEQ_CST)) {
        session_expire(5000);               /* 아무것도 만료되지 않는 시각 */
        nanosleep(&pause, NULL);
    }
    return NULL;
}

static void test_concurrent_access(void)
{
    session_clear();
    session_set_timeouts(60, 600);
    pthread_t clients[THREADS], sweeper;
    assert(pthread_create(&sweeper, NULL, sweeper_thread, NULL) == 0);
    for (long i = 0; i < THREADS; ++i) assert(pthread_create(&clients[i], NULL, client_thread, (void *)i) == 0);
    for (int i = 0; i < THREADS; ++i) pthread_join(clients[i], NULL);
    __atomic_store_n(&stop_sweeper, 1, __ATOMIC_SEQ_CST);
    pthread_join(sweeper, NULL);
    assert(session_count() == THREADS * ROUNDS / 2);
    assert(session_expire(5060) == THREADS * ROUNDS / 2);
    session_clear();
}

int main(void)
{
    assert(sodium_init() >= 0);
    test_create_lookup_destroy();
    test_invalid_ids();
    test_idle_and_absolute_timeout();
    test_expired_session_is_not_renewed();
    test_active_expiration();
    test_concurrent_access();
    session_clear();
    puts("PASS test_session: create/lookup/destroy, forged ids, idle/absolute timeout, no renewal after expiry, active expiration, concurrent access");
    return 0;
}
