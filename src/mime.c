/* mime.c — L4: 확장자 표로 Content-Type 을 고른다
 *
 * 레이어 구조에서의 위치
 *   L4 정적 파일 핸들러(static.c) 가 파일을 본문으로 넣을 때 Content-Type 값을 여기서 받는다.
 *
 * 구성: { 확장자(소문자), Content-Type } 표 TABLE + 대소문자 무시 비교 ieq + mime_from_path.
 * 새 파일 형식은 TABLE 에 한 줄 추가하면 된다. 표에 없으면 application/octet-stream.
 */
#include "mime.h"

#include <stddef.h>
#include <string.h>

static const struct {
    const char *ext;
    const char *type;
} TABLE[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"pdf", "application/pdf"},
};

#define DEFAULT_TYPE "application/octet-stream"

/* a 와 b 가 ASCII 대소문자 무시하고 같은가 */
static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        if (x != *b) /* 표는 소문자 */
            return 0;
    }
    return *a == *b;
}

/* 경로의 마지막 '/' 뒤 파일 이름에서 마지막 '.' 뒤를 확장자로 보고 TABLE 에서 찾는다 */
const char *mime_from_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) /* 확장자 없음, 또는 ".bashrc" 같은 숨김 파일 */
        return DEFAULT_TYPE;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
        if (ieq(dot + 1, TABLE[i].ext))
            return TABLE[i].type;
    return DEFAULT_TYPE;
}
