/*
 * test_net.c — send_all() 단위 테스트
 *
 * 부분 송신이나 EINTR은 로컬 TCP에서 마음대로 재현하기 어렵습니다.
 * 그래서 net.c를 직접 #include 하기 전에 send를 scripted_send로 바꿔치기해서,
 * "2바이트만 보냄", "EINTR 오류" 같은 결과를 원하는 순서대로 흉내 냅니다.
 */
#define send scripted_send
#include "../../src/net.c"
#undef send

#include <assert.h>

static const unsigned char payload[] = {'A', 0, 'B', 255, 'C', 'D'};
static const int *steps;
static size_t calls, step_count, offset;

/* steps[calls]가 양수면 그만큼 보냈다고 하고, 음수면 그 값을 errno로 하는 오류를 냅니다. */
ssize_t scripted_send(int fd, const void *data, size_t length, int flags)
{
    assert(fd == 42 && flags == 0 && calls < step_count);
    /* send_all이 "아직 안 보낸 부분부터" 이어서 보내는지 확인 */
    assert(data == payload + offset && length == sizeof(payload) - offset);
    int result = steps[calls++];
    if (result < 0) { errno = -result; return -1; }
    offset += (size_t)result;
    return result;
}

static void setup(const int *script, size_t count)
{
    steps = script;
    step_count = count;
    calls = offset = 0;
}

int main(void)
{
    const int partial[] = {2, -EINTR, 1, 3};        /* 일부 → EINTR → 일부 → 나머지 = 성공 */
    setup(partial, 4);
    assert(send_all(42, payload, sizeof(payload)) == 0);
    assert(offset == sizeof(payload) && calls == 4);

    const int broken[] = {2, -EPIPE};               /* 상대가 끊음 → 실패 */
    setup(broken, 2);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EPIPE && offset == 2);

    const int timeout[] = {-EAGAIN};                /* 송신 타임아웃 → 실패 */
    setup(timeout, 1);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EAGAIN);

    const int stalled[] = {0};                      /* 진행 없음 → 무한 반복 대신 EIO */
    setup(stalled, 1);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EIO);

    setup(NULL, 0);                                 /* 보낼 것이 없으면 send를 부르지 않음 */
    assert(send_all(42, payload, 0) == 0 && calls == 0);
    puts("PASS test_net: partial send, EINTR, EPIPE, timeout, zero progress, empty input");
    return 0;
}
