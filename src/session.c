/*
 * session.c — 서버 측 Session 저장소 (연결 리스트 + mutex 하나)
 *
 * 규모가 작은 학습용이라 해시 테이블 대신 연결 리스트를 씁니다. 조회는 O(n)이며,
 * 이 선택의 성능 영향은 EXPERIMENT_REPORT에 측정과 함께 기록합니다.
 */
#include "session.h"

#include <pthread.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

typedef struct Session {
    char id[SESSION_ID_LENGTH + 1];
    char user[SESSION_USER_MAX];
    time_t created;
    time_t last_seen;
    struct Session *next;
} Session;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Session *sessions;           /* lock으로 보호 */
static size_t stored;               /* lock으로 보호 */
static int idle_timeout = 1800;     /* 30분 — 시작 전에만 바꿈(session_set_timeouts) */
static int absolute_timeout = 28800; /* 8시간 */

time_t session_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

void session_set_timeouts(int idle_seconds, int absolute_seconds)
{
    idle_timeout = idle_seconds;
    absolute_timeout = absolute_seconds;
}

static int expired(const Session *session, time_t now)
{
    return now - session->last_seen >= idle_timeout || now - session->created >= absolute_timeout;
}

/* 64자 16진수인지 확인합니다. 형식이 틀린 ID는 저장소를 뒤지기 전에 거절합니다. */
static int valid_id(const char *id)
{
    for (size_t i = 0; i < SESSION_ID_LENGTH; ++i) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return id[SESSION_ID_LENGTH] == '\0';
}

int session_create(const char *user, time_t now, char id_out[SESSION_ID_LENGTH + 1])
{
    if (strlen(user) >= SESSION_USER_MAX) return -1;
    Session *session = malloc(sizeof(*session));
    if (!session) return -1;

    /*
     * 예측할 수 없는 ID: 운영체제의 CSPRNG에서 256비트를 받습니다(libsodium randombytes_buf).
     * 순번이나 시각, 사용자 이름으로 만들면 다른 사람의 ID를 추측할 수 있습니다.
     */
    unsigned char random[SESSION_ID_BYTES];
    randombytes_buf(random, sizeof(random));
    sodium_bin2hex(session->id, sizeof(session->id), random, sizeof(random));
    sodium_memzero(random, sizeof(random));
    strcpy(session->user, user);
    session->created = session->last_seen = now;

    pthread_mutex_lock(&lock);
    session->next = sessions;
    sessions = session;
    ++stored;
    memcpy(id_out, session->id, sizeof(session->id)); /* 락 안에서 복사 — 이 뒤로 노드는 다른 스레드 것 */
    pthread_mutex_unlock(&lock);
    return 0;
}

int session_lookup(const char *id, time_t now, SessionInfo *out)
{
    if (!valid_id(id)) return 0;
    int found = 0;
    pthread_mutex_lock(&lock);
    for (Session **link = &sessions; *link; link = &(*link)->next) {
        Session *session = *link;
        /* 비교 시간이 "몇 글자까지 맞았는지"에 따라 달라지지 않도록 상수 시간 비교 */
        if (sodium_memcmp(session->id, id, SESSION_ID_LENGTH) != 0) continue;
        if (expired(session, now)) {
            /* Passive Expiration: 만료된 Session은 갱신하지 않고 그 자리에서 지웁니다. */
            *link = session->next;
            --stored;
            free(session);
        } else {
            session->last_seen = now;
            memcpy(out->user, session->user, sizeof(out->user)); /* 포인터가 아니라 값을 복사 */
            out->created = session->created;
            out->last_seen = session->last_seen;
            found = 1;
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return found;
}

int session_destroy(const char *id)
{
    if (!valid_id(id)) return 0;
    int removed = 0;
    pthread_mutex_lock(&lock);
    for (Session **link = &sessions; *link; link = &(*link)->next) {
        Session *session = *link;
        if (sodium_memcmp(session->id, id, SESSION_ID_LENGTH) == 0) {
            *link = session->next;
            --stored;
            free(session);
            removed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return removed;
}

size_t session_expire(time_t now)
{
    size_t removed = 0;
    pthread_mutex_lock(&lock);
    Session **link = &sessions;
    while (*link) {
        Session *session = *link;
        if (expired(session, now)) {
            *link = session->next;
            free(session);
            ++removed;
        } else {
            link = &session->next;
        }
    }
    stored -= removed;
    pthread_mutex_unlock(&lock);
    return removed;
}

size_t session_count(void)
{
    pthread_mutex_lock(&lock);
    size_t count = stored;
    pthread_mutex_unlock(&lock);
    return count;
}

void session_clear(void)
{
    pthread_mutex_lock(&lock);
    while (sessions) {
        Session *next = sessions->next;
        free(sessions);
        sessions = next;
    }
    stored = 0;
    pthread_mutex_unlock(&lock);
}
