#!/usr/bin/env python3
"""SmartMedia の中身のファイル（gui で作る .img）と PC の間でファイルを出し入れする。

  smcard.py info カード.img
  smcard.py ls   カード.img [ディレクトリ]
  smcard.py get  カード.img カードの中の名前 [出す先]
  smcard.py put  カード.img PC のファイル [カードの中のディレクトリ]
  smcard.py rm   カード.img カードの中の名前

.img は NAND の生の並び（1 ページ 512 バイト + 予備 16 バイト、32 ページで 1 ブロック）。
その上に SSFDC の論理の並び（予備の領域のブロックの番地で、論理ブロックを物理ブロックへ割り当てる）、
さらにその上に MBR と FAT12/16 が載っている。書式化は本体の UTIL → CARD → Format で済ませておくこと。

書き込むときは、書き換えた論理ブロックを同じ物理ブロックへ書き戻す（まだ割り当てのない論理ブロックは、
同じゾーンの空いた物理ブロックに割り当てる）。ページごとに ECC を付け直す。
名前は 8.3 だけを扱う（長い名前は読むときに飛ばし、書くときは作らない）。
gui で差しているカードは書き換えないこと（gui が 2 秒ごとに書き戻すので、どちらかの書き込みが消える）。
"""

import os
import struct
import sys

PAGE = 512
SPARE = 16
PAGES_PER_BLOCK = 32
BLOCKS_PER_ZONE = 1024
LOGICAL_PER_ZONE = 1000


def ecc256(data):
    """256 バイトの ECC（3 バイト）。src/smartmedia.cpp の ecc256 と同じ並び"""
    reg1 = reg2 = reg3 = 0
    for j, b in enumerate(data):
        bit = [(b >> k) & 1 for k in range(8)]
        cp = ((bit[0] ^ bit[2] ^ bit[4] ^ bit[6]) |
              ((bit[1] ^ bit[3] ^ bit[5] ^ bit[7]) << 1) |
              ((bit[0] ^ bit[1] ^ bit[4] ^ bit[5]) << 2) |
              ((bit[2] ^ bit[3] ^ bit[6] ^ bit[7]) << 3) |
              ((bit[0] ^ bit[1] ^ bit[2] ^ bit[3]) << 4) |
              ((bit[4] ^ bit[5] ^ bit[6] ^ bit[7]) << 5))
        reg1 ^= cp
        if sum(bit) & 1:
            reg3 ^= j
            reg2 ^= (~j) & 0xff
    t1 = t2 = 0
    a = 0x80
    bm = 0x80
    for _ in range(4):
        if reg3 & a:
            t1 |= bm
        bm >>= 1
        if reg2 & a:
            t1 |= bm
        bm >>= 1
        a >>= 1
    bm = 0x80
    for _ in range(4):
        if reg3 & a:
            t2 |= bm
        bm >>= 1
        if reg2 & a:
            t2 |= bm
        bm >>= 1
        a >>= 1
    return bytes([(~t2) & 0xff, (~t1) & 0xff, (((~reg1) << 2) | 3) & 0xff])


def lba_code(lba):
    """予備の領域に書くブロックの番地。0x1000 | lba << 1 に、1 の数が偶数になる印を付ける"""
    v = 0x1000 | (lba << 1)
    if bin(v).count('1') & 1:
        v |= 1
    return v


class Card:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.raw = bytearray(f.read())
        per_block = PAGES_PER_BLOCK * (PAGE + SPARE)
        if len(self.raw) % per_block:
            raise SystemExit('%s: 大きさが SmartMedia の生の並びと合わない' % path)
        self.nblocks = len(self.raw) // per_block
        self.zones = max(1, self.nblocks // BLOCKS_PER_ZONE)
        self.map = {}          # 論理ブロック -> 物理ブロック
        self.free = {z: [] for z in range(self.zones)}
        self.cis = None
        for pb in range(self.nblocks):
            sp = self.spare(pb, 0)
            zone = pb // BLOCKS_PER_ZONE
            if sp == b'\xff' * SPARE:
                self.free[zone].append(pb)
                continue
            if sp[5] != 0xff:              # 悪いブロック
                continue
            code = (sp[6] << 8) | sp[7]
            if code == 0 and self.cis is None:
                self.cis = pb
                continue
            if (code & 0xf000) != 0x1000:
                continue
            lb = zone * LOGICAL_PER_ZONE + ((code >> 1) & 0x3ff)
            self.map.setdefault(lb, pb)
        self.dirty = {}        # 論理ブロック -> bytearray（ページの中身だけ、32 * 512）
        self.parse_fat()

    # ---- NAND と SSFDC

    def offset(self, pb, page):
        return (pb * PAGES_PER_BLOCK + page) * (PAGE + SPARE)

    def spare(self, pb, page):
        o = self.offset(pb, page) + PAGE
        return bytes(self.raw[o:o + SPARE])

    def block_data(self, lb):
        if lb in self.dirty:
            return self.dirty[lb]
        pb = self.map.get(lb)
        if pb is None:
            return bytearray(b'\xff' * PAGE * PAGES_PER_BLOCK)
        out = bytearray()
        for p in range(PAGES_PER_BLOCK):
            o = self.offset(pb, p)
            out += self.raw[o:o + PAGE]
        return out

    def read_sector(self, n):
        lb, p = divmod(n, PAGES_PER_BLOCK)
        d = self.block_data(lb)
        return bytes(d[p * PAGE:(p + 1) * PAGE])

    def write_sector(self, n, data):
        assert len(data) == PAGE
        lb, p = divmod(n, PAGES_PER_BLOCK)
        if lb not in self.dirty:
            self.dirty[lb] = bytearray(self.block_data(lb))
        self.dirty[lb][p * PAGE:(p + 1) * PAGE] = data

    def commit(self):
        for lb, data in sorted(self.dirty.items()):
            zone, lba = divmod(lb, LOGICAL_PER_ZONE)
            pb = self.map.get(lb)
            if pb is None:
                if not self.free[zone]:
                    raise SystemExit('カードのゾーン %d に空いたブロックが無い' % zone)
                pb = self.free[zone].pop(0)
                self.map[lb] = pb
            code = lba_code(lba)
            for p in range(PAGES_PER_BLOCK):
                page = bytes(data[p * PAGE:(p + 1) * PAGE])
                sp = bytearray(b'\xff' * SPARE)
                sp[6] = sp[11] = code >> 8
                sp[7] = sp[12] = code & 0xff
                sp[13:16] = ecc256(page[:256])
                sp[8:11] = ecc256(page[256:])
                o = self.offset(pb, p)
                self.raw[o:o + PAGE] = page
                self.raw[o + PAGE:o + PAGE + SPARE] = sp
        self.dirty.clear()
        with open(self.path, 'r+b') as f:
            f.write(self.raw)

    # ---- MBR と FAT

    def parse_fat(self):
        mbr = self.read_sector(0)
        if mbr[510:512] != b'\x55\xaa':
            raise SystemExit('%s: 書式化されていない（本体の UTIL → CARD → Format で書式化する）' % self.path)
        self.part = struct.unpack('<I', mbr[0x1c6:0x1ca])[0]
        bs = self.read_sector(self.part)
        (self.bps, self.spc, self.reserved, self.nfats, self.root_entries, total16, _media,
         self.fat_sectors) = struct.unpack('<HBHBHHBH', bs[11:24])
        total32 = struct.unpack('<I', bs[32:36])[0]
        self.total = total16 or total32
        if self.bps != PAGE:
            raise SystemExit('1 セクタが 512 バイトでない')
        self.fat_start = self.part + self.reserved
        self.root_start = self.fat_start + self.nfats * self.fat_sectors
        self.root_sectors = (self.root_entries * 32 + PAGE - 1) // PAGE
        self.data_start = self.root_start + self.root_sectors
        self.nclusters = (self.total - (self.data_start - self.part)) // self.spc
        self.fat12 = self.nclusters < 4085
        fat = bytearray()
        for i in range(self.fat_sectors):
            fat += self.read_sector(self.fat_start + i)
        self.fat = fat

    def fat_get(self, c):
        if self.fat12:
            o = c + c // 2
            v = self.fat[o] | (self.fat[o + 1] << 8)
            return (v >> 4) if (c & 1) else (v & 0xfff)
        return struct.unpack_from('<H', self.fat, c * 2)[0]

    def fat_set(self, c, v):
        if self.fat12:
            o = c + c // 2
            cur = self.fat[o] | (self.fat[o + 1] << 8)
            if c & 1:
                cur = (cur & 0x000f) | ((v & 0xfff) << 4)
            else:
                cur = (cur & 0xf000) | (v & 0xfff)
            self.fat[o] = cur & 0xff
            self.fat[o + 1] = cur >> 8
        else:
            struct.pack_into('<H', self.fat, c * 2, v & 0xffff)

    def is_end(self, v):
        return v >= (0xff8 if self.fat12 else 0xfff8)

    def end_mark(self):
        return 0xfff if self.fat12 else 0xffff

    def chain(self, c):
        out = []
        while 2 <= c < self.nclusters + 2 and c not in out:
            out.append(c)
            n = self.fat_get(c)
            if self.is_end(n) or n == 0:
                break
            c = n
        return out

    def cluster_sectors(self, c):
        s = self.data_start + (c - 2) * self.spc
        return range(s, s + self.spc)

    def flush_fat(self):
        for n in range(self.nfats):
            for i in range(self.fat_sectors):
                self.write_sector(self.fat_start + n * self.fat_sectors + i,
                                  bytes(self.fat[i * PAGE:(i + 1) * PAGE]))

    # ディレクトリは (セクタ番号の並び) で表す。None は根
    def dir_sectors(self, first_cluster):
        if first_cluster is None:
            return list(range(self.root_start, self.root_start + self.root_sectors))
        return [s for c in self.chain(first_cluster) for s in self.cluster_sectors(c)]

    def entries(self, first_cluster):
        for s in self.dir_sectors(first_cluster):
            sec = self.read_sector(s)
            for i in range(0, PAGE, 32):
                e = sec[i:i + 32]
                if e[0] == 0:
                    return
                yield s, i, e

    @staticmethod
    def entry_name(e):
        base = e[0:8].decode('cp932', 'replace').rstrip()
        ext = e[8:11].decode('cp932', 'replace').rstrip()
        return base + ('.' + ext if ext else '')

    def listdir(self, first_cluster):
        out = []
        for s, i, e in self.entries(first_cluster):
            if e[0] == 0xe5 or e[11] == 0x0f or (e[11] & 0x08):
                continue
            name = self.entry_name(e)
            if name in ('.', '..'):
                continue
            cl = struct.unpack('<H', e[26:28])[0]
            size = struct.unpack('<I', e[28:32])[0]
            out.append((name, bool(e[11] & 0x10), cl, size, s, i))
        return out

    def lookup(self, path):
        """カードの中の名前から (名前, ディレクトリか, 先頭クラスタ, 大きさ, セクタ, 位置)。根は None"""
        parts = [p for p in path.replace('\\', '/').split('/') if p]
        cur = None
        found = None
        for k, p in enumerate(parts):
            hit = [x for x in self.listdir(cur) if x[0].upper() == p.upper()]
            if not hit:
                raise SystemExit('カードに無い: %s' % path)
            found = hit[0]
            if k + 1 < len(parts):
                if not found[1]:
                    raise SystemExit('ディレクトリでない: %s' % p)
                cur = found[2]
        return found

    def dir_cluster(self, path):
        if not path or path in ('/', '\\', '.'):
            return None
        e = self.lookup(path)
        if not e[1]:
            raise SystemExit('ディレクトリでない: %s' % path)
        return e[2]

    def read_file(self, entry):
        data = bytearray()
        for c in self.chain(entry[2]):
            for s in self.cluster_sectors(c):
                data += self.read_sector(s)
        return bytes(data[:entry[3]])

    def free_space(self):
        return sum(1 for c in range(2, self.nclusters + 2) if self.fat_get(c) == 0) * self.spc * PAGE


def short_name(name):
    base, ext = os.path.splitext(os.path.basename(name))
    ext = ext[1:]
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-!#$%&\'()@^`{}~')
    base = ''.join(ch for ch in base.upper() if ch in ok)[:8]
    ext = ''.join(ch for ch in ext.upper() if ch in ok)[:3]
    if not base:
        raise SystemExit('8.3 の名前にできない: %s' % name)
    return base, ext


def cmd_info(card):
    kind = 'FAT12' if card.fat12 else 'FAT16'
    print('%s: %d MB、%s、クラスタ %d バイト × %d、空き %.2f MB' % (
        card.path, card.nblocks * PAGES_PER_BLOCK * PAGE // (1024 * 1024), kind,
        card.spc * PAGE, card.nclusters, card.free_space() / (1024 * 1024)))


def cmd_ls(card, path):
    for name, is_dir, _cl, size, _s, _i in card.listdir(card.dir_cluster(path)):
        print('%-12s %s' % (name, '<DIR>' if is_dir else '%10d' % size))


def cmd_get(card, path, out):
    e = card.lookup(path)
    if e[1]:
        raise SystemExit('ディレクトリは出せない: %s' % path)
    out = out or e[0]
    if os.path.isdir(out):
        out = os.path.join(out, e[0])
    with open(out, 'wb') as f:
        f.write(card.read_file(e))
    print('出した: %s（%d バイト）' % (out, e[3]))


def remove_entry(card, entry):
    for c in card.chain(entry[2]):
        card.fat_set(c, 0)
    sec = bytearray(card.read_sector(entry[4]))
    sec[entry[5]] = 0xe5
    card.write_sector(entry[4], bytes(sec))


def cmd_rm(card, path):
    e = card.lookup(path)
    if e[1]:
        raise SystemExit('ディレクトリは消さない: %s' % path)
    remove_entry(card, e)
    card.flush_fat()
    card.commit()
    print('消した: %s' % path)


def cmd_put(card, src, dest):
    with open(src, 'rb') as f:
        data = f.read()
    base, ext = short_name(src)
    name = base + ('.' + ext if ext else '')
    dcl = card.dir_cluster(dest)
    for x in card.listdir(dcl):
        if x[0].upper() == name:
            if x[1]:
                raise SystemExit('同じ名前のディレクトリがある: %s' % name)
            remove_entry(card, x)
    # 空いた場所を探す（根は広げられない。サブディレクトリは広げない）
    slot = None
    for s in card.dir_sectors(dcl):
        sec = card.read_sector(s)
        for i in range(0, PAGE, 32):
            if sec[i] in (0x00, 0xe5):
                slot = (s, i)
                break
        if slot:
            break
    if not slot:
        raise SystemExit('ディレクトリがいっぱい')
    per_cluster = card.spc * PAGE
    need = max(1, (len(data) + per_cluster - 1) // per_cluster)
    free = [c for c in range(2, card.nclusters + 2) if card.fat_get(c) == 0][:need]
    if len(free) < need:
        raise SystemExit('カードの空きが足りない（%d バイト要る）' % len(data))
    for k, c in enumerate(free):
        card.fat_set(c, free[k + 1] if k + 1 < len(free) else card.end_mark())
        chunk = data[k * per_cluster:(k + 1) * per_cluster]
        chunk = chunk + b'\x00' * (per_cluster - len(chunk))
        for j, s in enumerate(card.cluster_sectors(c)):
            card.write_sector(s, chunk[j * PAGE:(j + 1) * PAGE])
    e = bytearray(32)
    e[0:8] = base.encode('ascii').ljust(8)
    e[8:11] = ext.encode('ascii').ljust(3)
    e[11] = 0x20
    struct.pack_into('<H', e, 26, free[0] if data else 0)
    struct.pack_into('<I', e, 28, len(data))
    sec = bytearray(card.read_sector(slot[0]))
    sec[slot[1]:slot[1] + 32] = e
    card.write_sector(slot[0], bytes(sec))
    card.flush_fat()
    card.commit()
    print('入れた: %s → %s/%s（%d バイト）' % (src, dest or '', name, len(data)))


def main(argv):
    if len(argv) < 3 or argv[1] not in ('info', 'ls', 'get', 'put', 'rm'):
        sys.stdout.write(__doc__)
        return 1
    card = Card(argv[2])
    if argv[1] == 'info':
        cmd_info(card)
    elif argv[1] == 'ls':
        cmd_ls(card, argv[3] if len(argv) > 3 else '')
    elif argv[1] == 'get' and len(argv) > 3:
        cmd_get(card, argv[3], argv[4] if len(argv) > 4 else '')
    elif argv[1] == 'put' and len(argv) > 3:
        cmd_put(card, argv[3], argv[4] if len(argv) > 4 else '')
    elif argv[1] == 'rm' and len(argv) > 3:
        cmd_rm(card, argv[3])
    else:
        sys.stdout.write(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
