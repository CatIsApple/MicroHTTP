/* static.c — L4: 정적 파일 서빙 */
#define _XOPEN_SOURCE 700 /* realpath 는 POSIX 의 XSI 확장이라 _POSIX_C_SOURCE 만으로는 선언되지 않는다 */

#include "static.h"
#include "mime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int path_is_under(const char *root, const char *resolved)
{
    size_t n = strlen(root);
    if (n > 0 && root[n - 1] == '/') /* root 가 "/" 인 경우 */
        n--;
    return strncmp(resolved, root, n) == 0 && (resolved[n] == '\0' || resolved[n] == '/');
}

/* errno → 상태 코드 */
static int status_from_errno(int err)
{
    if (err == ENOENT || err == ENOTDIR || err == ENAMETOOLONG)
        return 404;
    if (err == EACCES || err == EPERM || err == ELOOP)
        return 403;
    return 500;
}

void static_handle(const server_config *cfg, const http_request *req, http_response *res)
{
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        http_response_error(res, 405);
        http_response_header(res, "Allow", "GET, HEAD");
        return;
    }

    /* 1. 루트 + 요청 경로. path 는 파서가 디코드하고 ".." 를 이미 걸러 냈다 */
    char joined[PATH_MAX];
    if (snprintf(joined, sizeof joined, "%s%s", cfg->doc_root, req->path) >= (int)sizeof joined) {
        http_response_error(res, 414);
        return;
    }

    /* 2. realpath 로 심볼릭 링크까지 풀고, 결과가 루트 안에 있는지 다시 확인 (2차 방어선) */
    char real[PATH_MAX];
    if (!realpath(joined, real)) {
        http_response_error(res, status_from_errno(errno));
        return;
    }
    if (!path_is_under(cfg->doc_root, real)) {
        http_response_error(res, 403);
        return;
    }

    struct stat st;
    if (stat(real, &st) < 0) {
        http_response_error(res, status_from_errno(errno));
        return;
    }

    /* 3. 디렉터리 */
    if (S_ISDIR(st.st_mode)) {
        size_t plen = strlen(req->path);
        if (plen == 0 || req->path[plen - 1] != '/') {
            /* "/docs" → "/docs/" : 그래야 index.html 안의 상대 경로 링크가 맞게 풀린다 */
            /* Location 에는 디코드 전 원본(target)을 쓴다. 공백 같은 문자가 인코딩된 채로 남도록 */
            char location[PATH_MAX];
            size_t raw = strcspn(req->target, "?");
            snprintf(location, sizeof location, "%.*s/%s%s", (int)raw, req->target,
                     req->query ? "?" : "", req->query ? req->query : "");
            http_response_error(res, 301);
            if (http_response_header_copy(res, "Location", location) < 0)
                http_response_error(res, 414);
            return;
        }
        size_t rlen = strlen(real);
        if (rlen + sizeof "/index.html" > sizeof real) {
            http_response_error(res, 414);
            return;
        }
        memcpy(real + rlen, "/index.html", sizeof "/index.html");
        if (stat(real, &st) < 0) {
            http_response_error(res, errno == ENOENT ? 403 : status_from_errno(errno)); /* 목록 보기는 하지 않음 */
            return;
        }
    }
    if (!S_ISREG(st.st_mode)) { /* 장치 파일, 소켓 등 */
        http_response_error(res, 403);
        return;
    }

    /* 4. 열어서 본문으로. fd 는 응답을 보낸 뒤 http_response_free 가 닫는다 */
    int fd = open(real, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        http_response_error(res, status_from_errno(errno));
        return;
    }
    if (fstat(fd, &st) < 0) { /* stat 과 open 사이에 바뀌었을 수 있으니 연 파일 기준으로 다시 */
        close(fd);
        http_response_error(res, 500);
        return;
    }
    res->status = 200;
    http_response_body_file(res, fd, st.st_size, mime_from_path(real));
}
