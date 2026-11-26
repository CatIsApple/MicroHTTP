/*
 * step2_uaf.c — STEP 2 실험: 조회 결과를 "포인터"로 넘기면 왜 Use-After-Free가 되는가
 *
 *   make build/step2_uaf CFLAGS='-std=c11 -O1 -g -fsanitize=address' LDFLAGS=-fsanitize=address
 *   build/step2_uaf unsafe     → ASan: heap-use-after-free
 *   build/step2_uaf safe       → 보고 없음
 *
 * 읽는 스레드: Session을 조회하고, 락 밖에서 "응답을 만드는 시간"(200us) 동안 사용자 이름을 씁니다.
 * 쓰는 스레드: 같은 Session을 로그아웃(session_destroy → free)하고 다시 로그인하기를 반복합니다.
 *   unsafe : session_lookup_unsafe()가 노드 안의 포인터를 돌려줌 → 그 사이 free되면 해제된 메모리를 읽음
 *   safe   : session_lookup()이 락 안에서 값을 복사 → 노드가 사라져도 복사본은 안전
 */
#include "session.h"

#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

enum { ROUNDS = 3000 };

static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER; /* 실험 하니스의 "현재 ID" 보호 (저장소와 별개) */
static char current_id[SESSION_ID_LENGTH + 1];
static int unsafe_mode;
static long responses, misses;

static void pause_us(long microseconds)
{
    struct timespec pause = { 0, microseconds * 1000 };
    nanosleep(&pause, NULL);
}

static void *reader(void *unused)
{
    (void)unused;
    for (int i = 0; i < ROUNDS; ++i) {
        char id[SESSION_ID_LENGTH + 1];
        pthread_mutex_lock(&id_lock);
        memcpy(id, current_id, sizeof(id));
        pthread_mutex_unlock(&id_lock);

        char response[128];
        if (unsafe_mode) {
            const char *user = session_lookup_unsafe(id, 100);
            if (!user) { ++misses; continue; }
            pause_us(200);                                              /* 락 밖에서 응답 생성 */
            snprintf(response, sizeof(response), "user: %s\n", user);  /* ✗ free됐을 수 있는 메모리 */
        } else {
            SessionInfo info;
            if (!session_lookup(id, 100, &info)) { ++misses; continue; }
            pause_us(200);
            snprintf(response, sizeof(response), "user: %s\n", info.user); /* ✓ 복사본 */
        }
        ++responses;
    }
    return NULL;
}

static void *writer(void *unused)
{
    (void)unused;
    for (int i = 0; i < ROUNDS; ++i) {
        char old_id[SESSION_ID_LENGTH + 1], new_id[SESSION_ID_LENGTH + 1];
        pthread_mutex_lock(&id_lock);
        memcpy(old_id, current_id, sizeof(old_id));
        pthread_mutex_unlock(&id_lock);
        pause_us(100);
        session_destroy(old_id);                                       /* 로그아웃 → free */
        session_create("userA", 100, new_id);                          /* 다시 로그인 */
        pthread_mutex_lock(&id_lock);
        memcpy(current_id, new_id, sizeof(current_id));
        pthread_mutex_unlock(&id_lock);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "unsafe") && strcmp(argv[1], "safe"))) {
        fprintf(stderr, "usage: %s unsafe|safe\n", argv[0]);
        return 2;
    }
    unsafe_mode = strcmp(argv[1], "unsafe") == 0;
    if (sodium_init() < 0) return 1;
    session_set_timeouts(3600, 3600);
    session_create("userA", 100, current_id);
    pthread_t read_thread, write_thread;
    pthread_create(&read_thread, NULL, reader, NULL);
    pthread_create(&write_thread, NULL, writer, NULL);
    pthread_join(read_thread, NULL);
    pthread_join(write_thread, NULL);
    printf("%s: responses=%ld misses(logged out at lookup)=%ld sessions_left=%zu\n",
           argv[1], responses, misses, session_count());
    session_clear();
    return 0;
}
