"""README 그림(SVG) 생성기 — python3 tools/draw_diagrams.py  →  docs/images/*.svg

스타일: 흰 상자 + 검은 테두리(구성 요소), 파란 화살표·글씨(정상 흐름), 분홍 화살표·글씨 + ✕(실패 흐름).
그림마다 오른쪽 아래에 '개념도' 또는 '실측'을 표시합니다. 실측 그림의 숫자는 docs/results/의 실제 결과입니다.
"""
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "images")
BLUE, PINK, INK, GRAY = "#2f6fe4", "#e0457b", "#111111", "#8a8a8a"
FONT = "-apple-system, 'Apple SD Gothic Neo', 'Noto Sans KR', 'Malgun Gothic', sans-serif"


class Svg:
    def __init__(self, width, height):
        self.w, self.h, self.parts = width, height, []

    def title(self, x, y, text, size=22):
        self.parts.append(f'<text x="{x}" y="{y}" font-size="{size}" font-weight="700" fill="{INK}">{text}</text>')

    def text(self, x, y, lines, color=INK, size=14, weight=400, anchor="middle"):
        if isinstance(lines, str):
            lines = [lines]
        top = y - (len(lines) - 1) * size * 0.65
        for i, line in enumerate(lines):
            self.parts.append(f'<text x="{x}" y="{top + i * size * 1.3:.1f}" font-size="{size}" font-weight="{weight}" '
                              f'fill="{color}" text-anchor="{anchor}" dominant-baseline="middle">{line}</text>')

    def box(self, x, y, w, h, lines, size=17, dashed=False, fill="#ffffff"):
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{INK}" stroke-width="1.6"{dash}/>')
        self.text(x + w / 2, y + h / 2, lines, size=size, weight=700)
        return (x, y, w, h)

    def arrow(self, x1, y1, x2, y2, color=BLUE, label=None, lx=None, ly=None, both=False, cross=False,
              dashed=False, size=13):
        marker = "pink" if color == PINK else ("gray" if color == GRAY else "blue")
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        start = f' marker-start="url(#{marker}-start)"' if both else ""
        end = "" if cross else f' marker-end="url(#{marker})"'
        self.parts.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="1.6"{dash}{start}{end}/>')
        if cross:  # 끝에 ✕
            s = 7
            self.parts.append(f'<path d="M{x2-s},{y2-s} L{x2+s},{y2+s} M{x2-s},{y2+s} L{x2+s},{y2-s}" stroke="{color}" stroke-width="2"/>')
        if label:
            self.text(lx if lx is not None else (x1 + x2) / 2, ly if ly is not None else (y1 + y2) / 2 - 14,
                      label, color=color, size=size)

    def tag(self, kind):
        self.text(self.w - 16, self.h - 14, kind, color=GRAY, size=12, anchor="end")

    def save(self, name):
        defs = []
        for marker, color in (("blue", BLUE), ("pink", PINK), ("gray", GRAY)):
            defs.append(f'<marker id="{marker}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto">'
                        f'<path d="M0,0 L10,5 L0,10 z" fill="{color}"/></marker>')
            defs.append(f'<marker id="{marker}-start" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="8" markerHeight="8" orient="auto">'
                        f'<path d="M10,0 L0,5 L10,10 z" fill="{color}"/></marker>')
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}" '
               f'font-family="{FONT}">\n<defs>{"".join(defs)}</defs>\n'
               f'<rect width="{self.w}" height="{self.h}" fill="#ffffff"/>\n' + "\n".join(self.parts) + "\n</svg>\n")
        with open(os.path.join(OUT, name), "w") as f:
            f.write(svg)


def architecture():
    s = Svg(960, 420)
    s.title(30, 42, "MicroHTTP 구조")
    s.box(30, 200, 150, 64, "브라우저")
    s.box(270, 200, 170, 64, ["main 스레드", "accept 루프"])
    s.box(520, 200, 170, 64, ["작업 스레드 ×8"])
    s.box(520, 70, 170, 64, ["Session 저장소", "리스트 + mutex"], size=15)
    s.box(770, 70, 170, 64, ["auth.c", "/login /logout /me"], size=15)
    s.box(770, 200, 170, 64, "정적 파일")
    s.box(770, 320, 170, 64, ["proxy.c", "→ 백엔드 서버"], size=15)
    s.arrow(180, 232, 268, 232, label=["TCP 연결", "Cookie: sid=…"], ly=204)
    s.arrow(440, 232, 518, 232, label=["fd → 큐"], ly=218)
    s.arrow(690, 220, 768, 130, label="인증 경로", lx=760, ly=168)
    s.arrow(690, 232, 768, 232)
    s.arrow(690, 244, 768, 344, label="-R 규칙", lx=760, ly=292)
    s.arrow(768, 102, 692, 102, label=["조회 결과는", "복사본만"], lx=730, ly=58)
    s.arrow(355, 200, 518, 116, color=GRAY, dashed=True, label=["1초마다", "만료 정리"], lx=400, ly=128)
    s.tag("개념도")
    s.save("architecture.svg")


def fd_vs_session():
    s = Svg(1040, 400)
    for ox, heading, server, b_label, b_result, b_ok in (
            (0, "연결(fd)로 사람을 구분하면", ["MicroHTTP", "fd 5 = alice"], ["연결 2 (fd 5 재사용)", "Cookie 없음"], ["200  user: alice", "남이 alice가 됨"], False),
            (530, "Session ID로 사람을 구분하면", ["MicroHTTP", "sid X = alice"], ["연결 2 (fd 5 재사용)", "Cookie 없음"], ["401", "누군지 모름"], True)):
        s.title(30 + ox, 42, heading)
        s.box(30 + ox, 90, 150, 64, ["A (alice)"])
        s.box(30 + ox, 270, 150, 64, ["B", "(로그인 안 함)"], size=15)
        s.box(300 + ox, 180, 170, 70, server, size=16)
        a_label = ["연결 1 (fd 5)", "로그인 후 연결 종료"] if not b_ok else ["어느 연결이든", "Cookie: sid=X → 200"]
        s.arrow(180 + ox, 122, 298 + ox, 200, label=a_label, lx=270 + ox, ly=110)
        if b_ok:
            s.arrow(180 + ox, 302, 300 + ox, 236, color=PINK, cross=True, label=b_label, lx=250 + ox, ly=330)
            s.text(400 + ox, 290, b_result, color=PINK, size=14, weight=700)
        else:
            s.arrow(180 + ox, 302, 298 + ox, 236, color=PINK, label=b_label, lx=250 + ox, ly=330)
            s.text(400 + ox, 290, b_result, color=PINK, size=14, weight=700)
    s.parts.append(f'<line x1="515" y1="70" x2="515" y2="360" stroke="#dddddd" stroke-width="1"/>')
    s.tag("실측 · docs/results/step1_fd_reuse.md")
    s.save("fd_vs_session.svg")


def forged_cookies():
    s = Svg(960, 470)
    s.title(30, 42, "클라이언트가 보낸 값은 '열쇠'일 뿐, 상태가 아니다")
    rows = [("Cookie: sid=정상 64자", True, "200"),
            ("Cookie 없음", False, "401"),
            ("sid 한 글자 변조", False, "401"),
            ("sid=aaaa…a (형식만 맞춤)", False, "401"),
            ("user=alice; logged_in=true", False, "401"),
            ("로그아웃한 sid 재사용", False, "401")]
    for i, (label, ok, result) in enumerate(rows):
        y = 80 + i * 60
        s.box(30, y, 270, 44, label, size=14)
        color = BLUE if ok else PINK
        s.arrow(300, y + 22, 560, 240, color=color, cross=not ok)
        s.text(312, y + (8 if y + 22 <= 240 else 38), result, color=color, size=14, weight=700, anchor="start")
    s.box(560, 200, 200, 80, ["서버 측 Session 저장소", "이 sid가 있고", "만료 전인가?"], size=14)
    s.box(800, 210, 130, 60, ["user: alice"], size=15)
    s.arrow(760, 240, 798, 240)
    s.text(780, 300, ["형식 검사 → 상수 시간 비교", "→ 만료 검사"], color=BLUE, size=13)
    s.tag("실측 · tests/functional/test_server.py test_auth_*")
    s.save("forged_cookies.svg")


def uaf():
    s = Svg(1060, 440)
    s.title(30, 42, "포인터를 넘기면")
    s.box(30, 90, 190, 64, ["작업 스레드 1", "/me 응답 만드는 중"], size=15)
    s.box(30, 290, 190, 64, ["작업 스레드 2", "/logout"], size=15)
    s.box(300, 190, 180, 64, ["Session 노드", "user = alice"], size=15, dashed=True)
    s.arrow(220, 112, 298, 200, label=["① 조회 → 노드 포인터", "받고 락 해제"], lx=345, ly=112)
    s.arrow(220, 322, 298, 246, color=PINK, label=["② free()"], lx=290, ly=316)
    s.arrow(150, 154, 330, 254, color=PINK, cross=True, label=["③ 해제된 메모리 읽기"], lx=120, ly=240)
    s.text(255, 395, ["ASan: heap-use-after-free", "Valgrind: Invalid read 4건"], color=PINK, size=14, weight=700)

    ox = 540
    s.parts.append(f'<line x1="525" y1="70" x2="525" y2="410" stroke="#dddddd" stroke-width="1"/>')
    s.title(30 + ox, 42, "복사해서 들고 나가면")
    s.box(30 + ox, 90, 190, 64, ["작업 스레드 1", "SessionInfo 복사본"], size=15)
    s.box(30 + ox, 290, 190, 64, ["작업 스레드 2", "/logout"], size=15)
    s.box(300 + ox, 190, 180, 64, ["Session 노드", "user = alice"], size=15, dashed=True)
    s.arrow(220 + ox, 112, 298 + ox, 200, label=["① 락 안에서 값 복사", "→ 락 해제"], lx=345 + ox, ly=112)
    s.arrow(220 + ox, 322, 298 + ox, 246, label=["② free()"], lx=290 + ox, ly=316)
    s.text(125 + ox, 200, ["③ 복사본으로", "응답 → 안전"], color=BLUE, size=14, weight=700)
    s.text(255 + ox, 395, ["ASan · Valgrind: 0건"], color=BLUE, size=14, weight=700)
    s.tag("실측 · docs/results/step2_uaf_asan.txt, step2_linux_verify.txt")
    s.save("uaf.svg")


def lock_scope():
    s = Svg(980, 430)
    s.title(30, 42, "락을 어디까지 쥘 것인가 — GET /me 20,000회, 3회 중앙값")
    groups = [("동시 8", 49975, 43002), ("동시 50", 46339, 45652)]
    maxv, x0, width = 55000, 170, 360
    for i, (name, narrow, wide) in enumerate(groups):
        y = 90 + i * 120
        s.text(150, y + 38, name, size=15, weight=700, anchor="end")
        for j, (label, value, color) in enumerate((("좁은 락", narrow, BLUE), ("넓은 락", wide, PINK))):
            yy = y + j * 40
            w = value / maxv * width
            s.parts.append(f'<rect x="{x0}" y="{yy + 10}" width="{w:.0f}" height="26" fill="{color}" opacity="0.85"/>')
            s.text(x0 + w + 10, yy + 23, f"{label}  {value:,} 요청/초", color=color, size=14, weight=700, anchor="start")
    s.text(x0, 345, ["동시 8: 3회 범위가 겹치지 않음 → 넓은 락이 약 14% 느림",
                     "동시 50: 범위가 겹침 → 차이 없음 (병목이 스레드 풀 대기로 이동)"], size=14, anchor="start")
    s.box(750, 90, 200, 110, ["평균 락 점유", "좁은 0.21 us", "넓은 7.26 us (34배)"], size=14)
    s.box(750, 220, 200, 110, ["평균 락 대기", "좁은 0.55 us", "넓은 16.10 us (29배)"], size=14)
    s.tag("실측 · docs/results/step2_lock_scope.csv, .err")
    s.save("lock_scope.svg")


def proxy_pass():
    s = Svg(960, 300)
    s.title(30, 42, "Reverse Proxy를 거쳐도 인증은 같다")
    s.box(30, 120, 160, 64, "브라우저")
    s.box(400, 120, 170, 64, ["Reverse Proxy", "(MicroHTTP)"], size=15)
    s.box(780, 120, 160, 64, ["백엔드", "(MicroHTTP + 인증)"], size=15)
    s.arrow(190, 140, 398, 140, label=["Cookie: sid=X"], ly=122)
    s.arrow(398, 168, 192, 168, label=["Set-Cookie, no-store", "그대로 중계"], ly=196)
    s.arrow(570, 140, 778, 140, label=["Cookie 그대로 전달", "(hop-by-hop 아님)"], ly=112)
    s.arrow(778, 168, 572, 168, label=["303 / 200 / 401"], ly=190)
    s.text(480, 260, "직접 접근과 프록시 경유: 로그인 · /me · 로그아웃 · Cookie 속성 · no-store 결과 동일", color=BLUE, size=14)
    s.tag("실측 · docs/results/step3_proxy_auth.md")
    s.save("proxy_pass.svg")


def multi_server():
    s = Svg(960, 400)
    s.title(30, 42, "서버가 2대가 되면")
    s.box(30, 160, 160, 64, "브라우저")
    s.box(330, 160, 190, 64, ["Reverse Proxy", "Round-Robin"], size=15)
    s.box(680, 70, 230, 64, ["서버 A", "Session X 있음"], size=15)
    s.box(680, 260, 230, 64, ["서버 B", "Session X 없음"], size=15)
    s.arrow(190, 192, 328, 192, label=["Cookie: sid=X"], ly=176)
    s.arrow(520, 180, 678, 104, label=["1·3·5번째 /me → 200"], lx=585, ly=118)
    s.arrow(520, 204, 680, 290, color=PINK, cross=True, label=["2·4·6번째 /me → 401"], lx=560, ly=268)
    s.text(480, 365, "같은 Cookie인데 요청마다 로그인과 로그아웃을 오감 — Session이 각 서버의 메모리에 있기 때문", color=PINK, size=14)
    s.tag("실측 · docs/results/step7_two_servers.md")
    s.save("multi_server.svg")


def ttl_concept():
    s = Svg(960, 420)
    s.title(30, 42, "TTL을 얼마로 할까")
    x0, y0, x1, y1 = 120, 340, 760, 90
    s.arrow(x0, y0, x1 + 20, y0, color=INK)
    s.arrow(x0, y0, x0, y1 - 20, color=INK)
    s.text((x0 + x1) / 2, y0 + 30, "Session 수명 (TTL)  짧게 ←→ 길게", size=14)
    s.text(x0 - 20, (y0 + y1) / 2, ["부담"], size=14, anchor="end")
    s.parts.append(f'<path d="M{x0+20},{y0-20} C{x0+250},{y0-30} {x1-250},{y1+120} {x1},{y1+10}" fill="none" stroke="{PINK}" stroke-width="2.2"/>')
    s.parts.append(f'<path d="M{x0+20},{y1+10} C{x0+200},{y0-80} {x1-200},{y0-20} {x1},{y0-15}" fill="none" stroke="{BLUE}" stroke-width="2.2"/>')
    s.text(x1 + 10, y1 + 10, ["탈취된 ID가", "유효한 시간"], color=PINK, size=14, weight=700, anchor="start")
    s.text(x0 + 30, y1 - 5, ["다시 로그인하는", "번거로움"], color=BLUE, size=14, weight=700, anchor="start")
    s.text(470, 385, "개념 설명용 — 측정값이 아니며, 두 곡선의 교점을 최적값으로 단정하지 않습니다", color=GRAY, size=13)
    s.tag("개념도")
    s.save("ttl_concept.svg")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for draw in (architecture, fd_vs_session, forged_cookies, uaf, lock_scope, proxy_pass, multi_server, ttl_concept):
        draw()
    print("written:", ", ".join(sorted(os.listdir(OUT))))
