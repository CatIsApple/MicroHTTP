
## Session ID(Cookie)로 식별 — ./server
서버 로그의 연결 fd 번호: 5 (세 연결이 같은 번호를 재사용했는지 확인)
| 단계 | 상태 | 응답 본문 |
|---|---:|---|
| 1) A 로그인 (새 연결) | 303 | logged in |
| 2) B: 자격 증명 없이 /me (새 연결) | 401 | not logged in |
| 3) A: 새 연결로 /me (Cookie 포함) | 200 | user: alice |

## 연결 fd로 식별 — build/server_fd_demo
서버 로그의 연결 fd 번호: 5 (세 연결이 같은 번호를 재사용했는지 확인)
| 단계 | 상태 | 응답 본문 |
|---|---:|---|
| 1) A 로그인 (새 연결) | 200 | logged in (fd demo) |
| 2) B: 자격 증명 없이 /me (새 연결) | 200 | user: alice (identified by fd 5) |
| 3) A: 새 연결로 /me | 200 | user: alice (identified by fd 5) |
