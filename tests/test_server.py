"""축구 경기 HTTP 요청을 실제 TCP 연결로 검증한다. 외부 서비스는 사용하지 않는다."""
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


# 서버가 연결을 닫을 때까지 받은 바이트를 모두 모아 돌려준다.
# (서버는 응답 하나 후 항상 연결을 닫으므로, EOF가 곧 응답의 끝이다.)
def receive_all(client):
    output = bytearray()
    while True:
        try:
            part = client.recv(8192)
        except ConnectionResetError:
            # 거부된 요청의 미수신 본문이 남으면 OS가 FIN 대신 RST로 종료할 수 있다.
            # 이미 수신한 HTTP 응답의 완전성은 아래에서 별도로 검사한다.
            return bytes(output)
        if not part:
            return bytes(output)
        output.extend(part)


# 원시 HTTP 응답 바이트를 (상태 코드, 헤더 dict, 본문)으로 나눈다.
# 동시에 응답 형식 자체도 검사한다: HTTP/1.1, 헤더 중복 없음, Connection: close,
# 본문 길이가 Content-Length와 일치(HEAD 응답이면 본문 없음).
def decode_response(raw, head=False):
    header, body = raw.split(b'\r\n\r\n', 1)
    lines = header.split(b'\r\n')
    version, status, _ = lines[0].decode('ascii').split(' ', 2)
    assert version == 'HTTP/1.1'
    headers = {}
    for line in lines[1:]:
        name, value = line.split(b':', 1)
        name = name.lower().decode('ascii')
        assert name not in headers
        headers[name] = value.strip().decode('ascii')
    assert headers['connection'] == 'close'
    assert len(body) == (0 if head else int(headers['content-length']))
    return int(status), headers, body


# 테스트용 HTTP 요청 바이트를 만든다. body를 주면 Content-Length를 자동으로 붙인다.
def request(method='GET', path='/matches/1', body=None, headers=()):
    data = f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'.encode()
    if body is not None:
        data += f'Content-Length: {len(body)}\r\n'.encode()
    for name, value in headers:
        data += f'{name}: {value}\r\n'.encode()
    return data + b'\r\n' + (body if body is not None else b'')


class FootballHttpTest(unittest.TestCase):
    # 테스트 시작 전에 서버를 한 번 띄운다.
    # 빈 포트를 OS에게 받아 쓰고, 접속이 될 때까지 최대 3초 기다린다.
    # 서버 로그(stderr)는 임시 파일에 모아 두었다가 실패 진단과 로그 검사에 쓴다.
    @classmethod
    def setUpClass(cls):
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            cls.port = reservation.getsockname()[1]
        cls.log = tempfile.TemporaryFile(mode='w+b')
        cls.server = subprocess.Popen([str(ROOT / 'server'), str(cls.port)], stderr=cls.log)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if cls.server.poll() is not None:
                break
            try:
                with cls.connect():
                    return
            except OSError:
                time.sleep(.02)
        cls.server.terminate()
        cls.server.wait(timeout=2)
        cls.log.seek(0)
        diagnostics = cls.log.read().decode(errors='replace')
        cls.log.close()
        raise AssertionError('startup failed: ' + diagnostics)

    # 모든 테스트가 끝나면 서버를 종료하고, SIGTERM으로 정상 종료됐는지와
    # 메모리 오류 검사기(Sanitizer) 경고가 없는지 확인한다.
    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(timeout=2)
        cls.log.seek(0)
        diagnostics = cls.log.read().decode(errors='replace')
        cls.log.close()
        assert cls.server.returncode == -signal.SIGTERM, diagnostics
        assert 'Sanitizer' not in diagnostics and 'runtime error:' not in diagnostics, diagnostics

    # 테스트 서버에 새 TCP 연결을 연다.
    @classmethod
    def connect(cls):
        return socket.create_connection(('127.0.0.1', cls.port), timeout=10)

    # 요청 하나를 보내고 응답을 받아 해석한다.
    # eof=True면 보낸 뒤 쓰기 방향을 닫아 "더 보낼 것 없음"을 서버에 알린다.
    @classmethod
    def exchange(cls, data, eof=False, head=False):
        with cls.connect() as client:
            client.sendall(data)
            if eof:
                client.shutdown(socket.SHUT_WR)
            return decode_response(receive_all(client), head=head)

    # 경기 화면 HTML이 /, /matches/1, 쿼리 문자열이 붙은 경로에서 모두 나오는지
    def test_01_match_page_and_home(self):
        for path in ('/', '/matches/1', '/matches/1?view=main'):
            with self.subTest(path=path):
                status, headers, body = self.exchange(request(path=path))
                self.assertEqual(status, 200)
                self.assertEqual(headers['content-type'], 'text/html; charset=utf-8')
                self.assertIn('학습용 가상 경기', body.decode())
                self.assertIn(b'Home FC', body)
                self.assertIn(b'Away FC', body)
                self.assertIn(b"fetchTimed('/api/matches/1')", body)

    # 경기 정보 JSON의 형식과 내용
    def test_02_match_json(self):
        status, headers, body = self.exchange(request(path='/api/matches/1'))
        self.assertEqual(status, 200)
        self.assertEqual(headers['content-type'], 'application/json; charset=utf-8')
        self.assertEqual(json.loads(body), dict(id=1, home_team='Home FC', away_team='Away FC',
                                               status='scheduled', is_demo=True))

    # 없는 경로는 404
    def test_03_unknown_routes(self):
        for path in ('/matches/2', '/api/matches/2/messages', '/favicon.ico', '/matches/10'):
            with self.subTest(path=path):
                self.assertEqual(self.exchange(request(path=path))[0], 404)

    # 읽기 전용 화면에 GET 외 메서드는 405 + Allow 헤더. HEAD 응답에는 본문이 없어야 함
    def test_04_method_policy_and_head(self):
        for method in ('POST', 'PUT', 'DELETE'):
            with self.subTest(method=method):
                status, headers, _ = self.exchange(request(method=method, body=b''))
                self.assertEqual(status, 405)
                self.assertEqual(headers['allow'], 'GET')
        status, headers, body = self.exchange(request(method='HEAD'), head=True)
        self.assertEqual(status, 405)
        self.assertEqual(body, b'')
        self.assertGreater(int(headers['content-length']), 0)

    # 요청을 1바이트씩 쪼개 보내도, 마지막 바이트 전까지는 응답하지 않고 기다리는지
    def test_05_header_fragmented_byte_by_byte(self):
        with self.connect() as client:
            data = request()
            for byte in data[:-1]:
                client.sendall(bytes([byte]))
            client.settimeout(.15)
            with self.assertRaises(TimeoutError):
                client.recv(1)
            client.sendall(data[-1:])
            client.settimeout(3)
            self.assertEqual(decode_response(receive_all(client))[0], 200)

    # UTF-8 본문이 다 도착할 때까지 응답을 미루는지
    def test_06_post_utf8_waits_for_complete_body(self):
        # '골!!!'은 6바이트. 성공적인 채팅 등록 대신 아직 읽기 전용인 경기 자원에 405.
        payload = '골!!!'.encode()
        self.assertEqual(len(payload), 6)
        data = request(method='POST', body=payload)
        with self.connect() as client:
            client.sendall(data[:-2])
            client.settimeout(.15)
            with self.assertRaises(TimeoutError):
                client.recv(1)
            client.sendall(data[-2:])
            client.settimeout(3)
            self.assertEqual(decode_response(receive_all(client))[0], 405)

    # NUL/0xFF가 섞인 바이너리 본문과 최대 크기(64 KiB) 본문도 안전하게 처리하는지
    def test_07_binary_and_maximum_body(self):
        for body in (b'A\x00B\xff', bytes(range(256)) * 256):
            with self.subTest(length=len(body)):
                self.assertEqual(self.exchange(request(method='POST', body=body))[0], 405)

    # Host 누락·중복·공백, LF만 쓴 줄바꿈 등 잘못된 요청은 400
    def test_08_invalid_requests_and_host(self):
        bad = (
            b'GET / HTTP/1.1\r\n\r\n',
            b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n',
            b'GET / HTTP/1.1\r\nHost: a b\r\n\r\n',
            b'GET / HTTP/1.1\nHost: a\n\n',
            b'GET / HTTP/1.1\r\nHost : a\r\n\r\n',
            b'GET / HTTP/1.1\r\nHost: a\x00b\r\n\r\n',
            b'GET / BAD\r\nHost: a\r\n\r\n',
        )
        for data in bad:
            with self.subTest(data=data):
                self.assertEqual(self.exchange(data)[0], 400)

    # Content-Length 형식 오류, Content-Length와 Transfer-Encoding 동시 사용은 400
    def test_09_invalid_body_framing(self):
        for value in ('-1', '+6', '6x', '6, 6', ''):
            with self.subTest(value=value):
                data = request(method='POST', headers=(('Content-Length', value),))
                self.assertEqual(self.exchange(data)[0], 400)
        data = request(method='POST', body=b'', headers=(('Content-Length', '0'),))
        self.assertEqual(self.exchange(data)[0], 400)
        data = request(method='POST', body=b'', headers=(('Transfer-Encoding', 'chunked'),))
        self.assertEqual(self.exchange(data)[0], 400)

    # 411(길이 없음), 501(chunked), 417(Expect), 505(HTTP/1.0)
    def test_10_missing_length_and_unsupported_features(self):
        self.assertEqual(self.exchange(request(method='POST'))[0], 411)
        self.assertEqual(self.exchange(request(method='POST', headers=(('Transfer-Encoding', 'chunked'),)))[0], 501)
        data = request(method='POST', body=b'', headers=(('Expect', '100-continue'),))
        self.assertEqual(self.exchange(data)[0], 417)
        data = b'GET /matches/1 HTTP/1.0\r\nHost: a\r\n\r\n'
        self.assertEqual(self.exchange(data)[0], 505)

    # 본문(413), 경로(414), 헤더(431) 크기 상한
    def test_11_size_limits(self):
        for value in ('65537', '9999999999999999999999999999999999'):
            self.assertEqual(self.exchange(request(method='POST', headers=(('Content-Length', value),)))[0], 413)
        self.assertEqual(self.exchange(request(path='/' + 'x' * 2048))[0], 414)
        self.assertEqual(self.exchange(b'GET / HTTP/1.1\r\nHost: a\r\nX: ' + b'x' * 16384)[0], 431)

    # 요청을 보내다 말고 연결을 닫으면 400, 완전한 요청 뒤 닫으면 정상 처리
    def test_12_incomplete_body_and_headers_eof(self):
        for data in (b'GET /matches', b'POST /matches/1 HTTP/1.1\r\nHost: a\r\nContent-Length: 6\r\n\r\nabc'):
            with self.subTest(data=data):
                self.assertEqual(self.exchange(data, eof=True)[0], 400)
        self.assertEqual(self.exchange(request(), eof=True)[0], 200)

    # keep-alive를 요청해도 응답 하나 후 연결을 닫는지
    def test_13_keep_alive_is_explicitly_closed(self):
        status, headers, _ = self.exchange(request(headers=(('Connection', 'keep-alive'),)))
        self.assertEqual(status, 200)
        self.assertEqual(headers['connection'], 'close')
        # 연결당 한 응답만 처리한다. consumed 이후의 다음 요청을 실행하지 않는다.
        raw = request() + request(path='/api/matches/1')
        self.assertEqual(self.exchange(raw)[0], 200)

    # 순차 서버라서, 느린 첫 클라이언트가 끝나기 전엔 두 번째 클라이언트가 기다리는지
    def test_14_slow_client_blocks_next_until_request_completes(self):
        with self.connect() as first:
            first.sendall(b'GET /matches/1 HTTP/1.1\r\nHost: a\r\n')
            # first가 수락되기 전 second가 먼저 처리되는 경합을 피하기 위해 짧게 대기.
            time.sleep(.05)
            with self.connect() as second:
                second.sendall(request(path='/api/matches/1'))
                second.settimeout(.15)
                with self.assertRaises(TimeoutError):
                    second.recv(1)
                first.sendall(b'\r\n')
                self.assertEqual(decode_response(receive_all(first))[0], 200)
                second.settimeout(3)
                self.assertEqual(decode_response(receive_all(second))[0], 200)

    # 멈춘 클라이언트는 타임아웃(약 5초) 후 408을 받고, 다음 클라이언트가 처리되는지
    def test_15_idle_timeout_releases_next_client(self):
        with self.connect() as first:
            first.sendall(b'GET /matches/1 HTTP/1.1\r\nHost: a\r\n')
            time.sleep(.05)
            with self.connect() as second:
                second.sendall(request())
                started = time.monotonic()
                self.assertEqual(decode_response(receive_all(first))[0], 408)
                self.assertGreater(time.monotonic() - started, 4)
                self.assertEqual(decode_response(receive_all(second))[0], 200)

    # 클라이언트 100개가 동시에 접속해도 모두 정상 응답을 받는지
    def test_16_one_hundred_clients(self):
        def fetch(number):
            status, _, body = self.exchange(request(path='/api/matches/1'))
            return number, status, json.loads(body)['id']
        with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
            results = list(pool.map(fetch, range(100)))
        self.assertEqual(len(results), 100)
        self.assertTrue(all(status == 200 and match_id == 1 for _, status, match_id in results))

    # 아무것도 안 보내고 닫은 연결, RST로 강제 종료된 연결 뒤에도 서버가 살아 있는지
    def test_17_reset_and_empty_connection(self):
        with self.connect():
            pass
        with self.connect() as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            client.sendall(request())
        self.assertEqual(self.exchange(request())[0], 200)

    # 이미 쓰는 포트, 잘못된 포트 인자로 실행하면 종료 코드 1로 실패하는지
    def test_18_configuration_errors(self):
        duplicate = subprocess.run([str(ROOT / 'server'), str(self.port)], capture_output=True, timeout=2)
        self.assertEqual(duplicate.returncode, 1)
        self.assertIn(b'bind:', duplicate.stderr)
        for value in ('0', '65536', '-1', 'wrong', ''):
            self.assertEqual(subprocess.run([str(ROOT / 'server'), value], capture_output=True, timeout=2).returncode, 1)


    # 채팅 메시지 POST 도우미
    def post_message(self, text, content_type='text/plain; charset=utf-8'):
        body = text.encode() if isinstance(text, str) else text
        return self.exchange(request(method='POST', path='/api/matches/1/messages', body=body,
                                     headers=(('Content-Type', content_type),)))

    # 채팅 목록 GET 도우미. 캐시 금지 헤더도 함께 확인한다.
    def messages(self):
        status, headers, body = self.exchange(request(path='/api/matches/1/messages'))
        self.assertEqual(status, 200)
        self.assertEqual(headers['cache-control'], 'no-store')
        return json.loads(body)['messages']

    # 한 연결에서 POST한 메시지가 다른 연결의 GET에서 보이는지
    def test_19_chat_post_then_other_client_get(self):
        text = '골!!! 다른 브라우저에서도 보이는 메시지 ⚽'
        status, headers, body = self.post_message(text)
        self.assertEqual(status, 201)
        self.assertEqual(headers['content-type'], 'application/json; charset=utf-8')
        created = json.loads(body)
        self.assertEqual(created['text'], text)
        self.assertGreater(created['created_at_ms'], 0)
        # GET은 별도 TCP 연결. 저장된 본문이 송신 버퍼에서 분리되어 보존되는지 확인한다.
        self.assertIn(created, self.messages())

    # 잘못된 텍스트는 400이고, 저장소에는 아무것도 추가되지 않는지
    def test_20_chat_rejects_invalid_text_without_storage(self):
        before = self.messages()
        for body in (b'', b' \t\r\n', b'a\x00b', b'\xff', b'\xc0\xaf',
                     b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'a\x01'):
            with self.subTest(body=body):
                status, _, result = self.post_message(body)
                self.assertEqual(status, 400)
                self.assertIn('error', json.loads(result))
        self.assertEqual(self.messages(), before)

    # 512바이트 경계, 그리고 따옴표·역슬래시·HTML 등이 JSON에서 올바르게 escape되는지
    def test_21_chat_byte_limit_and_json_escaping(self):
        self.assertEqual(self.post_message('골' * 170 + '!!')[0], 201)  # 512바이트
        self.assertEqual(self.post_message('골' * 171)[0], 413)
        text = '\" \\ \t \r\n <img src=x onerror=alert(1)> 골!!!'
        status, _, body = self.post_message(text)
        self.assertEqual(status, 201)
        self.assertEqual(json.loads(body)['text'], text)
        self.assertIn(json.loads(body), self.messages())

    # Content-Type 검사(415), 중복 헤더(400), 허용 메서드(405)
    def test_22_chat_media_type_and_method_contract(self):
        before = self.messages()
        for kind in ('application/json', 'text/html', 'text/plain; charset=iso-8859-1', 'text/ plain'):
            self.assertEqual(self.post_message('hello', kind)[0], 415)
        self.assertEqual(self.messages(), before)
        self.assertEqual(self.post_message('hello', '')[0], 400)
        self.assertEqual(self.exchange(request(method='POST', path='/api/matches/1/messages', body=b'hello'))[0], 415)
        self.assertEqual(self.post_message('대소문자와 공백', 'Text/Plain; Charset=UTF-8')[0], 201)
        status, headers, _ = self.exchange(request(method='DELETE', path='/api/matches/1/messages'))
        self.assertEqual(status, 405)
        self.assertEqual(headers['allow'], 'GET, POST')
        duplicate = request(method='POST', path='/api/matches/1/messages', body=b'x',
                            headers=(('Content-Type', 'text/plain'), ('Content-Type', 'text/plain')))
        self.assertEqual(self.exchange(duplicate)[0], 400)

    # 채팅 POST를 3바이트씩 쪼개 보내도 정상 저장되는지
    def test_23_fragmented_chat_post(self):
        text = '나눠서 전송한 골!!!'
        raw = request(method='POST', path='/api/matches/1/messages', body=text.encode(),
                      headers=(('Content-Type', 'text/plain; charset=utf-8'),))
        with self.connect() as client:
            for i in range(0, len(raw), 3):
                client.sendall(raw[i:i+3])
            status, _, body = decode_response(receive_all(client))
        self.assertEqual(status, 201)
        self.assertEqual(json.loads(body)['text'], text)
        self.assertIn(json.loads(body), self.messages())

    # 105개를 보내면 최근 100개만 순서대로 남는지 (링 버퍼)
    def test_24_recent_one_hundred_retained_in_order(self):
        sent = []
        for i in range(105):
            status, _, body = self.post_message(f'보관 한도 확인 {i}')
            self.assertEqual(status, 201)
            sent.append(json.loads(body))
        self.assertEqual(self.messages(), sent[-100:])

    # 동시에 여러 클라이언트가 POST해도 메시지가 섞이거나 id가 겹치지 않는지
    def test_25_concurrent_clients_do_not_mix_messages(self):
        def send(i):
            text = f'관람자-{i}: 골!!!'
            status, _, body = self.post_message(text)
            return status, text, json.loads(body)
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            results = list(pool.map(send, range(40)))
        self.assertTrue(all(status == 201 and text == result['text'] for status, text, result in results))
        self.assertEqual(len({result['id'] for _, _, result in results}), 40)
        stored = self.messages()
        for _, _, result in results:
            self.assertIn(result, stored)

    # 서버 로그에 단계별 소요 시간 필드가 찍히는지
    def test_26_logs_separate_phases(self):
        self.assertEqual(self.exchange(request())[0], 200)
        # pread는 파일 커서를 움직이지 않아 자식의 로그 쓰기를 방해하지 않는다.
        logs = os.pread(self.log.fileno(), os.fstat(self.log.fileno()).st_size, 0).decode()
        self.assertIn('[chat] stored_id=', logs)
        for label in ('request_id=', 'target=/api/matches/1/messages',
                      'receive_ms=', 'route_ms=', 'send_ms=', 'total_ms=', 'send_ok=1'):
            self.assertIn(label, logs)


    # 서버를 재시작하면 메모리 저장소가 비워지는지 (별도 포트로 서버를 두 번 실행)
    def test_27_restart_clears_memory_store(self):
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        def exchange_local(data):
            with socket.create_connection(('127.0.0.1', port), timeout=3) as client:
                client.sendall(data)
                return decode_response(receive_all(client))
        with tempfile.TemporaryFile() as log:
            for round_number in range(2):
                process = subprocess.Popen([str(ROOT / 'server'), str(port)], stderr=log)
                try:
                    deadline = time.monotonic() + 3
                    while True:
                        try:
                            status, _, body = exchange_local(request(path='/api/matches/1/messages'))
                            break
                        except OSError:
                            if time.monotonic() >= deadline or process.poll() is not None:
                                raise
                            time.sleep(.02)
                    self.assertEqual(status, 200)
                    self.assertEqual(json.loads(body)['messages'], [])
                    if round_number == 0:
                        raw = request(method='POST', path='/api/matches/1/messages', body=b'before restart',
                                      headers=(('Content-Type','text/plain'),))
                        self.assertEqual(exchange_local(raw)[0], 201)
                finally:
                    process.terminate()
                    process.wait(timeout=3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
