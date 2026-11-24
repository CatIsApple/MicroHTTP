/* test_mime.c — L4 MIME 표 단위 테스트 */
#include "mime.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define EXPECT_TYPE(path, want)                                                              \
    do {                                                                                     \
        const char *got = mime_from_path(path);                                              \
        if (strcmp(got, want) != 0) {                                                        \
            fprintf(stderr, "FAIL %s:%d  %s -> %s (want %s)\n", __FILE__, __LINE__, path, got, want); \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

int main(void)
{
    EXPECT_TYPE("/index.html", "text/html; charset=utf-8");
    EXPECT_TYPE("/a/b.htm", "text/html; charset=utf-8");
    EXPECT_TYPE("/style.css", "text/css; charset=utf-8");
    EXPECT_TYPE("/app.js", "text/javascript; charset=utf-8");
    EXPECT_TYPE("/img/logo.png", "image/png");
    EXPECT_TYPE("/img/photo.jpg", "image/jpeg");
    EXPECT_TYPE("/img/photo.JPEG", "image/jpeg"); /* 대소문자 무시 */
    EXPECT_TYPE("/data.json", "application/json");
    EXPECT_TYPE("/archive.tar.gz", "application/octet-stream");
    EXPECT_TYPE("/README", "application/octet-stream");
    EXPECT_TYPE("/.hidden", "application/octet-stream");
    EXPECT_TYPE("/dir.d/file", "application/octet-stream"); /* 점이 디렉터리 이름에만 있음 */
    EXPECT_TYPE("/x.", "application/octet-stream");
    EXPECT_TYPE("/x.htmlx", "application/octet-stream");
    if (failures) {
        fprintf(stderr, "test_mime: %d failed\n", failures);
        return 1;
    }
    printf("test_mime: ok\n");
    return 0;
}
