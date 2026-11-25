"""STEP 1 실험: 사용자를 TCP 연결(fd)로 식별하면 왜 틀리는가

    make server build/server_fd_demo && python3 tests/experiments/step1_fd_reuse.py

같은 시나리오를 두 서버에 실행합니다.
  - ./server              : Cookie의 Session ID로 식별 (올바른 설계)
  - build/server_fd_demo  : 로그인한 연결의 fd 번호로 식별 (잘못된 설계, AUTH_FD_DEMO)
시나리오 (요청마다 새 TCP 연결, Connection: close):
  1) A가 로그인
  2) 자격 증명이 전혀 없는 B가 /me
  3) A가 새 연결로 /me (Cookie 방식은 Cookie를 들고 감)
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
USERS = os.path.join(ROOT, "tests", "functional", "users.txt")


def send(port, raw):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(raw)
        data = b""
        while chunk := sock.recv(65536):
            data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1])
    cookie = re.search(rb"sid=([0-9a-f]{64})", head)
    return status, body.decode().strip(), cookie.group(1).decode() if cookie else None


def scenario(binary, port):
    server = subprocess.Popen([binary, "-p", str(port), "-r", os.path.join(ROOT, "www"), "-U", USERS],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(0.5)
    body = b"username=alice&password=alice-test-pw"
    login = (b"POST /login HTTP/1.1\r\nHost: a\r\nConnection: close\r\nContent-Length: "
             + str(len(body)).encode() + b"\r\n\r\n" + body)
    rows = []
    status, text, sid = send(port, login)
    rows.append(("1) A 로그인 (새 연결)", status, text))
    status, text, _ = send(port, b"GET /me HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
    rows.append(("2) B: 자격 증명 없이 /me (새 연결)", status, text))
    cookie = f"Cookie: sid={sid}\r\n".encode() if sid else b""
    status, text, _ = send(port, b"GET /me HTTP/1.1\r\nHost: a\r\nConnection: close\r\n" + cookie + b"\r\n")
    rows.append(("3) A: 새 연결로 /me" + (" (Cookie 포함)" if sid else ""), status, text))
    server.terminate()
    log = server.communicate(timeout=10)[1].decode()
    fds = sorted(set(re.findall(r"\[request\] fd=(\d+)", log)))
    return rows, fds


for binary, title in (("./server", "Session ID(Cookie)로 식별 — ./server"),
                      ("build/server_fd_demo", "연결 fd로 식별 — build/server_fd_demo")):
    rows, fds = scenario(os.path.join(ROOT, binary), 18950)
    print(f"\n## {title}")
    print(f"서버 로그의 연결 fd 번호: {', '.join(fds)} (세 연결이 같은 번호를 재사용했는지 확인)")
    print("| 단계 | 상태 | 응답 본문 |\n|---|---:|---|")
    for step, status, text in rows:
        print(f"| {step} | {status} | {text} |")
    time.sleep(0.5)
