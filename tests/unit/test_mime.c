/*
 * test_mime.c — 확장자 → Content-Type 매핑 단위 테스트
 */
#include "static_file.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(const char *path, const char *type)
{
    const char *actual = mime_type_for_path(path);
    if (strcmp(actual, type)) {
        fprintf(stderr, "%s: expected %s, got %s\n", path, type, actual);
        assert(0);
    }
}

int main(void)
{
    expect("/www/index.html", "text/html; charset=utf-8");
    expect("/www/old.htm", "text/html; charset=utf-8");
    expect("/www/css/style.css", "text/css; charset=utf-8");
    expect("/www/js/app.js", "text/javascript; charset=utf-8");
    expect("/www/data.json", "application/json");
    expect("/www/readme.txt", "text/plain; charset=utf-8");
    expect("/www/images/logo.png", "image/png");
    expect("/www/images/photo.jpg", "image/jpeg");
    expect("/www/images/photo.jpeg", "image/jpeg");
    expect("/www/images/PHOTO.JPG", "image/jpeg");               /* 대소문자 무시 */
    expect("/www/anim.gif", "image/gif");
    expect("/www/icon.svg", "image/svg+xml");
    expect("/www/favicon.ico", "image/x-icon");
    expect("/www/doc.pdf", "application/pdf");
    expect("/www/archive.tar.gz", "application/octet-stream");   /* 모르는 확장자 */
    expect("/www/Makefile", "application/octet-stream");         /* 확장자 없음 */
    expect("/www/v1.2/readme", "application/octet-stream");      /* 디렉터리 이름의 '.'은 무시 */
    expect("/www/trailing.", "application/octet-stream");
    puts("PASS test_mime: html, css, js, json, txt, png, jpg, gif, svg, ico, pdf, unknown");
    return 0;
}
