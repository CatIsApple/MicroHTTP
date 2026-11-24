/*
 * static_file.h — 정적 파일 찾기와 MIME 타입 매핑
 *
 * 이 모듈은 파일 시스템만 다루고 소켓은 다루지 않습니다.
 * 찾은 파일을 실제로 보내는 일은 server.c의 serve_static_file()이 합니다.
 */
#ifndef MICROHTTP_STATIC_FILE_H
#define MICROHTTP_STATIC_FILE_H

#include <stddef.h>

/* 파일 이름의 확장자로 Content-Type을 정합니다. 모르는 확장자는 application/octet-stream. */
const char *mime_type_for_path(const char *path);

/*
 * 요청 경로(target)에 해당하는 파일을 루트 디렉터리(root) 안에서 찾아 엽니다.
 *   root : realpath()로 정규화된 절대 경로 (예: "/home/me/MicroHTTP/www")
 * 반환값: HTTP 상태 코드
 *   200 → *file_fd(열린 파일), *file_size, *mime_type이 채워짐. 호출자가 close 해야 함
 *   400 → 잘못된 퍼센트 인코딩(%zz, %00)
 *   403 → 루트 밖으로 나가는 경로("..", 심볼릭 링크), 읽기 권한 없음, 일반 파일 아님
 *   404 → 파일 없음
 *   500 → 그 밖의 시스템 오류
 */
int static_file_open(const char *root, const char *target,
                     int *file_fd, size_t *file_size, const char **mime_type);

#endif
