/*
 * static_file.c — 정적 파일 찾기와 MIME 타입 매핑
 *
 * 요청 "/css/style.css" 가 들어오면
 *   1) 쿼리('?' 뒤)를 떼고 %XX 인코딩을 풀고        → "/css/style.css"
 *   2) ".." 같은 루트 탈출 시도를 거절하고           → 403
 *   3) 루트 경로와 이어 붙여 파일을 찾고              → "<root>/css/style.css"
 *   4) 심볼릭 링크를 따라간 실제 위치도 루트 안인지 확인
 *   5) 파일을 열어 크기와 MIME 타입을 알려줍니다.
 */
#include "static_file.h"
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *mime_type_for_path(const char *path)
{
    /*
     * 브라우저는 확장자가 아니라 Content-Type 헤더를 보고 내용을 해석합니다.
     * CSS를 text/plain으로 보내면 스타일이 적용되지 않고, JS는 실행되지 않습니다.
     */
    static const struct { const char *extension; const char *type; } table[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "json", "application/json" },
        { "txt",  "text/plain; charset=utf-8" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "svg",  "image/svg+xml" },
        { "ico",  "image/x-icon" },
        { "pdf",  "application/pdf" },
    };
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.'); /* 디렉터리 이름 속 '.'은 무시 */
    if (dot && dot[1]) {
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
            if (strcasecmp(dot + 1, table[i].extension) == 0) return table[i].type;
    }
    /* 모르는 형식: "그냥 바이트"로 알려서 브라우저가 멋대로 실행하지 않고 다운로드하게 함 */
    return "application/octet-stream";
}

/* 16진수 문자 하나의 값. 16진수가 아니면 -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * target에서 쿼리를 떼고 %XX를 풀어 decoded에 씁니다. (예: "/a%20b.txt?x=1" → "/a b.txt")
 * 반환값: 0 성공, 400 잘못된 인코딩
 */
static int decode_target_path(const char *target, char *decoded, size_t capacity)
{
    size_t length = 0;
    for (const char *p = target; *p && *p != '?'; ++p) {
        char c = *p;
        if (c == '%') {
            int high = hex_value(p[1]);
            int low = high < 0 ? -1 : hex_value(p[2]);
            if (low < 0) return 400;
            c = (char)(high * 16 + low);
            if (c == '\0') return 400; /* "%00"은 C 문자열을 중간에서 끊어 검사를 우회할 수 있음 */
            p += 2;
        }
        if (length + 1 >= capacity) return 400;
        decoded[length++] = c;
    }
    decoded[length] = '\0';
    return 0;
}

/* 경로 조각 중 ".."이 있는지 검사합니다. "/a/../../etc/passwd" 같은 루트 탈출 시도를 막습니다. */
static int has_parent_segment(const char *path)
{
    for (const char *segment = path; *segment; ) {
        size_t length = strcspn(segment, "/");
        if (length == 2 && segment[0] == '.' && segment[1] == '.') return 1;
        segment += length;
        if (*segment == '/') ++segment;
    }
    return 0;
}

/* stat/open 실패의 errno를 HTTP 상태 코드로 바꿉니다. */
static int status_from_errno(int error)
{
    if (error == ENOENT || error == ENOTDIR) return 404;
    if (error == EACCES) return 403;
    /* 파일 이름 한 조각이 NAME_MAX(보통 255)를 넘는 등 요청이 만든 문제 → 서버 오류(5xx)가 아님 */
    if (error == ENAMETOOLONG) return 414;
    return 500; /* 예: 심볼릭 링크 순환(ELOOP), 디스크 오류(EIO) → 서버 쪽 문제 */
}

int static_file_open(const char *root, const char *target,
                     int *file_fd, size_t *file_size, const char **mime_type)
{
    char decoded[HTTP_MAX_TARGET + 1];
    int status = decode_target_path(target, decoded, sizeof(decoded));
    if (status) return status;
    if (has_parent_segment(decoded)) return 403;

    /* 루트 + 요청 경로 */
    size_t root_length = strlen(root);
    char path[4096 + HTTP_MAX_TARGET + sizeof("/index.html")];
    int written = snprintf(path, sizeof(path), "%s%s", root, decoded);
    if (written < 0 || (size_t)written + sizeof("/index.html") > sizeof(path)) return 500;

    struct stat info;
    if (stat(path, &info) == -1) return status_from_errno(errno);
    if (S_ISDIR(info.st_mode)) {
        /* 디렉터리를 요청하면("/", "/docs/", "/docs") 그 안의 기본 문서 index.html을 보냅니다. */
        strcat(path, path[written - 1] == '/' ? "index.html" : "/index.html");
        if (stat(path, &info) == -1)
            return errno == ENOENT ? 403 : status_from_errno(errno); /* 디렉터리 목록은 보여주지 않음 */
    }
    if (!S_ISREG(info.st_mode)) return 403; /* 장치 파일, 소켓 등은 보내지 않음 */

    /*
     * ".."을 막아도 루트 안의 심볼릭 링크가 루트 밖(/etc 등)을 가리킬 수 있습니다.
     * realpath로 링크를 모두 따라간 "진짜 경로"를 구해 루트 안에 있는지 다시 확인합니다.
     * realpath(path, NULL)은 결과를 malloc으로 만들어 주므로 반드시 free 합니다.
     */
    char *real_path = realpath(path, NULL);
    if (!real_path) return status_from_errno(errno);
    int inside_root = strncmp(real_path, root, root_length) == 0 &&
                      (real_path[root_length] == '/' || real_path[root_length] == '\0');
    if (!inside_root) { free(real_path); return 403; }

    int fd = open(real_path, O_RDONLY);
    int open_error = errno;
    *mime_type = mime_type_for_path(real_path);
    free(real_path);
    if (fd == -1) return status_from_errno(open_error);
    /* 크기는 열린 파일 기준으로 다시 확인합니다 (stat과 open 사이에 파일이 바뀌었을 수 있음). */
    if (fstat(fd, &info) == -1 || !S_ISREG(info.st_mode)) { close(fd); return 500; }
    *file_fd = fd;
    *file_size = (size_t)info.st_size;
    return 200;
}
