/*
 * fail_malloc.c — 시험용 malloc 실패 주입 (Linux glibc 전용, 서버 코드는 바꾸지 않음)
 *
 *   gcc -shared -fPIC -o fail_malloc.so tests/linux/fail_malloc.c
 *   FAIL_MALLOC_SIZE=81920 LD_PRELOAD=./fail_malloc.so ./server ...
 *
 * 크기가 정확히 FAIL_MALLOC_SIZE인 malloc만 NULL을 돌려줍니다.
 * 81920 = 연결 버퍼(HTTP_MAX_HEADER 16384 + HTTP_MAX_BODY 65536) → handle_connection의 500 경로를 실제로 지나가게 합니다.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>

extern void *__libc_malloc(size_t size); /* glibc의 진짜 malloc */

void *malloc(size_t size)
{
    static long fail_size = -1;
    if (fail_size == -1) {
        const char *text = getenv("FAIL_MALLOC_SIZE");
        fail_size = text ? atol(text) : 0;
    }
    if (fail_size > 0 && size == (size_t)fail_size) {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_malloc(size);
}
