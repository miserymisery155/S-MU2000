// サンプリングが一回りするかを確かめる。
//
//   samptest <rom ディレクトリ> [-v]
//
// パネルで SAMPLING → REC に入り、A/D INPUT に 440Hz の正弦を流しながら 1 秒ほど録音して止め、
// 残す。SAMPLE の画面で AUDITION を押し、出てきた音が 440Hz かを見る。
// firmware が録音に使う SWP30 の働き（サンプリング RAM と波形アクセス 0x7000）が
// 正しくないと、サンプルが出来ないか、試聴で別の音か無音になる。
// 続けて A/D パートの音量と、SmartMedia への書き出し・読み戻し（書式化 → SAVE → 別の機械で LOAD）、
// REC の InputSrc（AD2 / AD1+2）で録るものが変わるかを見る。
// 食い違えば 1 を返す。
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
	double sine2_amp = -1.0;        // AD2 だけ 660Hz にするときの振幅（負なら AD1 と同じもの）
	u64 n = 0;
	std::vector<double> out;        // 集めている間の出力（左右の平均）
	bool collect = false;

	void pump(u32 ms)
	{
		const u64 until = n + u64(ms) * RATE / 1000;
		for (; n < until; n++) {
			const s32 v = s32(std::lround(sine_amp * std::sin(2 * PI * 440.0 * double(n) / RATE)));
			const s32 v2 = sine2_amp < 0 ? v : s32(std::lround(sine2_amp * std::sin(2 * PI * 660.0 * double(n) / RATE)));
			mu.set_audio_input(v, v2);
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

	// A/D パート。既定の音量は 0 で、入力は聞こえない。音量を上げると A/D INPUT がそのまま鳴る
	// （スレーブの MELI 6/7 を firmware がミキサに通す）
	g.press(B::exit);
	g.press(B::exit);
	g.press(B::exit);
	auto level_440 = [&](double &rms_out) {
		g.out.clear();
		g.sine_amp = 8000;
		g.pump(200);
		g.collect = true;
		g.pump(500);
		g.collect = false;
		g.sine_amp = 0;
		double sum = 0;
		for (double v : g.out)
			sum += v * v;
		rms_out = std::sqrt(sum / std::max<size_t>(1, g.out.size()));
		return tone(g.out, 440);
	};
	double rms_off = 0, rms_on = 0;
	const double ad_off = level_440(rms_off);
	for (int part = 0; part < 2; part++) {
		const u8 msg[] = { 0xf0, 0x43, 0x10, 0x4c, 0x10, u8(part), 0x0b, 100, 0xf7 };
		for (u8 b : msg)
			g.mu.midi_in(b, 0);
	}
	g.pump(300);
	const double ad_on = level_440(rms_on);
	const bool off_ok = rms_off < 0.001;
	const bool on_ok = rms_on > 0.05 && ad_on > 10 * std::max(tone(g.out, 330), tone(g.out, 587));
	std::printf("%s A/D パートの音量 0 では無音      rms %.5f\n", off_ok ? "合" : "NG", rms_off);
	std::printf("%s A/D パートの音量 100 で入力が鳴る rms %.4f / 440Hz %.5f\n", on_ok ? "合" : "NG", rms_on, ad_on);
	(void)ad_off;
	if (!off_ok) bad++;
	if (!on_ok) bad++;

	// SmartMedia。空のカードを差して UTIL → CARD → Format で書式化し、SAMPLING → SAVE で ALL+SEQ を書く。
	// 書いたカードを新しい機械に差し、SAMPLING → LOAD で読み戻して、サンプリング RAM が同じになるかを見る。
	// SmartMedia の NAND の命令・物理の書式・ECC と、SWP30 の続けて読む働き（波形アクセス 0x9000）を通る
	g.mu.card().create(32);
	g.pump(500);
	g.press(B::util);
	for (int i = 0; i < 4; i++)
		g.press(B::select_right);
	g.press(B::enter);
	for (int i = 0; i < 4; i++)
		g.press(B::select_right);
	expect("UTIL → CARD → Format", "Format");
	g.press(B::enter);
	g.press(B::enter);                  // 書式化してよいか
	for (int i = 0; i < 100 && g.lcd().find("Executing") != std::string::npos; i++)
		g.pump(100);
	expect("書式化を終えた", "Format");
	g.press(B::exit);
	g.press(B::exit);
	g.press(B::exit);

	g.press(B::sampling_mode);
	g.press(B::select_right);
	g.press(B::select_right);
	g.press(B::enter);
	expect("SAVE の画面", "ALL+SEQ");
	g.press(B::enter);                  // 保存先のディレクトリ
	g.pump(1000);
	g.press(B::enter);                  // ファイルの名前
	g.pump(1000);
	expect("ファイルの名前", "ALL_SEQ");
	g.press(B::enter);
	expect("書き出し中", "SAVING");
	for (int i = 0; i < 100 && g.lcd().find("SAVING") != std::string::npos; i++)
		g.pump(100);
	expect("書き終えた", "<SAVE>");

	static rig h;
	if (!h.mu.load_program(dir + "/mu2000_flash.bin") || !h.mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", h.mu.error().c_str());
		return 1;
	}
	h.verbose = g.verbose;
	h.mu.load_sintab(dir + "/standin/sin-table.bin");
	h.mu.reset();
	for (u32 i = 0; i < 30 * RATE && !h.mu.midi_ready(); i += RATE / 100)
		h.pump(10);
	h.mu.card() = g.mu.card();
	h.pump(1500);
	h.press(B::sampling_mode);
	h.press(B::select_right);
	h.press(B::enter);
	h.pump(1000);
	h.press(B::enter);                  // ディレクトリの中
	h.pump(1000);
	{
		const std::string s = h.lcd();
		const bool ok = s.find("ALL_SEQ.M2A") != std::string::npos;
		std::printf("%s %-28s [%s]\n", ok ? "合" : "NG", "カードにファイルがある", s.c_str());
		if (!ok) bad++;
	}
	h.press(B::enter);
	for (int i = 0; i < 100 && h.lcd().find("LOADING") != std::string::npos; i++)
		h.pump(100);
	// 録音の最後の 1 語の後ろ半分（サンプルの長さの外）は書き出されないので、そこだけは違ってよい
	const auto &a = g.mu.sample_ram(), &b = h.mu.sample_ram();
	size_t differ = 0, used = 0;
	for (size_t i = 0; i < a.size(); i++) {
		differ += a[i] != b[i];
		used += a[i] != 0;
	}
	const bool same = used > 50000 && differ <= 2;
	std::printf("%s 読み戻したサンプリング RAM     使っている %zu バイト、違う %zu バイト\n", same ? "合" : "NG", used, differ);
	if (!same) bad++;

	// REC の InputSrc。AD1 に 440Hz、AD2 に 660Hz を入れ、AD2 と AD1+2 で録る。
	// firmware は MELI 6/7 からミキサの出力 8 への音量を切り替えるので、録ったものの周波数で分かる
	auto record_src = [&](int presses, double &f440, double &f660) {
		g.press(B::exit);
		g.press(B::select_right);                       // SAVE の隣が REC
		g.press(B::enter);
		for (int i = 0; i < 3; i++)
			g.press(B::select_right);
		for (int i = 0; i < presses; i++)
			g.press(B::value_plus);
		for (int i = 0; i < 3; i++)
			g.press(B::select_left);
		const std::vector<u8> before = g.mu.sample_ram();
		g.sine_amp = 8000;
		g.sine2_amp = 8000;
		g.pump(200);
		g.press(B::enter);
		g.pump(700);
		g.press(B::enter);
		g.sine_amp = 0;
		g.sine2_amp = -1;
		g.pump(300);
		std::vector<double> x;
		const auto &after = g.mu.sample_ram();
		for (size_t i = 0; i + 1 < after.size(); i += 2)
			if (after[i] != before[i] || after[i + 1] != before[i + 1])
				x.push_back(double(s16(after[i] | (after[i + 1] << 8))) / 32768.0);
		// 途中の 4000 サンプルで見る（変わらなかったバイトを飛ばしているので、全部を繋ぐと位相が飛ぶ）
		const std::vector<double> mid = x.size() > 8000 ? std::vector<double>(x.begin() + 4000, x.begin() + 8000) : std::vector<double>();
		f440 = mid.empty() ? 0 : tone(mid, 440);
		f660 = mid.empty() ? 0 : tone(mid, 660);
		g.press(B::exit);                               // Keep Sample? から抜ける（残すかどうかは見ない）
		g.press(B::exit);
		return x.size();
	};
	{
		double a440 = 0, a660 = 0, b440 = 0, b660 = 0;
		const size_t na = record_src(1, a440, a660);    // AD1 → AD2
		const bool ad2 = a660 > 0.05 && a440 < a660 / 20;
		std::printf("%s InputSrc=AD2 で AD2 だけ録る    %zu サンプル、440Hz %.4f / 660Hz %.4f\n", ad2 ? "合" : "NG", na, a440, a660);
		const size_t nb = record_src(1, b440, b660);    // AD2 → AD1+2
		const bool both = b440 > 0.05 && b660 > 0.05;
		std::printf("%s InputSrc=AD1+2 で両方を録る     %zu サンプル、440Hz %.4f / 660Hz %.4f\n", both ? "合" : "NG", nb, b440, b660);
		if (!ad2) bad++;
		if (!both) bad++;
	}

	// REC の TriggerLvl。レベルを上げて Enter を押すと「Waiting!」で待ち、入力が来ると録音が始まる。
	// firmware は CPU の A/D 変換器の AN0 / AN2（A/D INPUT の大きさ）を回し続けて読む
	g.press(B::exit);
	g.press(B::select_right);
	g.press(B::enter);
	g.press(B::select_right);
	for (int i = 0; i < 6; i++)
		g.press(B::value_plus);
	expect("TriggerLvl を上げた", "TriggerLvl=06");
	g.press(B::select_left);
	g.press(B::enter);
	g.pump(500);
	expect("入力が無いと待つ", "Waiting!");
	g.sine_amp = 12000;
	g.pump(500);
	expect("入力が来ると録音する", "Recording!");
	g.press(B::enter);
	g.sine_amp = 0;
	g.pump(300);

	std::printf("サンプリング: 食い違い %d\n", bad);
	return bad ? 1 : 0;
}
