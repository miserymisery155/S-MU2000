// license:BSD-3-Clause
//
// インサーションの種類ごとのパラメータを、firmware の LCD から調べる（doc/pc-editor.md）。
// 種類ごとにパネルのボタンで編集画面に入り、1 ページずつ
//   * どの番地か（2 バイトの 30-43、1 バイトの 02-0B、20-25 の順に、0 と最大を書いて LCD が変わる所）
//   * firmware が受け付ける範囲（0 と最大を書いて RAM で読む）
//   * 値ごとの LCD の表示
// を書き出す。RAM: 塊の +0x02 から 1 バイトの値、+0x18 からパラメータ 1-10 の 16bit、+0x12 から 11-16。
// 種類は ROM の種類の表（xg/fx_types.h）の全部（LSB 違いも 1 つずつ）。
// 使い方: build/fxsweep.exe <rom ディレクトリ> [何番目から 何番目の前まで] > fxsweep.txt
// 1 つの種類に 2 分ほど。範囲を分けて並べて流せる
#include "mu2000.h"
#include "xg/fx_types.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static mu2000 mu;
static void pump(int ms) { s32 l, r; for (int i = 0; i < 44100 * ms / 1000; i++) { mu.run_sample(l, r); u8 b; while (mu.midi_out_take(b)) {} } }
static void send(const std::vector<u8> &m) { for (u8 b : m) mu.midi_in(b, 0); }
static std::string line2()
{
	const u8 *dd = mu.lcd().ddram();
	std::string s;
	for (int c = 25; c < 40; c++) { const u8 ch = dd[40 + c]; s += (ch >= 32 && ch < 127) ? char(ch) : '.'; }
	while (!s.empty() && s.back() == ' ') s.pop_back();
	return s;
}
using B = mu2000::button;
static void press(B b) { mu.set_button(b, true); pump(80); mu.set_button(b, false); pump(250); }
static const u32 INS1 = 0x0cb7e;

// 番地の読み書き。addr が 0x30 以上なら 2 バイト
static int ram_value(int addr)
{
	const auto &n = mu.nvram();
	if (addr >= 0x30) { const int i = (addr - 0x30) / 2; return n[INS1 + 0x18 + 2 * i] << 8 | n[INS1 + 0x19 + 2 * i]; }
	if (addr >= 0x20) return n[INS1 + 0x12 + (addr - 0x20)];
	return n[INS1 + addr];
}
static void put(int addr, int v, int ms)
{
	if (addr >= 0x30) send({ 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x00, u8(addr), u8((v >> 7) & 0x7f), u8(v & 0x7f), 0xf7 });
	else              send({ 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x00, u8(addr), u8(v & 0x7f), 0xf7 });
	pump(ms);
}
static std::string value_text(const std::string &s)
{
	const size_t k = s.find('=');
	std::string v = k == std::string::npos ? s : s.substr(k + 1);
	while (!v.empty() && v[0] == ' ') v.erase(0, 1);
	return v;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "fxsweep <rom ディレクトリ> [何番目から 何番目の前まで]\n");
		return 1;
	}
	const std::string dir = argv[1]; s32 l, r;
	mu.load_program(dir + "/mu2000_flash.bin"); mu.load_wave(dir + "/dump"); mu.load_sintab(dir + "/standin/sin-table.bin");
	mu.reset();
	for (int s = 0; s < 30 * 44100 && !mu.midi_ready(); s++) mu.run_sample(l, r);
	pump(1500);
	{
		std::ifstream f(dir + "/mu2000_flash.bin", std::ios::binary);
		const std::vector<u8> rom((std::istreambuf_iterator<char>(f)), {});
		if (!xg::set_fx_type_rom(rom)) {
			std::fprintf(stderr, "ROM の種類の表が読めない\n");
			return 1;
		}
	}
	const int first = argc > 2 ? std::stoi(argv[2]) : 0;
	const int last  = argc > 3 ? std::stoi(argv[3]) : int(xg::ins_types().size());

	for (int index = first; index < last && index < int(xg::ins_types().size()); index++) {
		const xg::fx_type &t = xg::ins_types()[index];
		if (t.msb == 0 || t.msb == 0x40) continue;
		send({ 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x00, 0x00, t.msb, t.lsb, 0xf7 }); pump(300);
		for (int i = 0; i < 4; i++) press(B::exit);
		press(B::effect);
		for (int i = 0; i < 5; i++) press(B::select_left);
		for (int i = 0; i < 3; i++) press(B::select_right);
		press(B::enter);
		for (int i = 0; i < 4; i++) press(B::select_left);
		press(B::enter);
		// 種類のページまで戻る。「Early Type=」「AmpType=」などのパラメータと取り違えないよう、頭で見る
		auto on_type_page = []() { const std::string s = line2(); return s.rfind("Type=", 0) == 0 || s.rfind(".Type=", 0) == 0; };
		for (int i = 0; i < 20 && !on_type_page(); i++) press(B::select_left);
		std::printf("T %02x %02x %s [%s]\n", t.msb, t.lsb, t.name, line2().c_str());
		std::fflush(stdout);

		std::string last;
		std::vector<int> used;
		for (int page = 1; page < 20; page++) {
			press(B::select_right);
			const std::string base = line2();
			if (base == last) break;
			last = base;
			std::string label = base.substr(0, base.find('='));
			if (!label.empty() && label[0] == '.') label.erase(0, 1);
			while (!label.empty() && label.back() == ' ') label.pop_back();
			if (label.empty() || label.find("Part") != std::string::npos || label.find("Ctrl") != std::string::npos) break;

			// 番地を探す
			std::vector<int> cands;
			for (int a = 0x30; a <= 0x42; a += 2) cands.push_back(a);
			for (int a = 0x02; a <= 0x0b; a++) cands.push_back(a);
			for (int a = 0x20; a <= 0x25; a++) cands.push_back(a);
			int found = -1;
			for (int a : cands) {
				bool skip = false;
				for (int u : used) skip |= u == a;
				if (skip) continue;
				const int orig = ram_value(a);
				put(a, 0, 150);
				const std::string t0 = line2();
				put(a, a >= 0x30 ? 0x3fff : 127, 150);
				const std::string t1 = line2();
				if (t0 != base || t1 != base) { found = a; put(a, orig, 150); break; }
				put(a, orig, 150);
			}
			if (found < 0) { std::printf("P -- [%s] now=%s\n", label.c_str(), base.c_str()); std::fflush(stdout); continue; }
			used.push_back(found);
			const int orig = ram_value(found);
			put(found, 0, 150);
			const int lo = ram_value(found);
			put(found, found >= 0x30 ? 0x3fff : 127, 150);
			const int hi = ram_value(found);
			std::printf("P %02x %d %d %d [%s] now=%s\n", found, orig, lo, hi, label.c_str(), value_text(base).c_str());
			// 値ごとの表示。128 を超えるものは間引く
			const int span = hi - lo;
			const int step = span <= 127 ? 1 : (span + 99) / 100;
			std::string prev;
			for (int v = lo; v <= hi; v += step) {
				put(found, v, 90);
				std::printf("V %d %s\n", v, value_text(line2()).c_str());
			}
			if (span > 127) { put(found, hi, 90); std::printf("V %d %s\n", hi, value_text(line2()).c_str()); }
			put(found, orig, 150);
			std::fflush(stdout);
		}
	}
	return 0;
}
