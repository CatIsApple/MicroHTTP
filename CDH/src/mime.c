/* mime.c — 확장자 → Content-Type 표와 조회.
 *
 * 성능: 요청마다 한 번 불린다. 확장자를 소문자로 복사(최대 8글자)한 뒤
 * 정렬된 표를 이진 탐색(bsearch) → 비교 5~6번. 할당 없음. */

#include "mime.h" /* 이 파일이 구현할 함수의 선언 */

#include <stdlib.h> /* bsearch */
#include <string.h> /* strrchr, strcmp */

#define EXT_MAX 8 /* 표의 가장 긴 확장자("woff2" 5글자)보다 넉넉히. 더 길면 볼 것도 없이 모르는 확장자 */

typedef struct {
    const char *ext;  /* 소문자 확장자 (점 없이) */
    const char *type; /* Content-Type 값 */
} mime_entry;

/* 반드시 ext 의 strcmp 오름차순 (bsearch 조건). 테스트가 정렬 여부도 검사한다 */
static const mime_entry TABLE[] = {
    {"avif", "image/avif"},
    {"bmp", "image/bmp"},
    {"css", "text/css; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"gif", "image/gif"},
    {"gz", "application/gzip"},
    {"htm", "text/html; charset=utf-8"},
    {"html", "text/html; charset=utf-8"},
    {"ico", "image/x-icon"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"js", "text/javascript; charset=utf-8"},   /* RFC 9239: text/javascript 가 표준 */
    {"json", "application/json"},               /* JSON 은 항상 UTF-8 이라 charset 불필요 (RFC 8259) */
    {"map", "application/json"},                /* 소스맵 */
    {"md", "text/markdown; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},  /* ES 모듈 */
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"png", "image/png"},
    {"svg", "image/svg+xml"},
    {"tar", "application/x-tar"},
    {"ttf", "font/ttf"},
    {"txt", "text/plain; charset=utf-8"},
    {"wasm", "application/wasm"},               /* WebAssembly.instantiateStreaming 은 이 타입이어야만 동작 */
    {"wav", "audio/wav"},
    {"webm", "video/webm"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"xml", "application/xml"},
    {"zip", "application/zip"},
};

#ifdef MICROHTTP_TESTING
/* 테스트에서 표 정렬을 검사할 수 있도록 노출 (헤더에는 없음 — 테스트가 extern 으로 선언).
 * 테스트 빌드(Makefile 의 TEST_CFLAGS)에서만 생긴다 → 서버 실행 파일에는 이 심볼이 없음 */
const void *mime_table_for_test(size_t *count, size_t *entry_size);
const void *mime_table_for_test(size_t *count, size_t *entry_size)
{
    *count = sizeof(TABLE) / sizeof(TABLE[0]);
    *entry_size = sizeof(TABLE[0]);
    return TABLE;
}
#endif

static int cmp_entry(const void *key, const void *elem) /* bsearch 비교 함수: key 는 소문자 확장자 문자열 */
{
    return strcmp((const char *)key, ((const mime_entry *)elem)->ext);
}

const char *mime_type_for(const char *path)
{
    const char *slash = strrchr(path, '/');            /* 마지막 경로 구분자 (디렉터리 이름의 점은 무시하려고) */
    const char *base = slash ? slash + 1 : path;       /* 파일 이름 부분 */
    const char *dot = strrchr(base, '.');              /* 마지막 점 = 확장자 시작 ("a.tar.gz" → "gz") */
    if (dot == NULL || dot == base || dot[1] == '\0')  /* 점 없음 / ".bashrc" 같은 숨김 파일 / "a." */
        return MIME_DEFAULT;

    char ext[EXT_MAX + 1];                             /* 소문자로 바꾼 확장자 */
    size_t n = 0;
    for (const char *p = dot + 1; *p; p++) {           /* 점 다음부터 끝까지 */
        if (n == EXT_MAX)                              /* 너무 김 → 표에 있을 리 없음 */
            return MIME_DEFAULT;
        char c = *p;
        ext[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; /* ASCII 소문자화 (locale 영향 없게 직접) */
    }
    ext[n] = '\0';

    const mime_entry *e = bsearch(ext, TABLE, sizeof(TABLE) / sizeof(TABLE[0]), sizeof(TABLE[0]), cmp_entry);
    return e ? e->type : MIME_DEFAULT;
}
