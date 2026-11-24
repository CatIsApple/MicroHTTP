/* router.c — L5: { 경로 접두사, 핸들러 } 표를 위에서부터 보고 고른다.
 * 새 동적 핸들러(예: C 로 만든 /api/...)는 표에 한 줄 추가하면 된다. */
#include "router.h"
#include "cgi.h"
#include "static.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *prefix; /* NULL 이면 cfg->cgi_prefix 를 쓴다 (설정으로 바뀌는 경로) */
    int needs_cgi;      /* cgi_dir 이 설정돼 있을 때만 쓰는 항목 */
    http_handler handler;
} route;

static const route ROUTES[] = {
    {NULL, 1, cgi_handle},
};

static int starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

void router_dispatch(const server_config *cfg, const http_request *req, http_response *res)
{
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
        const char *prefix = ROUTES[i].prefix ? ROUTES[i].prefix : cfg->cgi_prefix;
        if (!prefix || (ROUTES[i].needs_cgi && !cfg->cgi_dir))
            continue;
        if (starts_with(req->path, prefix)) {
            ROUTES[i].handler(cfg, req, res);
            return;
        }
    }
    static_handle(cfg, req, res);
}
