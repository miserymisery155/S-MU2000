#!/usr/bin/env python3
# license:BSD-3-Clause
"""MAME の mu2000 起動に必要だが再配布物として入手できないデバイス ROM の代替品を生成する。

  roms/mulcd.zip : mulcd.svg（LCD 描画用 SVG, 自作）+ hd44780u_b04.bin（フォント ROM, Adafruit GFX の 5x7 フォントから生成, BSD）
  roms/swp30.zip : sin-table.bin（1/4 波 sin テーブル。実チップのテーブルとは一致しない近似）

いずれも MAME の登録ハッシュとは一致しないため起動時に警告が出るが、動作はする。
LCD は HD44780 2 行モードで使われ、mulcd は先頭 64 セル（1 行目 DDRAM 0x00-0x27 の 40 セル + 2 行目 0x40-0x57 の 24 セル）
を出力する。実機の表示部は 2 行 × 24 桁なので、1 行目はセル 0-23、2 行目はセル 40-63 を配置する。
セル 24-39（DDRAM 0x18-0x27）は表示に使われないので描かない。
"""
import math
import re
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
REF = Path(__file__).resolve().parent / "ref"
OUT = ROOT / "roms/standin"
OUT.mkdir(parents=True, exist_ok=True)

# ---- フォント ROM (glcdfont.c: 1 文字 5 バイト, 各バイトが 1 列, bit0 が上端)
src = (REF / "glcdfont.c").read_text()
body = src[src.index("{"):]
cols = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", body)]
assert len(cols) >= 256 * 5, len(cols)
rom = bytearray(0x1000)
for c in range(256):
    for y in range(7):
        v = 0
        for x in range(5):
            if (cols[c * 5 + x] >> y) & 1:
                v |= 1 << (4 - x)      # HD44780: bit4 が左端
        rom[c * 16 + y] = v
# Yamaha 独自マスク B04 の 0x80 以上は図形文字（パートレベルメータ等）。内容が不明なので
# 代替フォントの欧文アクセント文字が出て化けないよう空白にし、レベルメータ「0」と思われる 0x89 だけ下線にする。
for c in range(0x80, 0x100):
    for y in range(8):
        rom[c * 16 + y] = 0
rom[0x89 * 16 + 7] = 0x15   # 2 px 幅のメータ 2 本分の基線（推定）
# 観測したコードの推定図形（doc/dump/hardware.md 参照）
def glyph(code, rows):
    for y, r in enumerate(rows):
        rom[code * 16 + y] = int(r.replace("#", "1").replace(".", "0"), 2)
glyph(0x14, [".....", ".....", ".###.", ".###.", ".###.", ".....", ".....", "....."])   # メニュー項目マーカー ▪
glyph(0xC7, ["##...", "##...", "##...", "##...", "##...", "##...", "##...", "##..."])   # 起動アニメの枠 左
glyph(0x87, ["...##", "...##", "...##", "...##", "...##", "...##", "...##", "...##"])   # 起動アニメの枠 右
glyph(0xCF, ["##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##", "##.##"])   # 起動アニメの枠 両側
(OUT / "hd44780u_b04.bin").write_bytes(rom)

# ---- sin テーブル
N = 0x8000
(OUT / "sin-table.bin").write_bytes(struct.pack("<%dH" % N, *(min(65535, round(0x8000 + math.sin((i + .5) / N * math.pi / 2) * 0x7fff)) for i in range(N))))

# ---- LCD SVG
W, H = 1280, 386
COLS = 24
cw = W / COLS                      # 53.3
px = cw / 6.2                      # ピクセル幅（5 列 + 隙間）
py = px                            # 正方ピクセル
row_h = py * 1.1 * 8 + py * 1.5    # 8 行 + 行間
y0 = (H - row_h * 2) / 2
svg = [f'<?xml version="1.0" encoding="UTF-8"?>',
       f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
       f'<rect x="0" y="0" width="{W}" height="{H}" fill="#ffffff"/>']
for row, first in ((0, 0), (1, 40)):   # 実機の表示部は 2 行 × 24 桁。セル 24-39 は表示しない
    for pos in range(COLS):
        cell = first + pos
        ox = pos * cw + px * 0.6
        oy = y0 + row * row_h
        for y in range(8):
            for z in range(5):
                rx = ox + (4 - z) * px * 1.1
                ry = oy + y * py * 1.1
                svg.append(f'<rect x="{rx:.1f}" y="{ry:.1f}" width="{px:.1f}" height="{py:.1f}" fill="#101010"><title>{cell:03x}.{y}.{z}</title></rect>')
data = "\n".join(svg).encode()
TARGET = 525261                    # MAME が要求するサイズに合わせてコメントで埋める
tail = b"\n</svg>\n"
pad = TARGET - len(data) - len(tail) - 8   # comment open (5 bytes) + close (3 bytes)
assert pad > 0
data += b"\n<!--" + b"-" * pad + b"-->" + tail
assert len(data) == TARGET
(OUT / "mulcd.svg").write_bytes(data)

with zipfile.ZipFile(ROOT / "roms/mulcd.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.write(OUT / "mulcd.svg", "mulcd.svg")
    z.write(OUT / "hd44780u_b04.bin", "hd44780u_b04.bin")
with zipfile.ZipFile(ROOT / "roms/swp30.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.write(OUT / "sin-table.bin", "sin-table.bin")
print("wrote roms/mulcd.zip, roms/swp30.zip")
