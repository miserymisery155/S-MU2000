#!/usr/bin/env python3
# license:BSD-3-Clause
"""鳴らした結果を小さな「指紋」にする。

WAV をリポジトリに置かずに回帰試験をするための道具。音そのものではなく、
音から測った数だけを置く。**標準ライブラリだけで動く**（MSYS2 の python でも
素の Windows の python でも同じ数が出るように、外の部品は使わない）。

指紋に入れるもの（doc/design.md「音の正しさを何で見るか」に沿う）:

  pcm_sha1   起動後の PCM そのもののハッシュ。**変わったかどうかはこれで決まる**
  keyon      keyon の時刻・チャンネル・サンプル番地・形式。音色が合っているか
  envelope   50ms ごとの最大振幅。音が抜けていないか
  dc         直流。整数の総和から出すので誤差が無い（doc/todo.md 3 番で使う）
  slow_rms   441 サンプル（=100Hz）平均した波形の実効値 ÷ 全体の実効値。
             低いところの量の目安。**「30Hz 以下の割合」ではない**ので、
             実機と比べる数としては使えない。こちら側の変化を追うためのもの

  python tools/fingerprint.py <wav> [--log <keyon ログ>] [--boot <サンプル数>]
"""
import argparse
import array
import hashlib
import json
import math
import re
import sys
import wave

ENV_MS = 50
SLOW_DIV = 441                  # 44100 / 441 = 100Hz まで落とす

KEYON = re.compile(r"^\[(\d+)\] keyon ([0-9a-f]{2}) (.*)$")


def load_wav(path):
    """(フレーム列, 標本化周波数, チャンネル数, 生バイト列) を返す"""
    w = wave.open(path)
    n, rate, ch, width = (w.getnframes(), w.getframerate(),
                          w.getnchannels(), w.getsampwidth())
    raw = w.readframes(n)
    w.close()
    if width != 2:
        raise SystemExit("16bit の WAV を前提にしている: %s" % path)
    a = array.array("h")
    a.frombytes(raw)
    return a, rate, ch, raw


def keyon_events(log_path, boot_samples):
    """-v で出た記録から keyon の行だけ拾う。時刻は MIDI の 0 秒を基準にする"""
    out = []
    if not log_path:
        return out
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = KEYON.match(line.strip())
            if m:
                out.append({"t": int(m.group(1)) - boot_samples,
                            "ch": int(m.group(2), 16),
                            "what": m.group(3)})
    out.sort(key=lambda e: (e["t"], e["ch"], e["what"]))
    return out


def envelope(frames, ch, rate, win_ms=ENV_MS):
    """win_ms ごとの最大振幅。左右のどちらかが出ていれば拾う"""
    win = int(rate * win_ms / 1000) * ch
    out = []
    for i in range(0, len(frames) - win + 1, win):
        seg = frames[i:i + win]
        out.append(max(max(seg), -min(seg)))
    return out


def stats(frames, ch):
    """チャンネルごとの (最大値, 実効値, 直流)。総和は整数なので誤差が無い"""
    peak, rms, dc = [], [], []
    n = len(frames) // ch
    for c in range(ch):
        s = frames[c::ch]
        if not n:
            peak.append(0); rms.append(0.0); dc.append(0.0); continue
        peak.append(max(max(s), -min(s)))
        total = 0
        sq = 0
        for v in s:
            total += v
            sq += v * v
        rms.append(round(math.sqrt(sq / n), 2))
        dc.append(round(total / n, 4))
    return peak, rms, dc


def slow_rms(frames, ch, div=SLOW_DIV):
    """平均して間引いた波形の実効値 ÷ 全体の実効値。低いところの量の目安"""
    n = len(frames) // ch
    if n < div * 4:
        return 0.0
    sq = 0
    for v in frames:
        sq += v * v
    if sq == 0:
        return 0.0
    full = math.sqrt(sq / len(frames))
    # 左右をまとめて 1 本にし、div 個ずつ平均する（先頭 0Hz から 100Hz まで）
    blocks = n // div
    slow_sq = 0.0
    for b in range(blocks):
        i = b * div * ch
        seg = frames[i:i + div * ch]
        m = sum(seg) / len(seg)
        slow_sq += m * m
    slow = math.sqrt(slow_sq / blocks)
    return round(slow / full, 6) if full else 0.0


def make(wav_path, log_path=None, boot_samples=0, name=None, seconds=None):
    frames, rate, ch, raw = load_wav(wav_path)
    cut = boot_samples * ch
    body = frames[cut:]
    peak, rms, dc = stats(body, ch)

    return {
        "name": name or "",
        "seconds": seconds,
        "rate": rate,
        "channels": ch,
        "boot_samples": int(boot_samples),
        "frames": len(body) // ch,
        "pcm_sha1": hashlib.sha1(raw[cut * 2:]).hexdigest(),
        "peak": peak,
        "rms": rms,
        "dc": dc,
        "slow_rms": slow_rms(body, ch),
        "envelope_ms": ENV_MS,
        "envelope": envelope(body, ch, rate),
        "keyon": keyon_events(log_path, boot_samples),
    }


def summary(fp):
    """人が読む 1 行"""
    dc = max(fp["dc"], key=abs) if fp["dc"] else 0.0
    return ("keyon %3d  peak %6d  rms %7.1f  dc %+8.3f  低域比 %6.3f%%"
            % (len(fp["keyon"]), max(fp["peak"]), max(fp["rms"]),
               dc, 100.0 * fp["slow_rms"]))


def diff(old, new):
    """どこが変わったかを人が読める行で返す。空なら同じ"""
    out = []
    if old.get("pcm_sha1") == new.get("pcm_sha1"):
        return out

    for key in ("boot_samples", "frames", "rate", "channels"):
        if old.get(key) != new.get(key):
            out.append("  %-12s %s → %s" % (key, old.get(key), new.get(key)))

    ko, kn = old.get("keyon", []), new.get("keyon", [])
    if len(ko) != len(kn):
        out.append("  keyon の数    %d → %d" % (len(ko), len(kn)))
    so = set((e["t"], e["ch"], e["what"]) for e in ko)
    sn = set((e["t"], e["ch"], e["what"]) for e in kn)
    gone, came = sorted(so - sn), sorted(sn - so)
    if gone or came:
        out.append("  keyon の中身が違う（消えた %d / 増えた %d）" % (len(gone), len(came)))
        for t, c, what in gone[:4]:
            out.append("    - [%8d] ch %02x %s" % (t, c, what))
        for t, c, what in came[:4]:
            out.append("    + [%8d] ch %02x %s" % (t, c, what))

    eo, en = old.get("envelope", []), new.get("envelope", [])
    n = min(len(eo), len(en))
    if n:
        d = [abs(en[i] - eo[i]) for i in range(n)]
        worst = d.index(max(d))
        base = max(1, max(eo[:n]))
        out.append("  包絡線  最大差 %d (%.2f%%) @ %.2f 秒、平均差 %.2f"
                   % (max(d), 100.0 * max(d) / base,
                      worst * ENV_MS / 1000.0, sum(d) / n))
    if len(eo) != len(en):
        out.append("  包絡線のコマ数 %d → %d" % (len(eo), len(en)))

    for key in ("peak", "rms", "dc"):
        if old.get(key) != new.get(key):
            out.append("  %-4s %s → %s" % (key, old.get(key), new.get(key)))
    if old.get("slow_rms") != new.get("slow_rms"):
        out.append("  低域比  %.4f%% → %.4f%%"
                   % (100.0 * old.get("slow_rms", 0), 100.0 * new.get("slow_rms", 0)))
    out.append("  pcm_sha1  %s → %s" % (old.get("pcm_sha1", "")[:12],
                                        new.get("pcm_sha1", "")[:12]))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--log")
    ap.add_argument("--boot", type=int, default=0)
    ap.add_argument("--json", action="store_true", help="指紋をそのまま出す")
    a = ap.parse_args()
    fp = make(a.wav, a.log, a.boot)
    print(json.dumps(fp, ensure_ascii=False, indent=1) if a.json else summary(fp))
    return 0


if __name__ == "__main__":
    sys.exit(main())
