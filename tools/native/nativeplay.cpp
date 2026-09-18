// license:BSD-3-Clause
//
// **SH-2 を止めたまま音を出す**（doc/native-engine.md の段 2）。
//
// やること:
//   1. firmware で起動して、SWP30 を実機と同じ初期状態にする
//   2. CPU を止める（ここから先、firmware は 1 命令も走らない）
//   3. 音色の記録から xg::nv::build_note でレジスタを組み立て、スロットに書いて鳴らす
//   4. WAV に書き出す
//
// 使い方:
//   build/nativeplay.exe <rom ディレクトリ> <出力.wav> [-b msb,lsb,prog] [-n 鍵] [-v 強さ]
//                        [-s 秒] [--firmware]
//   --firmware を付けると、比べるための「firmware に鳴らさせた版」を出す。
#include "compat/platform.h"
#include "mu2000.h"
#include "xg/native_voice.h"
#include "xg/ram.h"
#include "xg/voices.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {

void put32(std::vector<u8> &v, u32 x)
{ v.push_back(u8(x)); v.push_back(u8(x >> 8)); v.push_back(u8(x >> 16)); v.push_back(u8(x >> 24)); }
void put16(std::vector<u8> &v, u16 x) { v.push_back(u8(x)); v.push_back(u8(x >> 8)); }

bool write_wav(const std::string &path, const std::vector<s16> &pcm, u32 rate)
{
	std::vector<u8> h;
	const u32 bytes = u32(pcm.size()) * 2;
	h.insert(h.end(), { 'R', 'I', 'F', 'F' }); put32(h, 36 + bytes);
	h.insert(h.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' }); put32(h, 16);
	put16(h, 1); put16(h, 2); put32(h, rate); put32(h, rate * 4); put16(h, 4); put16(h, 16);
	h.insert(h.end(), { 'd', 'a', 't', 'a' }); put32(h, bytes);
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	std::fwrite(h.data(), 1, h.size(), f);
	std::fwrite(pcm.data(), 2, pcm.size(), f);
	std::fclose(f);
	return true;
}

// スロット 1 つぶんのレジスタを SWP30 へ入れる
void poke_slot(mu2000 &mu, int slot, const xg::nv::slot_regs &r)
{
	for (int i = 0; i < 0x40; i++)
		if (r.write & (u64(1) << i))
			mu.poke_swp(true, u32(slot) * 64 + u32(i), r.v[i]);
}

// まとめて keyon する
void key_on_mask(mu2000 &mu, u64 mask)
{
	const u32 MASK_REG[4] = { 0x1cf, 0x1ce, 0x18f, 0x18e };
	for (int i = 0; i < 4; i++)
		mu.poke_swp(true, MASK_REG[i], u16((mask >> (i * 16)) & 0xffff));
	mu.poke_swp(true, 0x20e, 1);
}

// keyon のマスクを立てて引き金を引く
void key_on(mu2000 &mu, int slot)
{
	const u32 MASK_REG[4] = { 0x1cf, 0x1ce, 0x18f, 0x18e };   // 0-15 / 16-31 / 32-47 / 48-63
	for (int i = 0; i < 4; i++)
		mu.poke_swp(true, MASK_REG[i], (slot >> 4) == i ? u16(1 << (slot & 15)) : 0);
	mu.poke_swp(true, 0x20e, 1);
}

// firmware に 1 音鳴らしてもらって、そのときのレジスタを写し取る。
// 要素が複数ある音色では、鳴ったスロットの数だけ返す（スロット番号の順）
std::vector<xg::nv::voice_cal> take_cals(mu2000 &mu, const u8 *rom, u32 rec,
                                         int note, int vel, u32 rate)
{
	std::vector<xg::nv::voice_cal> out;
	std::map<u32, u16> latest, seen;      // latest = 最後の値、seen = 引き金を引いた瞬間の写し
	u64 mask = 0, keyed = 0;
	mu.set_swp_watch([&](bool master, u32 reg, u16 value) {
		if (!master) return;
		latest[reg] = value;
		switch (reg) {
		case 0x18e: mask = (mask & ~(u64(0xffff) << 48)) | (u64(value) << 48); break;
		case 0x18f: mask = (mask & ~(u64(0xffff) << 32)) | (u64(value) << 32); break;
		case 0x1ce: mask = (mask & ~(u64(0xffff) << 16)) | (u64(value) << 16); break;
		case 0x1cf: mask = (mask & ~u64(0xffff)) | value; break;
		// 引き金を引いた瞬間が「鳴らすときの値」。そのあとの書き直しは
		// LFO や包絡線の更新なので、写し取りには使わない
		case 0x20e: keyed |= mask; if (seen.empty()) seen = latest; break;
		default: break;
		}
	});
	s32 l = 0, r = 0;
	for (u8 b : { u8(0x90), u8(note & 0x7f), u8(vel & 0x7f) })
		mu.midi_in(b, 0);
	for (u32 i = 0; i < rate / 25; i++)
		mu.run_sample(l, r);
	mu.set_swp_watch(nullptr);
	for (int ch = 0; ch < 64; ch++) {
		if (!(keyed & (u64(1) << ch)))
			continue;
		xg::nv::voice_cal cal;
		// 鳴らす値として何を採るか。
		//   0x05・0x0a・0x11 は LFO の更新で動き続けるので**引き金の瞬間**
		//   ほかは、鳴らしたあとに落ち着いた値（0x01 は鳴らしてから上がっていく）
		for (int i = 0; i < 0x40; i++) {
			const bool at_key = (i == 0x05 || i == 0x0a || i == 0x11);
			const std::map<u32, u16> &src = at_key ? seen : latest;
			const auto it = src.find(u32(ch) * 64 + u32(i));
			if (it != src.end())
				cal.set(i, it->second);
		}
		// **要素ごとに**音量を校正する。そのスロットに書かれた 0x09 が、
		// firmware がその要素に使った減衰そのもの。
		// どの要素かは、そのスロットが鳴らしている波形の番地で見分ける
		const int nel = xg::nv::element_count(rom, rec);
		int idx = -1;
		if (cal.has(0x16) && cal.has(0x17)) {
			const u32 want = u32(cal.reg[0x16]) << 16 | cal.reg[0x17];
			for (int k = 0; k < nel; k++) {
				const u8 *e2 = xg::nv::element(rom, rec, k);
				const u8 *w2 = xg::nv::wave_entry(rom, xg::nv::wave_set(e2), note);
				if (w2 && xg::nv::read_wave(w2).format_addr == want) { idx = k; break; }
			}
		}
		if (idx < 0) {                      // 見分けられなければ順番で
			int cnt = int(out.size());
			idx = 0;
			for (int k = 0; k < nel; k++) {
				const u8 *e2 = xg::nv::element(rom, rec, k);
				if (!xg::nv::element_active(e2, note, vel))
					continue;
				if (cnt-- == 0) { idx = k; break; }
			}
		}
		const u8 *el = xg::nv::element(rom, rec, idx);
		cal.base_level = xg::nv::calibrate_level(rom, el,
		    cal.has(9) ? (cal.reg[9] & 0xff) : mu.nvram()[0x3e96a], note, vel);
		cal.have = true;
		out.push_back(cal);
	}
	return out;
}

// 1 つだけ要るとき
xg::nv::voice_cal take_cal(mu2000 &mu, const u8 *rom, u32 rec,
                           int note, int vel, u32 rate)
{
	const std::vector<xg::nv::voice_cal> v = take_cals(mu, rom, rec, note, vel, rate);
	return v.empty() ? xg::nv::voice_cal() : v[0];
}

} // namespace


int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "nativeplay <rom ディレクトリ> <出力.wav> [-b msb,lsb,prog] [-n 鍵] [-v 強さ] [-s 秒] [--firmware]%c", 10);
		return 1;
	}
	const std::string dir = argv[1], out_path = argv[2];
	int msb = 0, lsb = 0, prog = 0, note = 60, vel = 100, slot = 0;
	double seconds = 2.0;
	bool firmware = false, compare = false, song = false, dump_voice = false, copyall = false;
	int sweep = 0, volsweep = 0, levels = 0;
	bool bench = false;
	for (int i = 3; i < argc; i++) {
		if (!std::strcmp(argv[i], "-b") && i + 1 < argc)
			std::sscanf(argv[++i], "%d,%d,%d", &msb, &lsb, &prog);
		else if (!std::strcmp(argv[i], "-n") && i + 1 < argc) note = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "-v") && i + 1 < argc) vel = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "-s") && i + 1 < argc) seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--slot") && i + 1 < argc) slot = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--firmware")) firmware = true;
		else if (!std::strcmp(argv[i], "--compare")) compare = true;
		else if (!std::strcmp(argv[i], "--song")) song = true;
		else if (!std::strcmp(argv[i], "--dump-voice")) dump_voice = true;
		else if (!std::strcmp(argv[i], "--copyall")) copyall = true;
		else if (!std::strcmp(argv[i], "--bench")) bench = true;
		else if (!std::strcmp(argv[i], "--sweep") && i + 1 < argc) sweep = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--volsweep") && i + 1 < argc) volsweep = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--levels") && i + 1 < argc) levels = std::atoi(argv[++i]);
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin") || !mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s%c", mu.error().c_str(), 10);
		return 1;
	}
	mu.load_sintab(dir + "/standin/sin-table.bin");
	mu.reset();

	const u32 RATE = 44100;
	s32 l = 0, r = 0;
	for (u32 i = 0; i < 30 * RATE && !mu.midi_ready(); i++)
		mu.run_sample(l, r);
	for (u32 i = 0; i < RATE; i++)                     // 起動が落ち着くまで
		mu.run_sample(l, r);

	// 音色を選ぶ。**ここは firmware に頼る**（バンクとプログラムの引き方は
	// xg/voices.h にあるが、パートの塊を作るのは firmware の仕事）
	for (u8 b : { u8(0xb0), u8(0x00), u8(msb & 0x7f), u8(0xb0), u8(0x20), u8(lsb & 0x7f),
	              u8(0xc0), u8(prog & 0x7f) })
		mu.midi_in(b, 0);
	for (u32 i = 0; i < RATE / 2; i++)
		mu.run_sample(l, r);

	const u8 *ram = mu.nvram().data();
	const u32 part0 = xg::ram::part_base(0);
	const u8 *pr = ram + part0;
	const u32 rec = u32(pr[xg::ram::PART_VOICE]) << 24 | u32(pr[xg::ram::PART_VOICE + 1]) << 16 |
	                u32(pr[xg::ram::PART_VOICE + 2]) << 8 | pr[xg::ram::PART_VOICE + 3];
	if (!rec) {
		std::fprintf(stderr, "音色の記録が引けなかった（ドラムか、まだ真似していない組）%c", 10);
		return 1;
	}
	const u8 *rom = mu.program_rom()->data();
	xg::voice_rom vr(mu.program_rom());
	std::printf("音色 %d,%d,%d = %s（記録 %06x、要素 %d）%c", msb, lsb, prog,
	            vr.record_name(rec).c_str(), rec, xg::nv::element_count(rom, rec), 10);

	// --dump-voice: firmware に 1 音鳴らさせて、声の構造体をワーク RAM から探す。
	// 音量の式の残り（+119・+120・level）を目で見るため
	if (dump_voice) {
		for (u8 b : { u8(0x90), u8(note & 0x7f), u8(vel & 0x7f) })
			mu.midi_in(b, 0);
		for (u32 i = 0; i < RATE / 50; i++)
			mu.run_sample(l, r);
		const std::vector<u8> &wr = mu.nvram();
		const int att = wr[0x3e96a];
		const int b119 = xg::nv::velocity_att(rom, vel);
		std::printf("ram[0x43E96A] = %d（att）。強さのぶん = %d、att/2 - それ = %d%c",
		            att, b119, att / 2 - b119, 10);
		int shown = 0;
		for (size_t base = 0; base + 130 < wr.size() && shown < 12; base++) {
			if (wr[base + 119] != b119)
				continue;
			// 声の塊らしさ: +118 が 1-127、+120 が 0-63、そして
			// +32 と +44 が「ワーク RAM か ROM を指すポインタ」であること
			if (wr[base + 118] == 0 || wr[base + 118] > 127 || wr[base + 120] > 63)
				continue;
			auto ptr = [&](size_t o) {
				return u32(wr[base + o]) << 24 | u32(wr[base + o + 1]) << 16 |
				       u32(wr[base + o + 2]) << 8 | wr[base + o + 3];
			};
			const u32 p32 = ptr(32), p44 = ptr(44);
			auto plausible = [](u32 a) {
				return (a >= 0x400000 && a < 0x440000) || (a >= 0x200000 && a < 0x400000);
			};
			if (!plausible(p32) || !plausible(p44))
				continue;
			std::printf("  +32=%08x +44=%08x%c", p32, p44, 10);
			std::printf("  候補 %06zx: +117=%3d +118=%3d +119=%3d +120=%3d +121=%3d  |  +2=%3d +29=%3d%c",
			            0x400000 + base, wr[base + 117], wr[base + 118], wr[base + 119],
			            wr[base + 120], wr[base + 121], wr[base + 2], wr[base + 29], 10);
			shown++;
		}
		return 0;
	}

	// --bench: 「firmware を走らせたまま」と「SH-2 を止めて」で、
	// 1 サンプルを作るのに何ナノ秒かかるかを測る。これが軽量化の答え
	if (bench) {
		const u8 *elem0 = xg::nv::element(rom, rec, 0);
		const int nelem = xg::nv::element_count(rom, rec);
		const std::vector<u8> base = mu.save_state();
		const u32 SECS = 6;
		auto run = [&](bool cpu_on, int voices) {
			std::string e2;
			mu.load_state(base.data(), base.size(), e2);
			mu.set_cpu_enabled(true);
			// 声を鳴らす
			if (cpu_on) {
				for (int v = 0; v < voices; v++)
					for (u8 b : { u8(0x90), u8(48 + v * 2), u8(vel & 0x7f) })
						mu.midi_in(b, 0);
				for (u32 i = 0; i < RATE / 10; i++) { s32 a = 0, b2 = 0; mu.run_sample(a, b2); }
			} else {
				const std::vector<xg::nv::voice_cal> cs = take_cals(mu, rom, rec, 60, vel, RATE);
				mu.load_state(base.data(), base.size(), e2);
				mu.set_cpu_enabled(false);
				u64 km = 0;
				int slot = 0;
				for (int v = 0; v < voices; v++) {
					for (int k = 0; k < nelem && slot < 60; k++) {
						const u8 *el = xg::nv::element(rom, rec, k);
						if (!xg::nv::element_active(el, 48 + v * 2, vel))
							continue;
						const xg::nv::voice_cal *c = cs.empty() ? nullptr : &cs[std::min(cs.size() - 1, size_t(k))];
						const int att = xg::nv::volume_att(rom, el, c ? c->base_level : 64, 48 + v * 2, vel);
						poke_slot(mu, slot, xg::nv::build_note(rom, el, 48 + v * 2, att, c));
						km |= u64(1) << slot;
						slot++;
					}
				}
				key_on_mask(mu, km);
			}
			const u64 t0 = smu2000::perf_ticks();
			for (u32 i = 0; i < SECS * RATE; i++) { s32 a = 0, b2 = 0; mu.run_sample(a, b2); }
			const u64 t1 = smu2000::perf_ticks();
			mu.set_cpu_enabled(true);
			return 1e9 * double(t1 - t0) / double(smu2000::perf_freq()) / double(SECS * RATE);
		};
		std::printf("%c声の数   firmware あり   SH-2 なし   速さの比%c", 10, 10);
		for (int v : { 1, 8, 16, 32 }) {
			const double a = run(true, v);
			const double b = run(false, v);
			std::printf("%6d %13.0f ns %10.0f ns %8.2f 倍%c", v, a, b, a / b, 10);
			std::fflush(stdout);
		}
		return 0;
	}

	// --levels: 音色ごとに「firmware で鳴らした音」と「SH-2 なしで鳴らした音」の
	// 大きさを比べる。音量の式の残りが、実際どれだけ効くかを見る
	if (levels) {
		std::printf("# prog  実機rms  SH-2なしrms    差dB  波形の残差dB%c", 10);
		double worst = 0.0, sum = 0.0, rsum = 0.0;
		int n = 0;
		for (int pg = 0; pg < levels; pg++) {
			for (u8 b : { u8(0xb0), u8(0x00), u8(msb & 0x7f), u8(0xb0), u8(0x20), u8(lsb & 0x7f),
			              u8(0xc0), u8(pg & 0x7f) })
				mu.midi_in(b, 0);
			for (u32 i = 0; i < RATE / 4; i++)
				mu.run_sample(l, r);
			const u8 *pr2 = mu.nvram().data() + part0;
			const u32 rec2 = u32(pr2[xg::ram::PART_VOICE]) << 24 | u32(pr2[xg::ram::PART_VOICE + 1]) << 16 |
			                 u32(pr2[xg::ram::PART_VOICE + 2]) << 8 | pr2[xg::ram::PART_VOICE + 3];
			if (!rec2)
				continue;
			const int nelem = xg::nv::element_count(rom, rec2);
			// **写し取り**: firmware に 1 回鳴らしてもらって、その音色の癖を覚える
			const std::vector<u8> before = mu.save_state();
			const std::vector<xg::nv::voice_cal> cals =
			    take_cals(mu, rom, rec2, note, vel, RATE);
			std::string err0;
			if (!mu.load_state(before.data(), before.size(), err0)) { std::fprintf(stderr, "%s%c", err0.c_str(), 10); return 1; }

			// firmware で鳴らす
			const std::vector<u8> saved = mu.save_state();
			for (u8 b : { u8(0x90), u8(note & 0x7f), u8(vel & 0x7f) })
				mu.midi_in(b, 0);
			double fw = 0.0;
			std::vector<double> fw_pcm, nv_pcm;
			fw_pcm.reserve(RATE / 4);
			for (u32 i = 0; i < RATE / 4; i++) {
				s32 li = 0, ri = 0;
				mu.run_sample(li, ri);
				fw_pcm.push_back(0.5 * (double(li) + double(ri)));
				if (i > RATE / 20) fw += double(li) * li + double(ri) * ri;
			}
			fw = std::sqrt(fw / double(RATE / 4 - RATE / 20) / 2);
			// 同じところへ戻して、SH-2 なしで鳴らす
			std::string err2;
			if (!mu.load_state(saved.data(), saved.size(), err2)) { std::fprintf(stderr, "%s%c", err2.c_str(), 10); return 1; }
			mu.set_cpu_enabled(false);
			u64 km = 0;
			int used = 0;
			for (int k = 0; k < nelem; k++) {
				const u8 *el = xg::nv::element(rom, rec2, k);
				if (!xg::nv::element_active(el, note, vel))
					continue;
				const u8 *we = xg::nv::wave_entry(rom, xg::nv::wave_set(el), note);
				const xg::nv::voice_cal *c = we ? xg::nv::match_cal(cals, xg::nv::read_wave(we).format_addr) : nullptr;
				if (!c && size_t(used) < cals.size())
					c = &cals[used];
				const int att = xg::nv::volume_att(rom, el, c ? c->base_level : 64, note, vel);
				poke_slot(mu, used, xg::nv::build_note(rom, el, note, att, c));
				km |= u64(1) << used;
				used++;
			}
			if (!km)
				continue;
			key_on_mask(mu, km);
			double nv = 0.0;
			nv_pcm.reserve(RATE / 4);
			for (u32 i = 0; i < RATE / 4; i++) {
				s32 li = 0, ri = 0;
				mu.run_sample(li, ri);
				nv_pcm.push_back(0.5 * (double(li) + double(ri)));
				if (i > RATE / 20) nv += double(li) * li + double(ri) * ri;
			}
			nv = std::sqrt(nv / double(RATE / 4 - RATE / 20) / 2);
			// 波形そのものも比べる（時間をずらして一番合う所で）
			double resid = 0.0;
			{
				int bshift = 0;
				double bbest = -1e30;
				for (int sh = -400; sh <= 400; sh++) {
					double acc = 0.0;
					for (size_t i = 3000; i + 400 < fw_pcm.size() && i < 20000; i += 5) {
						const size_t j = size_t(int(i) + sh);
						if (j < nv_pcm.size()) acc += fw_pcm[i] * nv_pcm[j];
					}
					if (acc > bbest) { bbest = acc; bshift = sh; }
				}
				double dsum = 0.0, asum = 0.0;
				size_t cnt = 0;
				for (size_t i = 3000; i + 400 < fw_pcm.size(); i++) {
					const size_t j = size_t(int(i) + bshift);
					if (j >= nv_pcm.size()) break;
					const double d = fw_pcm[i] - nv_pcm[j];
					dsum += d * d; asum += fw_pcm[i] * fw_pcm[i]; cnt++;
				}
				if (cnt && asum > 0.0)
					resid = 20.0 * std::log10(std::sqrt(dsum / double(cnt)) /
					                          std::sqrt(asum / double(cnt)));
			}
			mu.set_cpu_enabled(true);
			if (!mu.load_state(saved.data(), saved.size(), err2)) { std::fprintf(stderr, "%s%c", err2.c_str(), 10); return 1; }
			if (fw > 1.0 && nv > 1.0) {
				const double db = 20.0 * std::log10(nv / fw);
				std::printf("%5d %9.1f %11.1f %+7.2f %+9.1f  要素%d 実機%zu こちら%d%c",
				            pg, fw, nv, db, resid, nelem, cals.size(), used, 10);
				worst = std::max(worst, std::fabs(db));
				sum += std::fabs(db);
				rsum += resid;
				n++;
			}
			std::fflush(stdout);
		}
		if (n)
			std::printf("%c大きさの差: 平均 %.2fdB・最大 %.2fdB / 波形の残差: 平均 %.1fdB（%d 音色）%c",
			            10, sum / n, worst, rsum / n, n, 10);
		return 0;
	}

	// --volsweep: パート音量を振って att を測る。音量の掛け算の元を解くため
	if (volsweep) {
		std::printf("# prog  CC7  att  （強さ %d、鍵 %d）%c", vel, note, 10);
		for (int pg = 0; pg < volsweep; pg++) {
			for (u8 b : { u8(0xb0), u8(0x00), u8(msb & 0x7f), u8(0xb0), u8(0x20), u8(lsb & 0x7f),
			              u8(0xc0), u8(pg & 0x7f) })
				mu.midi_in(b, 0);
			for (u32 i = 0; i < RATE / 4; i++)
				mu.run_sample(l, r);
			const u8 *pr2 = mu.nvram().data() + part0;
			const u32 rec2 = u32(pr2[xg::ram::PART_VOICE]) << 24 | u32(pr2[xg::ram::PART_VOICE + 1]) << 16 |
			                 u32(pr2[xg::ram::PART_VOICE + 2]) << 8 | pr2[xg::ram::PART_VOICE + 3];
			if (!rec2 || xg::nv::element_count(rom, rec2) != 1)
				continue;
			std::printf("VOICE %d rec=%06x elem=", pg, rec2);
			for (int i = 0; i < 84; i++)
				std::printf("%02x", xg::nv::element(rom, rec2, 0)[i]);
			std::putchar(10);
			for (int cc : { 16, 32, 48, 64, 80, 100, 112, 127 }) {
				for (u8 b : { u8(0xb0), u8(0x07), u8(cc) })
					mu.midi_in(b, 0);
				for (u32 i = 0; i < RATE / 20; i++)
					mu.run_sample(l, r);
				for (u8 b : { u8(0x90), u8(note & 0x7f), u8(vel & 0x7f) })
					mu.midi_in(b, 0);
				for (u32 i = 0; i < RATE / 25; i++)
					mu.run_sample(l, r);
				std::printf("ATT %d %d%c", cc, mu.nvram()[0x3e96a], 10);
				for (u8 b : { u8(0x80), u8(note & 0x7f), u8(0x40) })
					mu.midi_in(b, 0);
				for (u32 i = 0; i < RATE / 10; i++)
					mu.run_sample(l, r);
			}
			std::fflush(stdout);
		}
		return 0;
	}

	// --sweep: 音色をいくつも替えて、レジスタの一致率をまとめて出す
	if (sweep > 0) {
		int total_same = 0, total_diff = 0;
		std::map<int, int> bad;                 // レジスタ番号 → 合わなかった回数
		int cases = 0, shown = 0;
		for (int pg = 0; pg < sweep; pg++) {
			for (u8 b : { u8(0xb0), u8(0x00), u8(msb & 0x7f), u8(0xb0), u8(0x20), u8(lsb & 0x7f),
			              u8(0xc0), u8(pg & 0x7f) })
				mu.midi_in(b, 0);
			for (u32 i = 0; i < RATE / 4; i++)
				mu.run_sample(l, r);
			const u8 *pr2 = mu.nvram().data() + part0;
			const u32 rec2 = u32(pr2[xg::ram::PART_VOICE]) << 24 | u32(pr2[xg::ram::PART_VOICE + 1]) << 16 |
			                 u32(pr2[xg::ram::PART_VOICE + 2]) << 8 | pr2[xg::ram::PART_VOICE + 3];
			if (!rec2 || xg::nv::element_count(rom, rec2) != 1)
				continue;                        // 1 要素のものだけ
			for (int nt : { 36, 60, 84 }) {
				std::map<u32, u16> seen;
				u64 mask = 0, keyed = 0;
				mu.set_swp_watch([&](bool master, u32 reg, u16 value) {
					if (!master) return;
					seen.emplace(reg, value);        // **最初の書き込み**を採る
					switch (reg) {
					case 0x18e: mask = (mask & ~(u64(0xffff) << 48)) | (u64(value) << 48); break;
					case 0x18f: mask = (mask & ~(u64(0xffff) << 32)) | (u64(value) << 32); break;
					case 0x1ce: mask = (mask & ~(u64(0xffff) << 16)) | (u64(value) << 16); break;
					case 0x1cf: mask = (mask & ~u64(0xffff)) | value; break;
					// 引き金が引かれた瞬間の値を残す。これより後は LFO などの
					// 書き直しなので、組み立ての答え合わせには使えない
					case 0x20e: keyed |= mask; break;
					default: break;
					}
				});
				for (u8 b : { u8(0x90), u8(nt), u8(vel & 0x7f) })
					mu.midi_in(b, 0);
				for (u32 i = 0; i < RATE / 25; i++)
					mu.run_sample(l, r);
				mu.set_swp_watch(nullptr);
				for (u8 b : { u8(0x80), u8(nt), u8(0x40) })
					mu.midi_in(b, 0);
				for (u32 i = 0; i < RATE / 10; i++)
					mu.run_sample(l, r);
				int ch = -1, n = 0;
				for (int c = 0; c < 64; c++)
					if (keyed & (u64(1) << c)) { ch = ch < 0 ? c : ch; n++; }
				if (ch < 0 || n != 1)
					continue;                    // 1 スロットだけのものを使う
				const xg::nv::slot_regs mine =
				    xg::nv::build_note(rom, xg::nv::element(rom, rec2, 0), nt,
				                       std::min(0xff, (xg::nv::velocity_att(rom, vel) +
				                                       xg::nv::VOICE_ATT_TYPICAL) * 2));
				cases++;
				for (int i = 0; i < 0x40; i++) {
					if (!(mine.write & (u64(1) << i)))
						continue;
					const auto it = seen.find(u32(ch) * 64 + u32(i));
					if (it == seen.end())
						continue;
					if (it->second == mine.v[i]) {
						total_same++;
					} else {
						total_diff++;
						bad[i]++;
						if (i == 0x11 && shown < 6) {
							shown++;
							std::printf("  [音程] prog%3d 鍵%3d  firmware %04x  こちら %04x  byte19=%02x%c",
							            pg, nt, it->second, mine.v[i], xg::nv::element(rom, rec2, 0)[19], 10);
						}
					}
				}
			}
		}
		std::printf("%d 件（音色 × 鍵）で、レジスタ 一致 %d / 違い %d（%.1f%% 一致）%c",
		            cases, total_same, total_diff,
		            100.0 * total_same / std::max(1, total_same + total_diff), 10);
		std::printf("合わないレジスタ:%c", 10);
		for (auto [reg, cnt] : bad)
			std::printf("  %02x  %d 回（%.0f%%）%c", reg, cnt, 100.0 * cnt / std::max(1, cases), 10);
		return 0;
	}

	// --compare: firmware に 1 音鳴らさせて、そのとき書かれたレジスタと
	// 自分で組み立てたものを突き合わせる（段 2 の「レジスタ列が一致」の物差し）
	if (compare) {
		std::map<u32, u16> seen;
		u64 mask = 0, keyed = 0;
		mu.set_swp_watch([&](bool master, u32 reg, u16 value) {
			if (!master)
				return;
			seen.emplace(reg, value);        // **最初の書き込み**を採る
			switch (reg) {
			case 0x18e: mask = (mask & ~(u64(0xffff) << 48)) | (u64(value) << 48); break;
			case 0x18f: mask = (mask & ~(u64(0xffff) << 32)) | (u64(value) << 32); break;
			case 0x1ce: mask = (mask & ~(u64(0xffff) << 16)) | (u64(value) << 16); break;
			case 0x1cf: mask = (mask & ~u64(0xffff)) | value; break;
			case 0x20e: keyed |= mask; break;
			default: break;
			}
		});
		for (u8 b : { u8(0x90), u8(note & 0x7f), u8(vel & 0x7f) })
			mu.midi_in(b, 0);
		for (u32 i = 0; i < RATE / 25; i++)
			mu.run_sample(l, r);
		mu.set_swp_watch(nullptr);
		int ch = -1;
		for (int c = 0; c < 64; c++)
			if (keyed & (u64(1) << c)) { ch = c; break; }
		if (ch < 0) {
			std::fprintf(stderr, "firmware が鳴らしたスロットが分からない%c", 10);
			return 1;
		}
		const u8 *elem = xg::nv::element(rom, rec, 0);
		const xg::nv::slot_regs mine = xg::nv::build_note(rom, elem, note,
		    std::min(0xff, (xg::nv::velocity_att(rom, vel) + xg::nv::VOICE_ATT_TYPICAL) * 2));
		// firmware がこのスロットに書いたもののうち、こちらが**書いていない**ものを出す
		std::printf("こちらが書いていないレジスタ:");
		for (const auto &[reg, val] : seen) {
			const int c = int(reg >> 6) & 0x3f, sl = int(reg) & 0x3f;
			if (c != ch || reg >= 0x1000 || sl == 0x0e || sl == 0x0f)
				continue;
			if (!(mine.write & (u64(1) << sl)))
				std::printf(" %02x=%04x", sl, val);
		}
		std::putchar(10);
		int same = 0, diff = 0, missing = 0;
		std::printf("firmware はスロット %d を鳴らした。突き合わせ:%c", ch, 10);
		for (int i = 0; i < 0x40; i++) {
			if (!(mine.write & (u64(1) << i)))
				continue;
			const auto it = seen.find(u32(ch) * 64 + u32(i));
			if (it == seen.end()) { missing++; continue; }
			if (it->second == mine.v[i]) {
				same++;
			} else {
				diff++;
				std::printf("  %02x  firmware %04x  こちら %04x%c", i, it->second, mine.v[i], 10);
			}
		}
		std::printf("一致 %d / 違い %d / firmware が書かなかった %d%c", same, diff, missing, 10);
		return diff ? 2 : 0;
	}

	// --song: SH-2 を止めたまま、何音かを順に鳴らす（段 2 の「和音と声の取り合い」の入口）
	if (song) {
		const int nelem = std::min(4, xg::nv::element_count(rom, rec));
		// 先に 1 音だけ firmware に鳴らしてもらって、この音色の癖を写し取る
		const std::vector<u8> before = mu.save_state();
		const std::vector<xg::nv::voice_cal> cals =
		    take_cals(mu, rom, rec, 60, vel, RATE);
		std::string errc;
		if (!mu.load_state(before.data(), before.size(), errc)) { std::fprintf(stderr, "%s%c", errc.c_str(), 10); return 1; }
		std::printf("要素 %d、写し取ったスロット %zu%c", nelem, cals.size(), 10);
		mu.set_cpu_enabled(false);
		std::vector<s16> out;
		struct ev { double t; int note; bool on; int slot; };
		// ドレミファソラシド＋和音
		static const int SCALE[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
		std::vector<ev> evs;
		int slot_at = 0;
		for (int i = 0; i < 8; i++) {
			evs.push_back({ 0.25 * i,        SCALE[i], true,  slot_at });
			evs.push_back({ 0.25 * i + 0.22, SCALE[i], false, slot_at });
			slot_at = (slot_at + 1) & 7;
		}
		for (int i = 0; i < 3; i++) {      // 最後に和音
			const int n = 60 + i * 4;
			evs.push_back({ 2.2, n, true,  slot_at });
			evs.push_back({ 3.4, n, false, slot_at });
			slot_at = (slot_at + 1) & 7;
		}
		std::sort(evs.begin(), evs.end(), [](const ev &a, const ev &b) { return a.t < b.t; });
		size_t at = 0;
		const size_t total = size_t(std::max(seconds, 4.5) * RATE);
		out.reserve(total * 2);
		for (size_t i = 0; i < total; i++) {
			const double t = double(i) / RATE;
			while (at < evs.size() && evs[at].t <= t) {
				const ev &e = evs[at];
				// 要素の数だけスロットを使う。鍵と強さの範囲に入るものだけ
				u64 keymask = 0;
				int used = 0;
				for (int k = 0; k < nelem; k++) {
					const u8 *el = xg::nv::element(rom, rec, k);
					if (!xg::nv::element_active(el, e.note, vel))
						continue;
					const u8 *we2 = xg::nv::wave_entry(rom, xg::nv::wave_set(el), e.note);
					const xg::nv::voice_cal *c = we2 ? xg::nv::match_cal(cals, xg::nv::read_wave(we2).format_addr) : nullptr;
					if (!c && size_t(used) < cals.size())
						c = &cals[used];
					used++;
					const int slot = (e.slot * 4 + k) & 0x3f;
					const int att = xg::nv::volume_att(rom, el,
					    c ? c->base_level : 64, e.note, vel);
					if (e.on) {
						poke_slot(mu, slot, xg::nv::build_note(rom, el, e.note, att, c));
						keymask |= u64(1) << slot;
					} else {
						mu.poke_swp(true, u32(slot) * 64 + 9,
						            xg::nv::release_reg(rom, el, e.note, att));
					}
				}
				if (e.on)
					key_on_mask(mu, keymask);
				at++;
			}
			s32 li = 0, ri = 0;
			mu.run_sample(li, ri);
			out.push_back(s16(std::clamp(li * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
			out.push_back(s16(std::clamp(ri * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
		}
		double pk = 0.0, sm = 0.0;
		for (s16 v : out) { pk = std::max(pk, double(std::abs(v))); sm += double(v) * v; }
		std::printf("SH-2 なしで %zu 音: 山 %.0f  rms %.1f%c", evs.size() / 2, pk,
		            std::sqrt(sm / double(out.size())), 10);
		if (!write_wav(out_path, out, RATE)) { std::fprintf(stderr, "書けない%c", 10); return 1; }
		std::printf("書き出した: %s%c", out_path.c_str(), 10);
		return 0;
	}

	std::vector<s16> pcm;
	pcm.reserve(size_t(seconds * RATE) * 2);

	if (firmware) {
		// 比べるための版。firmware にそのまま鳴らさせる
		for (u8 b : { u8(0x90), u8(note & 0x7f), u8(vel & 0x7f) })
			mu.midi_in(b, 0);
	} else {
		const u8 *elem = xg::nv::element(rom, rec, 0);
		// 先に 1 音だけ firmware に鳴らしてもらって癖を写し取る
		const std::vector<u8> before1 = mu.save_state();
		const xg::nv::voice_cal cal = take_cal(mu, rom, rec, note, vel, RATE);
		std::string err1;
		if (!mu.load_state(before1.data(), before1.size(), err1)) { std::fprintf(stderr, "%s%c", err1.c_str(), 10); return 1; }
		// ---- ここから SH-2 を止める
		mu.set_cpu_enabled(false);
		const int att = xg::nv::volume_att(rom, elem, cal.base_level, note, vel);
		xg::nv::slot_regs regs = xg::nv::build_note(rom, elem, note, att, &cal);
		if (copyall) {
			// 切り分け用: 写し取った値をそのまま全部使う（式を一切使わない）
			for (int i = 0; i < 0x40; i++)
				if (cal.has(i))
					regs.set(i, cal.reg[i]);
		}
		if (!regs.write) {
			std::fprintf(stderr, "波形の記録が引けなかった%c", 10);
			return 1;
		}
		std::printf("スロット %d に書くレジスタ:%c", slot, 10);
		for (int i = 0; i < 0x40; i++)
			if (regs.write & (u64(1) << i))
				std::printf("  %02x=%04x%s", i, regs.v[i], (i % 8 == 7) ? "\n" : "");
		std::putchar(10);
		poke_slot(mu, slot, regs);
		key_on(mu, slot);
	}

	for (size_t i = 0; i < size_t(seconds * RATE); i++) {
		s32 li = 0, ri = 0;
		mu.run_sample(li, ri);
		pcm.push_back(s16(std::clamp(li * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
		pcm.push_back(s16(std::clamp(ri * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
	}

	double peak = 0.0, sum = 0.0;
	for (s16 v : pcm) { peak = std::max(peak, double(std::abs(v))); sum += double(v) * v; }
	std::printf("%s: 山 %.0f  rms %.1f%c", firmware ? "firmware" : "SH-2 なし",
	            peak, std::sqrt(sum / double(pcm.size())), 10);
	if (!write_wav(out_path, pcm, RATE)) {
		std::fprintf(stderr, "書けない: %s%c", out_path.c_str(), 10);
		return 1;
	}
	std::printf("書き出した: %s%c", out_path.c_str(), 10);
	return 0;
}
