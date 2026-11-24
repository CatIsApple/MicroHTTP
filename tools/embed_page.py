"""web/match.html을 C 바이트 배열(football_page.inc)로 변환한다.

왜 필요한가?
    서버가 실행 중에 HTML 파일을 디스크에서 읽지 않도록, HTML을 실행 파일 안에
    통째로 넣기 위해서다. football.c가 생성된 파일을 #include 해서 사용한다.

언제 실행되나?
    `make` 할 때 web/match.html이 바뀌었으면 Makefile이 자동으로 실행한다.
    외부 패키지 없이 파이썬 표준 라이브러리만 사용한다.

생성 결과 예:
    static const unsigned char match_page[] = {
    60, 33, 100, 111, ...    # '<', '!', 'd', 'o', ... 각 바이트를 숫자로
    };
"""
from pathlib import Path

# 이 스크립트(tools/embed_page.py)의 한 단계 위 폴더 = 프로젝트 루트
root = Path(__file__).resolve().parents[1]
# HTML을 바이트 그대로 읽는다(한글 UTF-8 바이트도 그대로 보존).
# 끝에 '\0'을 붙여 C 문자열처럼 쓸 수 있게 한다. 그래서 C 쪽 길이는 sizeof - 1이다.
data = (root / 'web/match.html').read_bytes() + b'\0'
# 한 줄에 24바이트씩 "60, 33, 100, ..." 형태로 나눠 보기 좋게 만든다.
rows = [', '.join(str(byte) for byte in data[i:i + 24]) for i in range(0, len(data), 24)]
(root / 'football_page.inc').write_text(
    '/* Generated from web/match.html. Edit the HTML, not this file. */\n'
    'static const unsigned char match_page[] = {\n' + ',\n'.join(rows) + '\n};\n'
)
