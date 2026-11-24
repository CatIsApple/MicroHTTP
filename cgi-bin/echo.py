#!/usr/bin/env python3
"""POST 예제: 표준 입력으로 받은 본문을 그대로 돌려준다."""
import os
import sys

length = int(os.environ.get("CONTENT_LENGTH") or 0)
body = sys.stdin.buffer.read(length) if length else b""

out = sys.stdout.buffer
out.write(b"Content-Type: text/plain; charset=utf-8\r\n")
out.write(b"X-Body-Length: %d\r\n\r\n" % len(body))
out.write(b"method=" + os.environ.get("REQUEST_METHOD", "").encode() + b"\n")
out.write(b"content_type=" + os.environ.get("CONTENT_TYPE", "").encode() + b"\n")
out.write(b"body=" + body)
