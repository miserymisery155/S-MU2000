// license:BSD-3-Clause

#include "engine.h"

#include "mu2000.h"
#include "nvram.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <windows.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace smu2000 {
namespace vst3 {

namespace {

// ---- プラグイン本体（DLL）の置かれている場所

std::string module_dir()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(&module_dir), &self))
		return {};
	char buf[MAX_PATH * 2] = {};
	const DWORD n = GetModuleFileNameA(self, buf, sizeof(buf));
	if (!n || n >= sizeof(buf))
		return {};
	std::string s(buf, n);
	const size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : s.substr(0, slash);
}

std::string env(const char *name)
{
	char buf[MAX_PATH * 4];
	const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
	return (n && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

bool is_file(const std::string &p)
{
	const DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// そのディレクトリが ROM 置き場かどうか
bool has_roms(const std::string &dir)
{
	return !dir.empty() && is_file(dir + "\\mu2000_flash.bin");
}

// roms.txt に書かれた場所を読む（1 行目だけ）
std::string read_pointer_file(const std::string &path)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	char line[1024] = {};
	if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return {}; }
	std::fclose(f);
	std::string s(line);
	// メモ帳などが付ける BOM を落とす。これがあると場所を見失う
	if (s.size() >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
	    (unsigned char)s[2] == 0xbf)
		s.erase(0, 3);
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
	                      s.back() == ' '  || s.back() == '\t'))
		s.pop_back();
	return s;
}

// ---- 記録。画面が無いので、うまくいかなかったときはここを見てもらう

std::string log_path()
{
	const std::string base = env("LOCALAPPDATA");
	if (base.empty())
		return {};
	const std::string dir = base + "\\S-MU2000";
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir + "\\log.txt";
}

void logf(const char *fmt, ...)
{
	static const std::string path = log_path();
	if (path.empty())
		return;
	const bool fresh = !is_file(path);
	std::FILE *f = std::fopen(path.c_str(), "ab");
	if (!f)
		return;
	if (fresh)
		std::fwrite("\xef\xbb\xbf", 1, 3, f);   // UTF-8 の印。無いと化けて読まれる
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay,
	             t.wHour, t.wMinute, t.wSecond);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fclose(f);
}

// ROM 置き場を探す。見つかった場所を返す。無ければ空で、探した場所が tried に入る
std::string find_roms(std::string &tried)
{
	std::vector<std::string> cand;

	// 1. 環境変数。一番強い
	const std::string ev = env("S_MU2000_ROMS");
	if (!ev.empty())
		cand.push_back(ev);

	const std::string dir = module_dir();
	if (!dir.empty()) {
		// 2. バンドルの Resources。
		//    <名前>.vst3/Contents/x86_64-win/ に DLL がいるので 1 つ上
		cand.push_back(dir + "\\..\\Resources");
		cand.push_back(dir + "\\..\\Resources\\roms");
		// 3. DLL のすぐ横
		cand.push_back(dir + "\\roms");
		cand.push_back(dir);
		// 4. 場所を書いた紙
		const std::string notes[2] = { dir + "\\..\\Resources\\roms.txt",
		                               dir + "\\roms.txt" };
		for (const std::string &p : notes) {
			const std::string s = read_pointer_file(p);
			if (!s.empty())
				cand.push_back(s);
		}
	}

	// 5. 決め打ちの置き場
	const std::string local = env("LOCALAPPDATA");
	if (!local.empty())
		cand.push_back(local + "\\S-MU2000\\roms");
	const std::string home = env("USERPROFILE");
	if (!home.empty())
		cand.push_back(home + "\\Documents\\S-MU2000\\roms");

	for (const std::string &c : cand) {
		char full[MAX_PATH * 2] = {};
		const DWORD n = GetFullPathNameA(c.c_str(), sizeof(full), full, nullptr);
		const std::string p = (n && n < sizeof(full)) ? std::string(full, n) : c;
		if (has_roms(p))
			return p;
		tried += "  " + p + "\n";
	}
	return {};
}

} // namespace


// ---- 読み込んだ ROM の使い回し。
// 誰も使わなくなったら消えるよう、控えは weak_ptr で持つ

namespace {

struct rom_set {
	mu2000::u8rom  prog, wave;
	mu2000::u16rom sintab;
	mu2000::u8rom  font;
	std::string    warn;
};

std::mutex             g_rom_mutex;
std::string            g_rom_dir;
std::weak_ptr<rom_set> g_roms;

} // namespace


engine::engine()
{
	build_table();
	for (std::vector<uint8_t> &p : m_pending)
		p.reserve(4096);
}

engine::~engine()
{
	m_abort.store(true, std::memory_order_relaxed);
	if (m_thread.joinable())
		m_thread.join();
	delete m_mu;
}

std::string engine::message() const
{
	return state() == status::loading ? std::string("起動中") : m_message;
}

void engine::log_line(const char *text)
{
	logf("%s", text);
}

void engine::start()
{
	if (!m_thread.joinable())
		m_thread = std::thread([this] { boot(); });
}

void engine::boot()
{
	std::string tried;
	const std::string dir = find_roms(tried);
	if (dir.empty()) {
		m_message = "ROM が見つからない。探した場所:\n" + tried +
		            "環境変数 S_MU2000_ROMS で場所を指定できる";
		logf("ROM が見つからない。探した場所:\n%s", tried.c_str());
		ui::driver::publish_message(m_bridge, "ROM が見つからない");
		m_state.store(status::failed, std::memory_order_release);
		return;
	}
	logf("ROM: %s", dir.c_str());
	ui::driver::publish_message(m_bridge, "ROM 読み込み中");

	mu2000 *mu = new mu2000;
	std::string warn;
	{
		// ROM は読むだけなので、この DLL の中で 1 組あればいい。
		// トラックごとに挿されると 36MB × 枚数になってしまう
		std::lock_guard<std::mutex> lock(g_rom_mutex);
		std::shared_ptr<rom_set> shared;
		if (g_rom_dir == dir)
			shared = g_roms.lock();

		if (shared) {
			mu->set_program_rom(shared->prog);
			mu->set_wave_rom(shared->wave);
			mu->set_sintab_rom(shared->sintab);
			mu->set_lcd_font(shared->font);
			warn = shared->warn;
			logf("ROM は読み込み済みのものを借りた");
		} else {
			if (!mu->load_program(dir + "\\mu2000_flash.bin") ||
			    !mu->load_wave(dir + "\\dump")) {
				m_message = mu->error();
				logf("%s", m_message.c_str());
				delete mu;
				m_state.store(status::failed, std::memory_order_release);
				return;
			}
			if (!mu->load_sintab(dir + "\\standin\\sin-table.bin")) {
				warn = mu->error();
				logf("警告: %s", warn.c_str());
			}
			// LCD の字の絵。無くても音は出るが、画面に何も映らなくなる
			if (!mu->load_lcd_font(dir + "\\hd44780u_b04.bin") &&
			    !mu->load_lcd_font(dir + "\\standin\\hd44780u_b04.bin"))
				logf("警告: %s", mu->error().c_str());
			shared = std::make_shared<rom_set>();
			shared->prog   = mu->program_rom();
			shared->wave   = mu->wave_rom();
			shared->sintab = mu->sintab_rom();
			shared->font   = mu->lcd_font();
			shared->warn   = warn;
			g_rom_dir = dir;
			g_roms    = shared;
		}
		// 借り手が 1 人でも生きている限り、次の人も借りられる
		m_roms = shared;
	}

	mu->set_threaded(true);
	// gui / live が残した設定で起動する。**読むだけで書かない。**VST3 の中で
	// 変えたものは DAW のプロジェクトに残るし、何枚も挿されたときに
	// 同じファイルを取り合わずに済む
	if (nvram::load(*mu))
		logf("設定: %s", nvram::path(*mu).c_str());
	mu->reset();

	ui::driver::publish_message(m_bridge, "MU2000 起動中");

	// 起動を待つ。ここを待たずに MIDI を流すと音色指定が全部捨てられる
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	const int64_t limit = int64_t(30.0 * NATIVE_RATE);
	int64_t i = 0;
	for (; i < limit; i++) {
		if (!(i & 4095) && m_abort.load(std::memory_order_relaxed)) {
			delete mu;
			return;
		}
		if (mu->midi_ready())
			break;
		s32 l = 0, r = 0;
		mu->run_sample(l, r);
	}
	if (i >= limit) {
		m_message = "MU2000 が起動しなかった（ROM が壊れている可能性）";
		logf("%s", m_message.c_str());
		delete mu;
		m_state.store(status::failed, std::memory_order_release);
		return;
	}

	LARGE_INTEGER t1, f;
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&f);
	logf("起動: 音 %.2f 秒ぶん / 実時間 %.2f 秒", double(i) / NATIVE_RATE,
	     double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart));

	m_mu = mu;
	m_message = warn.empty() ? std::string("ROM: ") + dir
	                         : std::string("ROM: ") + dir + "\n警告: " + warn;
	m_state.store(status::ready, std::memory_order_release);
	ui::driver::publish_now(*m_mu, m_bridge, true, nullptr);
}


// ---- 標本化周波数の変換

void engine::build_table()
{
	m_tab.resize(size_t(HALF) * STEPS + 2);
	for (size_t k = 0; k < m_tab.size(); k++) {
		const double d = double(k) / STEPS;             // 中心からの距離
		const double x = M_PI * d;
		const double sinc = (k == 0) ? 1.0 : std::sin(x) / x;
		// ブラックマン窓。TAPS 本で阻止域 -74dB くらい
		const double t = (d + HALF) / double(TAPS);
		const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * t)
		                      + 0.08 * std::cos(4.0 * M_PI * t);
		m_tab[k] = float(sinc * w);
	}
}

void engine::set_output_rate(double rate)
{
	if (rate <= 0.0)
		rate = NATIVE_RATE;
	m_direct = std::fabs(rate - NATIVE_RATE) < 1e-6;
	m_step   = NATIVE_RATE / rate;
	// 上へ変換するときは入力のナイキストまで通す。
	// 下へ変換するときは出力のナイキストで切らないと折り返す
	m_cutoff = std::min(1.0, rate / NATIVE_RATE) * 0.955;
	// 音源側は「必要な先の音」をその場で作れるので、変換に先読みの遅れは無い
	m_latency = 0;
	flush_resampler();
}

void engine::flush_resampler()
{
	std::memset(m_ring_l, 0, sizeof(m_ring_l));
	std::memset(m_ring_r, 0, sizeof(m_ring_r));
	m_written = 0;
	m_pos     = 0.0;
}

void engine::one_sample(float &l, float &r)
{
	s32 li = 0, ri = 0;
	m_mu->run_sample(li, ri);
	const float k = 1.0f / float(mu2000::DAC_FULL_SCALE);
	l = std::clamp(float(li) * k, -1.0f, 1.0f);
	r = std::clamp(float(ri) * k, -1.0f, 1.0f);
}


void engine::midi(const uint8_t *bytes, size_t n, int port)
{
	port = port == 1 ? 1 : 0;
	const status s = state();
	if (s == status::ready) {
		for (size_t i = 0; i < n; i++) {
			m_mu->midi_in(bytes[i], port);
			m_drv.watch(bytes[i], port);
		}
		return;
	}
	if (s == status::failed)
		return;
	// 起動待ち。あふれるようなら捨てる
	std::vector<uint8_t> &pending = m_pending[port];
	if (pending.size() + n > 65536)
		return;
	pending.insert(pending.end(), bytes, bytes + n);
}

void engine::all_notes_off()
{
	for (int port = 0; port < 2; port++)
		for (int ch = 0; ch < 16; ch++) {
			const uint8_t msg[6] = { uint8_t(0xb0 | ch), 120, 0,
			                         uint8_t(0xb0 | ch), 123, 0 };
			midi(msg, sizeof(msg), port);
		}
}


void engine::fill(float *left, float *right, int n)
{
	if (n <= 0)
		return;
	if (state() != status::ready) {
		std::memset(left,  0, size_t(n) * sizeof(float));
		std::memset(right, 0, size_t(n) * sizeof(float));
		return;
	}
	// 頼まれている保存と復元を、ここ（音声スレッド）で片づける
	m_fill_tick.fetch_add(1, std::memory_order_release);
	serve_state();

	m_drv.apply_buttons(*m_mu, m_bridge);
	m_drv.pump_midi(*m_mu, m_bridge);
	m_drv.pump_wheel(*m_mu, m_bridge);

	for (int port = 0; port < 2; port++) {
		for (uint8_t b : m_pending[port]) {
			m_mu->midi_in(b, port);
			m_drv.watch(b, port);
		}
		m_pending[port].clear();
	}

	if (m_direct) {
		for (int i = 0; i < n; i++)
			one_sample(left[i], right[i]);
		m_drv.pump_out(*m_mu, m_bridge);
		m_drv.publish(*m_mu, m_bridge, u32(n), u32(NATIVE_RATE), true, nullptr);
		return;
	}

	for (int i = 0; i < n; i++) {
		const int64_t centre = int64_t(std::floor(m_pos));
		// 畳み込みに要る一番先のサンプルまで作る
		while (m_written <= centre + HALF) {
			float l, r;
			one_sample(l, r);
			m_ring_l[m_written & RMASK] = l;
			m_ring_r[m_written & RMASK] = r;
			m_written++;
		}

		double al = 0.0, ar = 0.0, sum = 0.0;
		for (int k = -HALF + 1; k <= HALF; k++) {
			const int64_t idx = centre + k;
			const double d  = std::fabs((m_pos - double(idx)) * m_cutoff);
			const double fx = d * STEPS;
			const size_t j  = size_t(fx);
			if (j + 1 >= m_tab.size())
				continue;
			const double t = fx - double(j);
			const double h = m_tab[j] + (m_tab[j + 1] - m_tab[j]) * t;
			al  += h * m_ring_l[idx & RMASK];
			ar  += h * m_ring_r[idx & RMASK];
			sum += h;
		}
		if (sum > 1e-9) { al /= sum; ar /= sum; }
		left[i]  = std::clamp(float(al), -1.0f, 1.0f);
		right[i] = std::clamp(float(ar), -1.0f, 1.0f);
		m_pos += m_step;
	}

	// firmware が MIDI OUT から送り出したもの（画面の問い合わせの返事）
	m_drv.pump_out(*m_mu, m_bridge);
	m_drv.publish(*m_mu, m_bridge, u32(n), u32(NATIVE_RATE), true, nullptr);

	// 桁が落ちる前に原点を戻す。RING の倍数だけずらせば環の並びは変わらない
	if (m_pos > double(1 << 28)) {
		const int64_t base = (int64_t(m_pos) - HALF) & ~int64_t(RMASK);
		m_pos     -= double(base);
		m_written -= base;
	}
}



// ---- 状態の保存と復元
//
// 機械に触れてよいのは、音を作っているあいだは音声スレッドだけ。
// だから頼み事を置いておいて、fill() の頭で片づける。止まっているときは
// 誰も触っていないので、その場でやってしまう。

void engine::serve_state()
{
	if (m_load_req.load(std::memory_order_acquire) == 1) {
		std::string err;
		if (m_mu)
			m_mu->load_state(m_load_buf.data(), m_load_buf.size(), err);
		m_load_req.store(0, std::memory_order_release);
	}
	if (m_save_req.load(std::memory_order_acquire) == 1) {
		m_save_buf = m_mu ? m_mu->save_state() : std::vector<u8>();
		m_save_req.store(2, std::memory_order_release);
	}
}

std::vector<uint8_t> engine::save_state()
{
	if (state() != status::ready || !m_mu)
		return {};
	if (!m_processing.load(std::memory_order_acquire)) {
		// 誰も触っていない。その場で
		return m_mu->save_state();
	}
	// 「動いている」と言われていても、実際に fill() が回っていないことがある
	// （止めたばかり、ホストが呼んでいない）。回っていなければその場でやる
	const uint64_t t0 = m_fill_tick.load(std::memory_order_acquire);
	m_save_req.store(1, std::memory_order_release);
	for (int i = 0; i < 200; i++) {          // 2 秒まで待つ
		if (m_save_req.load(std::memory_order_acquire) == 2)
			break;
		Sleep(10);
	}
	if (m_save_req.load(std::memory_order_acquire) != 2) {
		m_save_req.store(0, std::memory_order_release);
		if (m_fill_tick.load(std::memory_order_acquire) == t0)
			return m_mu->save_state();   // 誰も回していない
		log_line("状態を保存できなかった（音声スレッドが応じない）");
		return {};
	}
	std::vector<uint8_t> out;
	out.swap(m_save_buf);
    m_save_req.store(0, std::memory_order_release);
	return out;
}

bool engine::load_state(const uint8_t *p, size_t n)
{
	if (state() != status::ready || !m_mu || !p || !n)
		return false;
	if (!m_processing.load(std::memory_order_acquire)) {
		std::string err;
		const bool ok = m_mu->load_state(p, n, err);
		if (!ok)
			log_line(("状態を読み戻せない: " + err).c_str());
		return ok;
	}
	const uint64_t t0 = m_fill_tick.load(std::memory_order_acquire);
	m_load_buf.assign(p, p + n);
	m_load_req.store(1, std::memory_order_release);
	for (int i = 0; i < 200; i++) {
		if (m_load_req.load(std::memory_order_acquire) == 0)
			return true;
		Sleep(10);
	}
	m_load_req.store(0, std::memory_order_release);
	if (m_fill_tick.load(std::memory_order_acquire) == t0) {
		std::string err;
		return m_mu->load_state(p, n, err);   // 誰も回していない
	}
	log_line("状態を読み戻せなかった（音声スレッドが応じない）");
	return false;
}

} // namespace vst3
} // namespace smu2000
