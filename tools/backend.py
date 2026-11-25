#!/usr/bin/env python3
"""Reverse Proxy 실습용 백엔드 서버 (Python 표준 라이브러리).

MicroHTTP는 C로 구현하고, 이 파일은 "프록시 뒤에 있는 애플리케이션 서버" 역할만 합니다.

    python3 tools/backend.py 9001 A
    python3 tools/backend.py 9002 B
    ./server -R /api/=127.0.0.1:9001,127.0.0.1:9002

응답(JSON): 어느 백엔드가 처리했는지, 받은 메서드/경로/헤더/본문
특별 경로: /api/slow   → 10초 뒤 응답 (프록시 504 타임아웃 확인용)
           /api/linger → 응답을 다 보낸 뒤 1초 있다가 연결을 닫음 (클라이언트가 먼저 닫는 경우 확인용)
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    name = "backend"

    def respond(self):
        if self.path.startswith("/api/slow"):
            time.sleep(10)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        payload = json.dumps({
            "backend": self.name,
            "method": self.command,
            "path": self.path,
            "headers": {k.lower(): v for k, v in self.headers.items()},
            "body": body.decode("utf-8", errors="replace"),
            "body_bytes": len(body),
        }, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        if self.path.startswith("/api/linger"):
            self.wfile.flush()
            time.sleep(1)

    do_GET = do_POST = do_PUT = do_DELETE = respond

    def log_message(self, fmt, *args):
        sys.stderr.write(f"[backend {self.name}] {fmt % args}\n")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
    Handler.name = sys.argv[2] if len(sys.argv) > 2 else str(port)
    # 기본 listen 대기열(5)은 동시 요청 10개에서도 넘쳐 연결이 끊깁니다. 시험 도구가 병목이 되지 않도록
    # MicroHTTP의 BACKLOG와 같은 128로 맞춥니다.
    ThreadingHTTPServer.request_queue_size = 128
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"backend {Handler.name} on 127.0.0.1:{port}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
