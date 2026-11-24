/* mime.h — 파일 이름(확장자) → Content-Type 매핑의 "목차"(선언부). 구현은 src/mime.c.
 * 소켓·파일과 무관한 순수 함수라 단위 테스트가 쉽다. */

#ifndef MIME_H /* 인클루드 가드 */
#define MIME_H

#define MIME_DEFAULT "application/octet-stream" /* 모르는 확장자: "그냥 바이트" → 브라우저는 다운로드로 처리 */

/* path 의 마지막 확장자로 Content-Type 을 고른다. 대소문자 무시 ("A.HTML" == "a.html").
 * 텍스트 계열은 "; charset=utf-8" 까지 붙여서 돌려준다 (한글 깨짐 방지).
 * 확장자가 없거나 ("Makefile"), 디렉터리 이름에만 점이 있거나 ("a.d/file"), 모르는 확장자면 MIME_DEFAULT.
 * 반환 문자열은 정적 상수 (free 금지, 항상 NULL 아님). */
const char *mime_type_for(const char *path);

#endif /* MIME_H */
