/* mime.h — L4: 파일 확장자 → Content-Type */
#ifndef MICROHTTP_MIME_H
#define MICROHTTP_MIME_H

/* "/img/logo.PNG" → "image/png". 확장자는 대소문자 무시, 모르면 "application/octet-stream" */
const char *mime_from_path(const char *path);

#endif
