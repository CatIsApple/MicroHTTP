/* test_mime.c — mime_type_for 단위 테스트 */

#include "mime.h"
#include "test.h"

#include <stddef.h> /* size_t */
#include <string.h> /* strcmp */

const void *mime_table_for_test(size_t *count, size_t *entry_size); /* mime.c 내부 표 (정렬 검사용) */

int main(void)
{
    /* readme 필수 타입 */
    CHECK_STR(mime_type_for("index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for("a.htm"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for("style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for("app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for("logo.png"), "image/png");
    CHECK_STR(mime_type_for("photo.jpg"), "image/jpeg");
    CHECK_STR(mime_type_for("photo.jpeg"), "image/jpeg");

    /* 그 밖의 흔한 타입 */
    CHECK_STR(mime_type_for("data.json"), "application/json");
    CHECK_STR(mime_type_for("icon.svg"), "image/svg+xml");
    CHECK_STR(mime_type_for("favicon.ico"), "image/x-icon");
    CHECK_STR(mime_type_for("a.txt"), "text/plain; charset=utf-8");
    CHECK_STR(mime_type_for("f.woff2"), "font/woff2");
    CHECK_STR(mime_type_for("m.wasm"), "application/wasm");

    /* 대소문자 무시 */
    CHECK_STR(mime_type_for("INDEX.HTML"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for("Photo.JpG"), "image/jpeg");

    /* 경로: 마지막 확장자만, 디렉터리의 점은 무시 */
    CHECK_STR(mime_type_for("css/site.min.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for("a.tar.gz"), "application/gzip");
    CHECK_STR(mime_type_for("v1.2/README"), MIME_DEFAULT);
    CHECK_STR(mime_type_for("dir.html/file"), MIME_DEFAULT);

    /* 확장자 없음 / 숨김 파일 / 점으로 끝남 / 모르는 확장자 / 너무 긴 확장자 */
    CHECK_STR(mime_type_for("Makefile"), MIME_DEFAULT);
    CHECK_STR(mime_type_for(".htaccess"), MIME_DEFAULT);
    CHECK_STR(mime_type_for("a/.html"), MIME_DEFAULT);
    CHECK_STR(mime_type_for("file."), MIME_DEFAULT);
    CHECK_STR(mime_type_for("x.unknownext"), MIME_DEFAULT);
    CHECK_STR(mime_type_for("x.xyz"), MIME_DEFAULT);
    CHECK_STR(mime_type_for(""), MIME_DEFAULT);
    CHECK_STR(mime_type_for("x.htmlhtmlhtml"), MIME_DEFAULT);

    /* 표가 정렬돼 있어야 bsearch 가 맞다 (항목 추가 시 실수 방지) */
    size_t count, esz;
    const char *tbl = mime_table_for_test(&count, &esz);
    for (size_t i = 1; i < count; i++) {
        const char *prev = *(const char *const *)(tbl + (i - 1) * esz); /* 각 항목의 첫 필드 = ext */
        const char *cur = *(const char *const *)(tbl + i * esz);
        CHECK(strcmp(prev, cur) < 0);
    }
    /* 표의 모든 확장자가 실제로 찾아짐 */
    for (size_t i = 0; i < count; i++) {
        const char *ext = *(const char *const *)(tbl + i * esz);
        char name[32] = "f.";
        strcat(name, ext);
        CHECK(strcmp(mime_type_for(name), MIME_DEFAULT) != 0);
    }

    return TEST_REPORT();
}
