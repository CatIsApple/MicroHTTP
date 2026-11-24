/*
 * test_static.c — 정적 파일 찾기(경로 → 파일, 403/404/400) 단위 테스트
 *
 * 임시 디렉터리에 작은 루트를 만들어 두고 static_file_open()의 결과를 확인합니다.
 */
#define _DARWIN_C_SOURCE /* macOS는 _POSIX_C_SOURCE만 켜면 mkdtemp 선언을 숨깁니다 (Linux에서는 무시됨) */
#include "static_file.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[256];  /* 임시 디렉터리 */
static char *root;      /* base/root 의 realpath */

static void write_file(const char *relative, const char *content)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base, relative);
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(content, file);
    fclose(file);
}

/* target을 열어 보고 상태 코드를 돌려줍니다. 200이면 내용이 expected인지도 확인합니다. */
static int open_target(const char *target, const char *expected_content, const char *expected_type)
{
    int fd = -1;
    size_t size = 0;
    const char *type = NULL;
    int status = static_file_open(root, target, &fd, &size, &type);
    if (status == 200) {
        char content[256] = {0};
        assert(size < sizeof(content));
        assert(read(fd, content, size) == (ssize_t)size);
        close(fd);
        if (expected_content) assert(!strcmp(content, expected_content));
        if (expected_type) assert(!strcmp(type, expected_type));
    }
    return status;
}

int main(void)
{
    snprintf(base, sizeof(base), "/tmp/microhttp_static_XXXXXX");
    assert(mkdtemp(base));
    char path[512];
    snprintf(path, sizeof(path), "%s/root", base);                assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/root/docs", base);           assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/root/empty", base);          assert(mkdir(path, 0755) == 0);
    write_file("root/index.html", "home");
    write_file("root/docs/index.html", "docs");
    write_file("root/a b.txt", "space");
    write_file("root/secret.txt", "secret");
    write_file("outside.txt", "outside");
    /* 루트 안에 있지만 루트 밖을 가리키는 심볼릭 링크 */
    char link_target[512], link_path[512];
    snprintf(link_target, sizeof(link_target), "%s/outside.txt", base);
    snprintf(link_path, sizeof(link_path), "%s/root/escape.txt", base);
    assert(symlink(link_target, link_path) == 0);
    /* 읽기 권한이 없는 파일 */
    snprintf(path, sizeof(path), "%s/root/secret.txt", base);
    assert(chmod(path, 0000) == 0);
    snprintf(path, sizeof(path), "%s/root", base);
    root = realpath(path, NULL);
    assert(root);

    assert(open_target("/", "home", "text/html; charset=utf-8") == 200);       /* 디렉터리 → index.html */
    assert(open_target("/index.html?v=1", "home", NULL) == 200);              /* 쿼리는 무시 */
    assert(open_target("/docs/", "docs", NULL) == 200);
    assert(open_target("/docs", "docs", NULL) == 200);                        /* '/' 없이 디렉터리 */
    assert(open_target("/a%20b.txt", "space", "text/plain; charset=utf-8") == 200); /* %20 디코딩 */
    assert(open_target("/missing.html", NULL, NULL) == 404);
    assert(open_target("/missing/dir/", NULL, NULL) == 404);
    assert(open_target("/empty/", NULL, NULL) == 403);                        /* 목록은 보여주지 않음 */
    assert(open_target("/../outside.txt", NULL, NULL) == 403);                /* .. 탈출 */
    assert(open_target("/docs/../../outside.txt", NULL, NULL) == 403);
    assert(open_target("/%2e%2e/outside.txt", NULL, NULL) == 403);            /* 인코딩된 .. */
    assert(open_target("/escape.txt", NULL, NULL) == 403);                    /* 심볼릭 링크 탈출 */
    assert(open_target("/a%zzb", NULL, NULL) == 400);                         /* 잘못된 인코딩 */
    assert(open_target("/a%00.html", NULL, NULL) == 400);                     /* NUL 주입 */
    if (geteuid() != 0)                                                       /* root는 권한 검사를 무시함 */
        assert(open_target("/secret.txt", NULL, NULL) == 403);

    /* 정리 */
    chmod(path, 0755);
    char command[600];
    snprintf(command, sizeof(command), "chmod -R u+rwx '%s' && rm -rf '%s'", base, base);
    assert(system(command) == 0);
    free(root);
    puts("PASS test_static: index, query, percent-decode, 404, 403 (.., symlink, permission, dir), 400");
    return 0;
}
