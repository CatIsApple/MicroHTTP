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

static void setup(const int *script, size_t count)
{
    steps = script;
    step_count = count;
    calls = offset = 0;
}

int main(void)
{
    const int partial[] = {2, -EINTR, 1, 3};
    setup(partial, 4);
    assert(send_all(42, payload, sizeof(payload)) == 0);
    assert(offset == sizeof(payload) && calls == 4);
    const int broken[] = {2, -EPIPE};
    setup(broken, 2);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EPIPE && offset == 2);
    const int timeout[] = {-EAGAIN};
    setup(timeout, 1);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EAGAIN);
    const int stalled[] = {0};
    setup(stalled, 1);
    assert(send_all(42, payload, sizeof(payload)) == -1 && errno == EIO);
    setup(NULL, 0);
    assert(send_all(42, payload, 0) == 0 && calls == 0);
    puts("PASS: partial send, EINTR, EPIPE, timeout, zero progress, empty input");
    return 0;
}
