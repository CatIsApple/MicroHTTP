/*
 * test_send.c — server.c의 send_all() 단위 테스트
 *
 * 트릭: server.c를 통째로 #include 하되, 그 직전에
 *   - main  → server_entrypoint 로 이름을 바꿔 이 파일의 main과 충돌하지 않게 하고
 *   - send  → scripted_send 로 바꿔, 실제 소켓 대신 아래의 "가짜 send"가 불리게 합니다.
 * 가짜 send는 steps 배열에 적힌 대로 "2바이트만 보냄", "EINTR 오류" 같은 결과를 흉내 냅니다.
 */
/* 부분 송신과 EINTR은 로컬 TCP에서 항상 재현되지 않아 반환값을 직접 제어한다. */
#define main server_entrypoint
#define send scripted_send
#include "../server.c"
#undef main
#undef send

#include <assert.h>

static const unsigned char payload[] = {'A', 0, 'B', 255, 'C', 'D'};
static const int *steps;
static size_t calls, step_count, offset;

/*
 * 가짜 send. steps[calls]가 양수면 그만큼 보냈다고 하고, 음수면 그 값을 errno로 하는 오류를 냅니다.
 * send_all이 "아직 안 보낸 부분부터" 정확히 이어서 보내는지도 assert로 확인합니다.
 */
ssize_t scripted_send(int fd, const void *data, size_t length, int flags)
{
    assert(fd == 42 && flags == 0 && calls < step_count);
    assert(data == payload + offset && length == sizeof(payload) - offset);
    int result = steps[calls++];
    if (result < 0) { errno = -result; return -1; }
    assert((size_t)result <= length);
    offset += (size_t)result;
    return result;
}

/* 다음 테스트에서 쓸 가짜 send의 결과 순서를 지정합니다. */
static void setup(const int *script, size_t count)
{
    steps = script;
    step_count = count;
    calls = offset = 0;
}

int main(void)
{
    /* 일부만 보냄 → EINTR → 다시 일부 → 나머지: 결국 전부 보내고 성공해야 함 */
    const int partial[] = {2, -EINTR, 1, 3};
    setup(partial, 4);
    assert(send_all(42, payload, sizeof(payload)) == 0);
    assert(offset == sizeof(payload) && calls == 4);
    /* 보내던 중 상대가 연결을 끊음(EPIPE) → 실패 */
    const int broken[] = {2, -EPIPE};
    setup(broken, 2);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EPIPE && offset == 2);
    /* 송신 타임아웃(EAGAIN) → 실패 */
    const int timeout[] = {-EAGAIN};
    setup(timeout, 1);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EAGAIN);
    /* 0바이트 진행 → 무한 반복 대신 EIO로 실패 */
    const int stalled[] = {0};
    setup(stalled, 1);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EIO);
    /* 보낼 것이 없으면 send를 아예 호출하지 않음 */
    setup(NULL, 0);
    assert(send_all(42, payload, 0) == 0 && calls == 0);
    puts("PASS: partial send, EINTR, EPIPE, timeout, zero progress, empty input");
    return 0;
}
