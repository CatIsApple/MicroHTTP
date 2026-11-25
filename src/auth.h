/*
 * auth.h — 로그인 / 로그아웃 / 인증이 필요한 페이지
 *
 *   POST /login   (username=..&password=..)  → 303 + Set-Cookie: sid=...   실패 401
 *   POST /logout                              → 서버 측 Session 삭제 + Cookie 지우기
 *   GET  /me                                  → 로그인했으면 200 "user: 이름", 아니면 401
 *
 * 사용자 목록은 시작할 때 파일에서 읽습니다 (한 줄에 "이름:argon2id 해시").
 * 해시는 `build/hashpw`로 만듭니다. 비밀번호 해싱은 직접 구현하지 않고 libsodium을 씁니다.
 */
#ifndef MICROHTTP_AUTH_H
#define MICROHTTP_AUTH_H

#include "http.h"

/* libsodium 초기화 + 사용자 파일 읽기(path가 NULL이면 사용자 없음). 성공 0, 실패 -1. 스레드 시작 전에 호출 */
int auth_init(const char *users_path);

/*
 * 인증 경로(/login, /logout, /me)면 처리하고 보낸 상태 코드를 돌려줍니다(송신 실패 -1).
 * 인증 경로가 아니면 0을 돌려주고 아무것도 보내지 않습니다.
 */
int auth_handle_request(int client_fd, const HttpRequest *request, int keep_alive);

#endif
