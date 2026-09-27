#!/usr/bin/env python3
# license:BSD-3-Clause
"""ノートを複数の S-MU2000 に振り分ける（同時発音 128 を超える曲を、何台かで鳴らす）。

  python tools/split_instances.py 曲.mid [出力.mid] [--n 4] [--release 0.5] [--files]

  出力を省くと「曲_split4.mid」。形式 1 で、1 本目がテンポなどのメタ（コンダクター）、
  あとは口 A-D（FF 21）× チャンネルごとに 1 本ずつ（Domino は 1 トラックに
  1 チャンネルでないと開けない）と、口ごとの SysEx のトラック。口 1 つを S-MU2000 1 台で受ける（gui.exe を n 個立ち上げて、
  それぞれ別の MIDI ポートで受ける。DAW なら n 個挿して、トラックごとに分ける）
  --n        台数（2-4。既定 4）
  --release  離した音をあと何秒「鳴っている」と数えるか（既定 0.5）
  --files    台ごとに分けたファイル（曲_split4_A.mid …）も書く。口の指定が無い
             （口 A で受ける）ので、1 台ずつ鳴らして確かめるのに使う

振り分け方:
* ノートオンは、そのとき鳴っている音（押している音と、離して --release 秒以内の音）が
  いちばん少ない台へ。同じ数なら若い口
* ノートオフ（とベロシティ 0 のノートオン）は、対応するノートオンを受けた台へ
  （同じチャンネル・同じ鍵の押しを古い順に対応させる）
* ノート以外（音色・CC・ペダル・ピッチベンド・SysEx）は全部の台へ。どの台も同じ設定で
  鳴る。テンポ・拍子・曲名などのメタは 1 本目（コンダクター）にだけ置く
* 元のファイルの口の指定（FF 21）は無視する（口 A の 16 パートの曲を前提にする）

気を付けること: 1 台の中で閉じる働き（モノのパートのレガート、ポルタメント、同じ鍵の
押し直し）は、台をまたぐと実機と違う。リバーブなどのエフェクトはそれぞれの台で掛かり、
足すと 1 台で鳴らしたのとほぼ同じになる（インサーションの歪みなど、足し算にならない
ものは少し違う）
"""
import struct
import sys


def read_vlq(d, p):
    v = 0
    while True:
        c = d[p]
        p += 1
        v = (v << 7) | (c & 0x7f)
        if not c & 0x80:
            return v, p


def vlq(n):
    out = [n & 0x7f]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7f))
        n >>= 7
    return bytes(reversed(out))


def read_events(path):
    """全トラックの (tick, トラック番号, 並び番号, 生バイト列, 種類) を時刻順に"""
    d = open(path, 'rb').read()
    if d[:4] != b'MThd':
        raise SystemExit('MIDI ファイルではない: %s' % path)
    hlen = struct.unpack('>I', d[4:8])[0]
    _, ntrk, div = struct.unpack('>HHH', d[8:14])
    pos = 8 + hlen
    evs = []
    for ti in range(ntrk):
        ln = struct.unpack('>I', d[pos + 4:pos + 8])[0]
        p, end = pos + 8, pos + 8 + ln
        pos = end
        tick, run, k = 0, 0, 0
        while p < end:
            dt, p = read_vlq(d, p)
            tick += dt
            st = d[p]
            if st & 0x80:
                p += 1
            else:
                st = run
            if st == 0xff:
                ty = d[p]
                n, q = read_vlq(d, p + 1)
                body = d[q:q + n]
                p = q + n
                if ty == 0x2f:
                    break
                if ty == 0x21:
                    continue                      # 元の口の指定は捨てる
                evs.append((tick, ti, k, bytes([0xff, ty]) + vlq(n) + body, 'meta', ty))
            elif st in (0xf0, 0xf7):
                n, q = read_vlq(d, p)
                evs.append((tick, ti, k, bytes([st]) + vlq(n) + d[q:q + n], 'sysex', None))
                p = q + n
            else:
                run = st
                n = 1 if (st & 0xf0) in (0xc0, 0xd0) else 2
                evs.append((tick, ti, k, bytes([st]) + d[p:p + n], 'chan', None))
                p += n
            k += 1
    evs.sort(key=lambda e: (e[0], e[1], e[2]))
    return div, evs


def tempo_map(evs, div):
    """tick → 秒 に直す関数"""
    changes = [(0, 500000)]
    for tick, _, _, raw, kind, ty in evs:
        if kind == 'meta' and ty == 0x51:
            changes.append((tick, int.from_bytes(raw[-3:], 'big')))     # テンポは中身 3 バイト
    changes.sort()
    seg = []
    sec, last_tick, us = 0.0, 0, 500000
    for tick, u in changes:
        sec += (tick - last_tick) * us / (div * 1e6)
        seg.append((tick, sec, u))
        last_tick, us = tick, u

    def to_sec(t):
        lo = 0
        for i, (tk, _, _) in enumerate(seg):
            if tk <= t:
                lo = i
            else:
                break
        tk, s, u = seg[lo]
        return s + (t - tk) * u / (div * 1e6)
    return to_sec


def track_bytes(items):
    out, last = b'', 0
    for tick, raw in items:
        out += vlq(tick - last) + raw
        last = tick
    out += vlq(0) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(out)) + out


def main():
    args = [a for a in sys.argv[1:]]
    opts = {'n': 4, 'release': 0.5}
    files = False
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ('--n', '--release') and i + 1 < len(args):
            opts[a[2:]] = float(args[i + 1]) if a == '--release' else int(args[i + 1])
            i += 2
            continue
        if a == '--files':
            files = True
        else:
            pos.append(a)
        i += 1
    if not pos:
        print(__doc__)
        return 1
    n = max(2, min(4, opts['n']))
    src = pos[0]
    base = src[:-4] if src.lower().endswith('.mid') else src
    dst = pos[1] if len(pos) > 1 else '%s_split%d.mid' % (base, n)

    div, evs = read_events(src)
    to_sec = tempo_map(evs, div)
    rel = opts['release']

    out = [[] for _ in range(n)]
    held = {}                                  # (ch, key) → [台, …]（古い順）
    ends = [[] for _ in range(n)]              # 台ごとの「鳴っている」音の終わり（秒。押している間は None）
    peak = [0] * n
    count = [0] * n
    single_peak = 0
    single_now = []
    for tick, ti, k, raw, kind, ty in evs:
        if kind == 'chan' and (raw[0] & 0xf0) in (0x80, 0x90):
            ch, key = raw[0] & 15, raw[1]
            on = (raw[0] & 0xf0) == 0x90 and raw[2] > 0
            now = to_sec(tick)
            if on:
                live = []
                for j in range(n):
                    ends[j] = [e for e in ends[j] if e is None or e > now]
                    live.append(len(ends[j]))
                j = min(range(n), key=lambda x: (live[x], x))
                held.setdefault((ch, key), []).append(j)
                ends[j].append(None)
                peak[j] = max(peak[j], live[j] + 1)
                count[j] += 1
                single_now = [e for e in single_now if e is None or e > now] + [None]
                single_peak = max(single_peak, len(single_now))
                out[j].append((tick, raw))
            else:
                q = held.get((ch, key))
                if not q:
                    for j in range(n):              # 対応する押しが無い離しは全部へ
                        out[j].append((tick, raw))
                    continue
                j = q.pop(0)
                if None in ends[j]:
                    ends[j][ends[j].index(None)] = now + rel
                if None in single_now:
                    single_now[single_now.index(None)] = now + rel
                out[j].append((tick, raw))
            continue
        if kind == 'meta' and ty in (0x51, 0x58, 0x59):
            out[0].append((tick, raw))              # テンポ・拍子・調は 1 本目だけ
            continue
        for j in range(n):
            out[j].append((tick, raw))

    names = 'ABCD'

    def name_ev(text):
        b = text.encode()
        return (0, b'\xff\x03' + vlq(len(b)) + b)

    def split_tracks(items, port):
        """1 台ぶんを「チャンネルごとのトラック」と「SysEx のトラック」に分ける（Domino は
        1 トラック 1 チャンネルでないと開けない）。メタは捨てる（コンダクターに置く）"""
        per_ch, sysex = {}, []
        for t, r in items:
            if r[0] == 0xff:
                continue
            if r[0] in (0xf0, 0xf7):
                sysex.append((t, r))
            else:
                per_ch.setdefault(r[0] & 15, []).append((t, r))
        head = [] if port is None else [(0, b'\xff\x21\x01' + bytes([port]))]
        label = '' if port is None else 'Port %s ' % names[port]
        out_tracks = []
        if sysex:
            out_tracks.append(track_bytes(head + [name_ev(label + 'SysEx')] + sysex))
        for ch in sorted(per_ch):
            out_tracks.append(track_bytes(head + [name_ev('%sCh%d' % (label, ch + 1))] + per_ch[ch]))
        return out_tracks

    # 1 本目はテンポ・拍子・曲名などのメタだけ（コンダクター）
    conductor = track_bytes([(t, r) for t, r in out[0] if r[0] == 0xff])
    tracks = [conductor]
    for j in range(n):
        tracks += split_tracks(out[j], j)
    open(dst, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 1, len(tracks), div) + b''.join(tracks))
    if files:
        # 1 台ずつのファイル（口の指定なし。どれも口 A で受ける）
        stem = dst[:-4] if dst.lower().endswith('.mid') else dst
        for j in range(n):
            tr = [conductor] + split_tracks(out[j], None)
            open('%s_%s.mid' % (stem, names[j]), 'wb').write(
                b'MThd' + struct.pack('>IHHH', 6, 1, len(tr), div) + b''.join(tr))
    print('%s: %d 台に分けた（ノートオン %s）' % (dst, n, ' / '.join('%s %d' % (names[j], count[j]) for j in range(n))))
    print('同時に鳴っている音の見積もり（押している音と離して %.1f 秒以内）: 1 台なら最大 %d、'
          '分けたあとは %s' % (rel, single_peak, ' / '.join('%s %d' % (names[j], peak[j]) for j in range(n))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
