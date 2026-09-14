#!/usr/bin/env python3
# license:BSD-3-Clause
"""fxsweep.exe の書き出し（インサーションの種類ごとのパラメータ）から src/xg/fx_params.h を作る。

  build/fxsweep.exe ../MU2000/roms > fxsweep.txt
  python tools/fxsweep/make_fx_params.py fxsweep.txt [fxsweep2.txt ...] src/xg/fx_params.h

書き出しを分けて流したときは、全部を並べて渡す（最後が出力）。

書き出しの形:
  T <msb> <lsb> <名前> [...]
  P <番地> <今の値> <下限> <上限> [<LCD の名前>] now=<表示>
  V <値> <LCD の表示>
"""
import re
import sys


def parse(paths):
    types = {}
    for path in paths:
        cur = None
        par = None
        for line in open(path, encoding='utf-8', errors='replace'):
            line = line.rstrip('\n')
            if line.startswith('T '):
                m = re.match(r'T (\w\w) (\w\w) (.*?) \[', line)
                cur = {'msb': int(m.group(1), 16), 'lsb': int(m.group(2), 16), 'name': m.group(3), 'params': []}
                types[(cur['msb'], cur['lsb'])] = cur
                par = None
            elif line.startswith('P ') and not line.startswith('P --'):
                m = re.match(r'P (\w\w) (\d+) (\d+) (\d+) \[(.*?)\]', line)
                par = {'addr': int(m.group(1), 16), 'lo': int(m.group(3)), 'hi': int(m.group(4)),
                       'label': m.group(5), 'values': []}
                cur['params'].append(par)
            elif line.startswith('V ') and par is not None:
                parts = line.split(' ', 2)
                par['values'].append((int(parts[1]), parts[2] if len(parts) > 2 else ''))
    return [types[k] for k in sorted(types)]


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    types = parse(sys.argv[1:-1])
    tables = {}          # 表示の並び → 名前
    order = []
    out_types = []
    for t in types:
        rows = []
        for p in t['params']:
            size = 2 if p['addr'] >= 0x30 else 1
            vals = p['values']
            span = p['hi'] - p['lo']
            fmt, table = 'raw', None
            if span > 127:
                if all(txt == '%.1f' % (v / 10) for v, txt in vals):
                    fmt = 'tenths'
            else:
                texts = [txt for v, txt in vals]
                if len(texts) == span + 1 and not all(re.fullmatch(r'0*%d' % v, txt) for v, txt in vals):
                    key = tuple(texts)
                    if key not in tables:
                        tables[key] = 'T%d' % len(tables)
                        order.append(key)
                    fmt, table = 'table', tables[key]
            rows.append((p['addr'], size, p['lo'], p['hi'], p['label'], fmt, table))
        out_types.append((t, rows))

    f = open(sys.argv[-1], 'w', encoding='utf-8', newline='\n')
    w = f.write
    w('// license:BSD-3-Clause\n//\n')
    w('// インサーションエフェクトの種類ごとのパラメータ（doc/pc-editor.md の「インサーションの設定の窓」）。\n')
    w('// **tools/fxsweep/make_fx_params.py が作ったもの。手で直さない。**\n//\n')
    w('// firmware の LCD の編集画面を 1 ページずつ見て、パラメータの名前、SysEx の番地（03 nn xx）、\n')
    w('// 受け付ける範囲、値ごとの表示を調べた（tools/fxsweep/fxsweep.cpp）。\n')
    w('// パラメータ 1-10 は、2 バイトの値を持つ種類（ディレイなど）では 30-43、ほかは 02-0B。11-16 は 20-25。\n\n')
    w('#ifndef S_MU2000_XG_FX_PARAMS_H\n#define S_MU2000_XG_FX_PARAMS_H\n\n#pragma once\n\n')
    w('#include "compat/mamecompat.h"\n\nnamespace xg {\n\n')
    w('enum class fx_fmt : u8 {\n\traw,        // 数のまま\n\ttable,      // texts[値 - lo]\n\ttenths,     // 10 分の 1（ミリ秒など）\n};\n\n')
    w('struct fx_param {\n\tu8 addr;             // 03 nn の後ろの番地\n\tu8 size;             // バイト数（7bit ずつ）\n')
    w('\tu16 lo, hi;          // firmware が受け付ける範囲\n\tconst char *label;   // LCD の名前\n\tfx_fmt fmt;\n\tconst char *const *texts;\n};\n\n')
    w('struct fx_def {\n\tu8 msb, lsb;         // 種類\n\tconst fx_param *params;\n\tint count;\n};\n\n')
    w('namespace fx_text {\n')
    for key in order:
        w('inline constexpr const char *const %s[] = {\n' % tables[key])
        for i in range(0, len(key), 8):
            w('\t' + ', '.join(c_str(s) for s in key[i:i + 8]) + ',\n')
        w('};\n')
    w('} // namespace fx_text\n\n')

    # 並びが同じ種類（HALL 1 と HALL 2 など）は 1 つの配列を分け合う
    lists = {}
    defs = []
    for t, rows in out_types:
        key = tuple(rows)
        if key not in lists:
            lists[key] = 'FX_%02X_%02X' % (t['msb'], t['lsb'])
            w('// %s\n' % t['name'])
            w('inline constexpr fx_param %s[] = {\n' % lists[key])
            for addr, size, lo, hi, label, fmt, table in rows:
                texts = 'fx_text::' + table if table else 'nullptr'
                w('\t{ 0x%02x, %d, %5d, %5d, %-14s fx_fmt::%-6s %s },\n' % (addr, size, lo, hi, c_str(label) + ',', fmt + ',', texts))
            if not rows:
                w('\t{ 0, 0, 0, 0, "", fx_fmt::raw, nullptr },\n')
            w('};\n')
        defs.append((t, lists[key], len(rows)))
    w('\ninline constexpr fx_def FX_DEFS[] = {\n')
    for t, name, n in defs:
        w('\t{ 0x%02x, 0x%02x, %-12s %2d },   // %s\n' % (t['msb'], t['lsb'], name + ',', n, t['name']))
    w('};\n\n')
    w('// 種類（MSB << 7 | LSB）から。表に無い LSB なら、同じ MSB の LSB 0 の並び。無ければ nullptr\n')
    w('inline const fx_def *fx_find(int type)\n{\n\tconst int msb = type >> 7, lsb = type & 0x7f;\n')
    w('\tfor (const fx_def &d : FX_DEFS)\n\t\tif (d.msb == msb && d.lsb == lsb)\n\t\t\treturn &d;\n')
    w('\tfor (const fx_def &d : FX_DEFS)\n\t\tif (d.msb == msb && d.lsb == 0)\n\t\t\treturn &d;\n\treturn nullptr;\n}\n\n')
    w('} // namespace xg\n\n#endif // S_MU2000_XG_FX_PARAMS_H\n')


if __name__ == '__main__':
    main()
