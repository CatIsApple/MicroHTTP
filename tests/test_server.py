"""실제 TCP 연결로 최소 서버를 검증한다. 서버와 별도 포트에서 실행한다."""
import concurrent.futures
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def receive_all(client):
    result = bytearray()
    while True:
        chunk = client.recv(4096)
        if not chunk:
            return bytes(result)
        result.extend(chunk)


def receive_exact(client, length):
    result = bytearray()
    while len(result) < length:
        chunk = client.recv(length - len(result))
        if not chunk:
            raise AssertionError('unexpected EOF')
        result.extend(chunk)
    return bytes(result)


class BlockingServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 포트를 잠시 예약해 빈 번호를 찾는다. 해제와 서버 bind 사이의 경합은 가능하다.
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            cls.port = reservation.getsockname()[1]
        # 파이프가 로그로 가득 차 서버가 멈추지 않도록 임시 파일을 사용한다.
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
        raise AssertionError('server startup failed: ' + diagnostics)

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(timeout=2)
        cls.log.seek(0)
        diagnostics = cls.log.read().decode(errors='replace')
        cls.log.close()
        assert cls.server.returncode == -signal.SIGTERM, diagnostics
        assert 'Sanitizer' not in diagnostics and 'runtime error:' not in diagnostics, diagnostics

    @classmethod
    def connect(cls):
        return socket.create_connection(('127.0.0.1', cls.port), timeout=10)

    @classmethod
    def echo(cls, payload):
        with cls.connect() as client:
            client.sendall(payload)
            client.shutdown(socket.SHUT_WR)
            return receive_all(client)

    def test_01_bytes_and_eof(self):
        for payload in (b'', '골!!!\n'.encode(), b'A\x00B\xff', bytes(range(256)) * 128):
            with self.subTest(length=len(payload)):
                self.assertEqual(self.echo(payload), payload)

    def test_02_fragmented_input_same_connection(self):
        # UTF-8 문자 중간도 자를 수 있다. 서버는 문자열로 해석하지 않고 바이트를 보존한다.
        payload = '축구 ⚽ 골!!!'.encode()
        fragments = [payload[:1], payload[1:5], payload[5:9], payload[9:]]
        with self.connect() as client:
            for fragment in fragments:
                client.sendall(fragment)
                self.assertEqual(receive_exact(client, len(fragment)), fragment)
                time.sleep(.03)
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(receive_all(client), b'')

    def test_03_queued_client_waits_for_current_connection(self):
        with self.connect() as first:
            first.sendall(b'A')
            self.assertEqual(receive_exact(first, 1), b'A')
            with self.connect() as second:
                second.sendall(b'B')
                second.settimeout(.2)
                with self.assertRaises(TimeoutError):
                    second.recv(1)
                first.shutdown(socket.SHUT_WR)
                self.assertEqual(receive_all(first), b'')
                second.settimeout(2)
                self.assertEqual(receive_exact(second, 1), b'B')
                second.shutdown(socket.SHUT_WR)
                self.assertEqual(receive_all(second), b'')

    def test_04_idle_timeout_releases_next_client(self):
        with self.connect() as idle:
            idle.sendall(b'ready')
            self.assertEqual(receive_exact(idle, 5), b'ready')
            started = time.monotonic()
            with self.connect() as queued:
                queued.sendall(b'after timeout')
                queued.shutdown(socket.SHUT_WR)
                self.assertEqual(receive_all(idle), b'')
                waited = time.monotonic() - started
                self.assertGreaterEqual(waited, 4.5)
                self.assertLess(waited, 9)
                self.assertEqual(receive_all(queued), b'after timeout')

    def test_05_one_hundred_clients(self):
        barrier = threading.Barrier(100, timeout=10)

        def client(number):
            payload = f'fan-{number}: goal!'.encode()
            with self.connect() as connection:
                # 모두 연결한 다음 전송해 대기열이 생긴 상황을 만든다.
                barrier.wait()
                connection.sendall(payload)
                connection.shutdown(socket.SHUT_WR)
                return payload, receive_all(connection)

        with concurrent.futures.ThreadPoolExecutor(max_workers=100) as pool:
            results = list(pool.map(client, range(100)))
        self.assertEqual(len(results), 100)
        for sent, received in results:
            self.assertEqual(received, sent)

    def test_06_abrupt_disconnect(self):
        with self.connect() as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            client.sendall(b'disconnect before reading')
        self.assertEqual(self.echo(b'server still alive'), b'server still alive')

    def test_07_configuration_errors(self):
        duplicate = subprocess.run([str(ROOT / 'server'), str(self.port)], capture_output=True, timeout=2)
        self.assertEqual(duplicate.returncode, 1)
        self.assertIn(b'bind:', duplicate.stderr)
        for argument in ('0', '65536', '-1', 'wrong', ''):
            process = subprocess.run([str(ROOT / 'server'), argument], capture_output=True, timeout=2)
            self.assertEqual(process.returncode, 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
