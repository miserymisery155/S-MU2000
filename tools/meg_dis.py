# license:BSD-3-Clause
# SWP30 の MEG（エフェクト DSP）のプログラムを人の読める形にする。
#
#   build/render.exe roms 曲.mid out.wav 20 --dump-meg meg
#   python tools/meg_dis.py meg.m
#   python tools/meg_dis.py meg.m --diff meg2.m     2 つを並べる
#
# 命令の形は MAME の swp30_disassembler をそのまま写した。
# 1 サンプルにつき 384 段（0x180）を上から順に 1 回ずつ流すだけで、
# 分岐は無い。3 段で 1 組（pc/3 が番地表の添字）。

import sys


def bits(v, pos, n=1):
    return (v >> pos) & ((1 << n) - 1)


def load(path):
    prg, const, off, lfo, mp, mix = {}, {}, {}, {}, {}, []
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "prg":
            prg[int(p[1], 16)] = int(p[2], 16)
            const[int(p[1], 16)] = int(p[3], 16)
        elif p[0] == "off":
            off[int(p[1], 16)] = int(p[2], 16)
        elif p[0] == "lfo":
            lfo[int(p[1], 16)] = int(p[2], 16)
        elif p[0] == "map":
            mp[int(p[1], 16)] = int(p[2], 16)
        elif p[0] == "mix":
            mix.append(line.rstrip())
    return prg, const, off, lfo, mp, mix


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def disasm(pc, opcode, const, off):
    r = []

    def add(x):
        r.append(x)

    sm = bits(opcode, 0x04, 6)
    sr = bits(opcode, 0x0B, 7)
    dm = bits(opcode, 0x27, 6)
    dr = bits(opcode, 0x30, 7)
    t = bits(opcode, 0x38, 3)
    gconst = "%g" % (s16(const.get(pc, 0)) / 32768.0)

    if bits(opcode, 0x3F):
        # 分岐（doc/upstream.md の 11）。条件 bit 3 が 0 なら必ず、1 なら bit 2 で負／負でない、bit 1 で 0 も
        cond = bits(opcode, 0x18, 8)
        if not cond & 8:
            c = "always"
        else:
            c = ("n" if cond & 4 else "!n") + (" || z" if cond & 2 else "")
        return "skip to %03x if %s" % ((pc & ~0xFF) | bits(opcode, 0x10, 8), c)

    mmode = bits(opcode, 0x16, 2)
    if mmode != 0:
        m1t = bits(opcode, 0x14, 2)
        mul1 = ("t%x" % t) if m1t in (1, 2) else gconst
        if bits(opcode, 0x13):
            mul1 = "exp(%s)" % mul1
        mul2 = (("m%02x" % sm) if sm else "0") if bits(opcode, 0x12) \
            else (("r%02x" % sr) if sr else "0")

        at = bits(opcode, 0x18, 2)
        if at == 0:
            aop = "p"
        elif at == 1:
            aop = ("r%02x" % sr) if sr else "(p>>15)"
        elif at == 2:
            aop = ("m%02x" % sm) if sm else "(p>>15)"
        else:
            aop = ""

        if mmode == 1:
            op = "(%s << 8)" % mul1
        elif mmode == 2:
            op = "%s * %s" % (mul1, mul2)
        else:
            op = mul2

        aopf = ""
        if at != 3:
            aopf = [" + %s", " - %s", " + abs(%s)", " & %s"][bits(opcode, 0x1A, 2)] % aop

        o = op + aopf
        shift = bits(opcode, 0x1C, 2)
        if shift:
            o = "(%s) << %d" % (o, 4 if shift == 3 else shift)
        add("p %s %s" % (["=", "=s", "=_", "=a"][bits(opcode, 0x1E, 2)], o))

    if dm:
        src = bits(opcode, 0x2D, 3)
        if src < 4:
            add("m%02x = lfo.%02x" % (dm, pc >> 4))
        elif src == 4:
            add("m%02x = mr" % dm)
        elif src == 5:
            add("m%02x = rand" % dm)
        elif src == 6:
            add("m%02x = p" % dm)
        else:
            add("m%02x = %s" % (dm, ("m%02x" % sm) if sm else "0"))

    if dr:
        add("r%02x = r%02x" % (dr, sr) if bits(opcode, 0x37) else "r%02x = p" % dr)
    if bits(opcode, 0x3D):
        add("mw = p")
    if bits(opcode, 0x3E):
        add("idx = p")
    if bits(opcode, 0x3B):
        add("t%x = p" % t if bits(opcode, 0x3C) else "t%x = %s" % (t, gconst))
    if bits(opcode, 0x0A):
        add("nodither")
    if bits(opcode, 0x20):
        add("flags")
    memmode = bits(opcode, 0x24, 2)
    if memmode:
        add("mem_%s +%x%s" % ([None, "w", "r", "1r"][memmode],
                              off.get(pc // 3, 0),
                              "+idx" if bits(opcode, 0x21) else ""))
    if opcode == 0:
        add("nop")
    return " ; ".join(r)


def listing(path):
    prg, const, off, lfo, mp, mix = load(path)
    out = []
    for pc in range(0x180):
        out.append((pc, prg.get(pc, 0), disasm(pc, prg.get(pc, 0), const, off)))
    return out, (prg, const, off, lfo, mp, mix)


def main():
    path = sys.argv[1]
    other = None
    quiet = "--all" not in sys.argv
    if "--diff" in sys.argv:
        other = sys.argv[sys.argv.index("--diff") + 1]

    a, (prg, const, off, lfo, mp, mix) = listing(path)
    if other:
        b, _ = listing(other)
        print("%-5s %-46s | %s" % ("pc", path, other))
        for (pc, oa, da), (_, ob, db) in zip(a, b):
            if oa == ob:
                continue
            print("%03x  %-46s | %s" % (pc, da or "-", db or "-"))
        return

    print("---- プログラム")
    for pc, op, d in a:
        if quiet and op == 0:
            continue
        print("%03x  %016x  %s" % (pc, op, d))
    print("---- 番地表 (offset)")
    print("  " + " ".join("%02x:%04x" % (k, v) for k, v in sorted(off.items()) if v))
    print("---- LFO")
    print("  " + " ".join("%02x:%04x" % (k, v) for k, v in sorted(lfo.items()) if v))
    print("---- map")
    print("  " + " ".join("%x:%04x" % (k, v) for k, v in sorted(mp.items())))
    print("---- ミキサ")
    for m in mix:
        print("  " + m)


main()
