// license:BSD-3-Clause
//
// SMF の読み込み。smf.h の説明を参照。

#include "smf.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace smf {

namespace {
u32 be32(const u8 *p) { return (u32(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
u16 be16(const u8 *p) { return u16((p[0] << 8) | p[1]); }
} // namespace

// SMF を (秒, バイト列) の並びに開く。format 0/1 の両方に対応する
bool load(const std::string &path, std::vector<event> &out, std::string &err)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) { err = "MIDI ファイルを開けない: " + path; return false; }
	std::fseek(f, 0, SEEK_END);
	std::vector<u8> d(size_t(std::ftell(f)));
	std::fseek(f, 0, SEEK_SET);
	if (std::fread(d.data(), 1, d.size(), f) != d.size()) {
		std::fclose(f); err = "MIDI ファイルを読めない"; return false;
	}
	std::fclose(f);

	if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4)) {
		err = "MThd がない。標準 MIDI ファイルではないらしい"; return false;
	}
	const u16 ntrk = be16(&d[10]);
	const u16 div  = be16(&d[12]);
	if (div & 0x8000) { err = "SMPTE 単位の MIDI には未対応"; return false; }

	// まずは全トラックを (tick, バイト列) で集める
	struct raw { u64 tick; std::vector<u8> bytes; bool tempo; u32 usec; u8 port; };
	std::vector<raw> all;

	size_t pos = 8 + be32(&d[4]);
	for (u16 t = 0; t < ntrk && pos + 8 <= d.size(); t++) {
		if (std::memcmp(&d[pos], "MTrk", 4)) break;
		const size_t len = be32(&d[pos + 4]);
		size_t p = pos + 8;
		const size_t end = std::min(p + len, d.size());
		pos = p + len;

		u64 tick = 0;
		u8  running = 0;
		// トラックごとの出し先。`FF 21 01 pp` で決まる。無ければ 0。
		// 昔の `FF 04`（機器名）でポートを言う流儀もあるが、そちらは見ない
		u8  port = 0;
		while (p < end) {
			u64 delta = 0;                       // 可変長
			while (p < end) {
				delta = (delta << 7) | (d[p] & 0x7f);
				if (!(d[p++] & 0x80)) break;
			}
			tick += delta;
			if (p >= end) break;

			u8 status = d[p];
			if (status < 0x80) status = running;  // ランニングステータス
			else p++;

			if (status == 0xff) {                 // メタイベント
				const u8 type = d[p++];
				u64 l = 0;
				while (p < end) { l = (l << 7) | (d[p] & 0x7f); if (!(d[p++] & 0x80)) break; }
				if (type == 0x51 && l == 3)
					all.push_back({ tick, {}, true,
					                (u32(d[p]) << 16) | (d[p+1] << 8) | d[p+2], 0 });
				if (type == 0x21 && l == 1)
					port = d[p];
				if (type == 0x09 && l >= 1 && l <= 32) {
					// 機器名で口を言う流儀。「A」〜「D」か「Port 1」〜「Port 4」（大文字小文字は問わない）だけ見る
					std::string name(d.begin() + p, d.begin() + std::min(p + size_t(l), end));
					while (!name.empty() && (name.back() == ' ' || name.back() == 0)) name.pop_back();
					for (char &c : name) c = char(std::tolower(u8(c)));
					if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'd')
						port = u8(name[0] - 'a');
					else if (name.size() == 6 && name.compare(0, 5, "port ") == 0 && name[5] >= '1' && name[5] <= '4')
						port = u8(name[5] - '1');
				}
				p += size_t(l);
				continue;
			}
			if (status == 0xf0 || status == 0xf7) {   // システムエクスクルーシブ
				u64 l = 0;
				while (p < end) { l = (l << 7) | (d[p] & 0x7f); if (!(d[p++] & 0x80)) break; }
				std::vector<u8> b;
				if (status == 0xf0) b.push_back(0xf0);
				b.insert(b.end(), d.begin() + p, d.begin() + std::min(p + size_t(l), end));
				p += size_t(l);
				all.push_back({ tick, std::move(b), false, 0, port });
				continue;
			}

			running = status;
			const int n = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 1 : 2;
			std::vector<u8> b{ status };
			for (int i = 0; i < n && p < end; i++) b.push_back(d[p++]);
			all.push_back({ tick, std::move(b), false, 0, port });
		}
	}

	std::stable_sort(all.begin(), all.end(),
	                 [](const raw &a, const raw &b) { return a.tick < b.tick; });

	// テンポを追いながら秒に直す
	double sec = 0.0, us_per_beat = 500000.0;   // 既定は 120 BPM
	u64 last = 0;
	for (const raw &e : all) {
		sec += double(e.tick - last) * us_per_beat / (div * 1e6);
		last = e.tick;
		if (e.tempo) { us_per_beat = e.usec; continue; }
		out.push_back({ sec, e.bytes, e.port });
	}
	return true;
}


} // namespace smf
