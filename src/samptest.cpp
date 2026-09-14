// サンプリングが一回りするかを確かめる。
//
//   samptest <rom ディレクトリ> [-v]
//
// パネルで SAMPLING → REC に入り、A/D INPUT に 440Hz の正弦を流しながら 1 秒ほど録音して止め、
// 残す。SAMPLE の画面で AUDITION を押し、出てきた音が 440Hz かを見る。
// firmware が録音に使う SWP30 の働き（サンプリング RAM と波形アクセス 0x7000）が
// 正しくないと、サンプルが出来ないか、試聴で別の音か無音になる。食い違えば 1 を返す。
#include "mu2000.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = 44100;
constexpr double PI = 3.14159265358979323846;

struct rig {
	mu2000 mu;
	bool verbose = false;
	double sine_amp = 0.0;          // A/D INPUT に流す正弦の振幅（0 なら無音）
	u64 n = 0;
	std::vector<double> out;        // 集めている間の出力（左右の平均）
	bool collect = false;

	void pump(u32 ms)
	{
		const u64 until = n + u64(ms) * RATE / 1000;
		for (; n < until; n++) {
			const s32 v = s32(std::lround(sine_amp * std::sin(2 * PI * 440.0 * double(n) / RATE)));
			mu.set_audio_input(v, v);
			s32 l, r;
			mu.run_sample(l, r);
			u8 b;
			while (mu.midi_out_take(b)) {}
			if (collect)
				out.push_back((double(l) + double(r)) * 0.5 / mu2000::DAC_FULL_SCALE);
		}
	}

	std::string lcd()
	{
		const u8 *dd = mu.lcd().ddram();
		std::string s;
		for (int row = 0; row < 2; row++) {
			for (int c = 0; c < 24; c++) {
				const u8 ch = dd[row * 0x40 + c];
				s += (ch >= 32 && ch < 127) ? char(ch) : ' ';
			}
			if (!row)
				s += '|';
		}
		return s;
	}

	void press(mu2000::button b, u32 hold_ms = 80)
	{
		mu.set_button(b, true);
		pump(hold_ms);
		mu.set_button(b, false);
		pump(300);
		if (verbose)
			std::printf("  %-14s [%s]\n", mu2000::button_name(b), lcd().c_str());
	}
};

// 周波数 f の成分の大きさ（Goertzel）
double tone(const std::vector<double> &x, double f)
{
	const double w = 2 * PI * f / RATE, c = 2 * std::cos(w);
	double s1 = 0, s2 = 0;
	for (double v : x) {
		const double s0 = v + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / double(x.size());
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "使い方: samptest <rom ディレクトリ> [-v]\n");
		return 1;
	}
	const std::string dir = argv[1];
	static rig g;
	g.verbose = argc > 2 && !std::strcmp(argv[2], "-v");

	if (!g.mu.load_program(dir + "/mu2000_flash.bin") || !g.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", g.mu.error().c_str());
		return 1;
	}
	g.mu.load_sintab(dir + "/standin/sin-table.bin");
	g.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !g.mu.midi_ready(); i += RATE / 100)
		g.pump(10);
	g.pump(1500);

	int bad = 0;
	auto expect = [&](const char *what, const char *text) {
		const std::string s = g.lcd();
		const bool ok = s.find(text) != std::string::npos;
		std::printf("%s %-28s [%s]\n", ok ? "合" : "NG", what, s.c_str());
		if (!ok)
			bad++;
	};

	using B = mu2000::button;
	// SAMPLING の品書き: EDIT LOAD SAVE / REC UTIL RAM。REC は 4 つ目
	g.press(B::sampling_mode);
	expect("SAMPLING の品書き", "REC");
	for (int i = 0; i < 3; i++)
		g.press(B::select_right);
	g.press(B::enter);
	expect("REC の画面", "Sp=001");

	// 録音。始める前から正弦を流しておく
	g.sine_amp = 12000;
	g.pump(200);
	g.press(B::enter);
	expect("録音中", "Recording!");
	g.pump(1000);
	g.press(B::enter);                  // 止める
	g.sine_amp = 0;
	g.pump(300);
	g.press(B::exit);
	expect("残すか聞かれる", "Keep Sample 001?");
	g.press(B::enter);
	g.press(B::exit);

	// EDIT → SAMPLE → SMPL001 で試聴
	for (int i = 0; i < 3; i++)
		g.press(B::select_left);
	g.press(B::enter);
	g.press(B::enter);
	expect("出来たサンプル", "SMPL001");
	g.collect = true;
	g.mu.set_button(B::audition, true);
	g.pump(1000);
	g.mu.set_button(B::audition, false);
	g.collect = false;

	double rms = 0;
	for (double v : g.out)
		rms += v * v;
	rms = std::sqrt(rms / std::max<size_t>(1, g.out.size()));
	const double t440 = tone(g.out, 440), t330 = tone(g.out, 330), t587 = tone(g.out, 587);
	const bool loud = rms > 0.01;
	const bool pitch = t440 > 10 * std::max(t330, t587);
	std::printf("%s 試聴の音の大きさ              rms %.4f（全振幅 1）\n", loud ? "合" : "NG", rms);
	std::printf("%s 試聴の音が 440Hz              440Hz %.5f / 330Hz %.5f / 587Hz %.5f\n",
	            pitch ? "合" : "NG", t440, t330, t587);
	if (!loud) bad++;
	if (!pitch) bad++;

	std::printf("サンプリング: 食い違い %d\n", bad);
	return bad ? 1 : 0;
}
