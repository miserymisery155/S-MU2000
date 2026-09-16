// license:BSD-3-Clause
//
// CLAP プラグインを DAW 無しで鳴らしてみる小さなホスト。
//
//   clapprobe <S-MU2000.clap> <MIDI ファイル> <出力 wav> [--rate 48000] [--block 512] [--adc-silence]
//
// vst3probe と同じ MIDI・同じ標本化周波数・同じブロックの長さで流し、同じ形の wav を書く。
// 両方の wav を比べれば、CLAP の口で音が変わっていないかが分かる。
//
// --adc-silence は A/D INPUT（音声入力の口）に無音を渡す。付けなければ入力の口は繋がない。
// REAPER などは CLAP の入力の口をトラックのチャンネルに繋ぐので、その形を真似る

#include "clap/clap.h"
#include "smf.h"

#include "compat/console.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// ---- ホスト。何も頼まれても何もしない
const void *host_ext(const clap_host_t *, const char *) { return nullptr; }
void host_nop(const clap_host_t *) {}

const clap_host_t kHost = {
	CLAP_VERSION_INIT, nullptr, "clapprobe", "S-MU2000", "", "0.1",
	host_ext, host_nop, host_nop, host_nop,
};

// ---- 入ってくるイベントの並び。MIDI は 3 バイトまで中に持ち、SysEx は外を指す
struct in_events {
	std::vector<clap_event_midi_t>       midi;
	std::vector<clap_event_midi_sysex_t> sysex;
	std::vector<clap_event_note_t>       notes;
	std::vector<const clap_event_header_t *> order;
	std::vector<std::pair<int, size_t>> arrival;    // 来た順（0 MIDI / 1 SysEx / 2 ノート）
	clap_input_events_t api;

	in_events()
	{
		api.ctx = this;
		api.size = [](const clap_input_events_t *l) -> uint32_t {
			return uint32_t(static_cast<in_events *>(l->ctx)->order.size());
		};
		api.get = [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
			const auto *self = static_cast<in_events *>(l->ctx);
			return i < self->order.size() ? self->order[i] : nullptr;
		};
	}
};

bool out_push(const clap_output_events_t *, const clap_event_header_t *) { return true; }

void write_wav(const std::string &path, const std::vector<int16_t> &pcm, uint32_t rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const uint32_t bytes = uint32_t(pcm.size() * 2);
	auto u32w = [&](uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
	                              std::fwrite(b, 1, 4, f); };
	auto u16w = [&](uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) }; std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

} // namespace

int main(int argc, char **argv)
{
	smu2000::init_console_utf8();
	if (argc < 4) {
		std::fprintf(stderr, "使い方: clapprobe <S-MU2000.clap> <MIDI> <出力 wav> [--rate 48000] [--block 512] [--adc-silence]\n");
		return 1;
	}
	const std::string dll = argv[1], mid = argv[2], wav = argv[3];
	double rate = 48000.0;
	int block = 512;
	double extra = 3.0;
	bool adc = false;
	bool clap_notes = false;   // ノートオン・オフを CLAP 流（CLAP_EVENT_NOTE_*）で渡す
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) extra = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--adc-silence")) adc = true;
		else if (!std::strcmp(argv[i], "--clap-notes")) clap_notes = true;
	}

#if defined(_WIN32)
	HMODULE mod = LoadLibraryA(dll.c_str());
	if (!mod) { std::fprintf(stderr, "DLL を読めない: %s（エラー %lu）\n", dll.c_str(), GetLastError()); return 1; }
	const auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(mod, "clap_entry"));
#else
	void *mod = dlopen(dll.c_str(), RTLD_NOW);
	if (!mod) { std::fprintf(stderr, "読めない: %s\n", dll.c_str()); return 1; }
	const auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(mod, "clap_entry"));
#endif
	if (!entry || !entry->init(dll.c_str())) { std::fprintf(stderr, "clap_entry が無い\n"); return 1; }
	const auto *fac = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	if (!fac || fac->get_plugin_count(fac) < 1) { std::fprintf(stderr, "工場が無い\n"); return 1; }
	const clap_plugin_descriptor_t *desc = fac->get_plugin_descriptor(fac, 0);
	std::printf("%s（%s）\n", desc->name, desc->id);
	const clap_plugin_t *plug = fac->create_plugin(fac, &kHost, desc->id);
	if (!plug || !plug->init(plug)) { std::fprintf(stderr, "作れない\n"); return 1; }

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	const double length = events.empty() ? 0.0 : events.back().time;
	std::printf("MIDI: %zu イベント、%.2f 秒\n", events.size(), length);

	// activate が起動を待ちきる（issue #19）
	if (!plug->activate(plug, rate, 1, uint32_t(block))) { std::fprintf(stderr, "activate に失敗\n"); return 1; }
	plug->start_processing(plug);

	const size_t nb = static_cast<size_t>(block);
	std::vector<float> bl(nb), br(nb), il(nb), ir(nb);
	float *outs[2] = { bl.data(), br.data() };
	float *ins[2]  = { il.data(), ir.data() };
	clap_audio_buffer_t obuf{}; obuf.data32 = outs; obuf.channel_count = 2;
	clap_audio_buffer_t ibuf{}; ibuf.data32 = ins;  ibuf.channel_count = 2;
	in_events ev;
	clap_output_events_t oev{ nullptr, out_push };

	const int64_t total = int64_t((length + extra) * rate);
	std::vector<int16_t> pcm;
	pcm.reserve(size_t(total) * 2);
	size_t next = 0;
	int64_t pos = 0;
	while (pos < total) {
		const uint32_t n = uint32_t(std::min<int64_t>(block, total - pos));
		ev.midi.clear(); ev.sysex.clear(); ev.notes.clear(); ev.order.clear(); ev.arrival.clear();
		ev.midi.reserve(4096); ev.sysex.reserve(256); ev.notes.reserve(4096);
		std::fill(il.begin(), il.end(), 0.0f);
		std::fill(ir.begin(), ir.end(), 0.0f);
		while (next < events.size() && events[next].time * rate < double(pos + n)) {
			const smf::event &e = events[next++];
			if (e.bytes.empty())
				continue;
			const uint32_t off = uint32_t(std::clamp<int64_t>(int64_t(e.time * rate) - pos, 0, int64_t(n) - 1));
			const uint16_t port = uint16_t(e.port < 4 ? e.port : 3);
			if (e.bytes[0] == 0xf0) {
				clap_event_midi_sysex_t s{};
				s.header = { sizeof(s), off, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0 };
				s.port_index = port;
				s.buffer = e.bytes.data();
				s.size = uint32_t(e.bytes.size());
				ev.arrival.push_back({ 1, ev.sysex.size() });
				ev.sysex.push_back(s);
			} else if (clap_notes && e.bytes.size() >= 3 && ((e.bytes[0] & 0xe0) == 0x80)) {
				const bool on = (e.bytes[0] & 0xf0) == 0x90 && e.bytes[2];
				clap_event_note_t nt{};
				nt.header = { sizeof(nt), off, CLAP_CORE_EVENT_SPACE_ID, uint16_t(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
				nt.note_id = -1;
				nt.port_index = int16_t(port);
				nt.channel = int16_t(e.bytes[0] & 15);
				nt.key = int16_t(e.bytes[1]);
				nt.velocity = double(e.bytes[2]) / 127.0;
				ev.arrival.push_back({ 2, ev.notes.size() });
				ev.notes.push_back(nt);
			} else if (e.bytes[0] >= 0x80 && e.bytes[0] < 0xf0) {
				clap_event_midi_t m{};
				m.header = { sizeof(m), off, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
				m.port_index = port;
				for (size_t k = 0; k < 3 && k < e.bytes.size(); k++)
					m.data[k] = e.bytes[k];
				ev.arrival.push_back({ 0, ev.midi.size() });
				ev.midi.push_back(m);
			}
		}
		// 時刻順に並べる（同じ時刻なら来た順）。vector が伸び終わってから指す
		for (const auto &a : ev.arrival)
			ev.order.push_back(a.first == 1 ? &ev.sysex[a.second].header
			                    : a.first == 2 ? &ev.notes[a.second].header : &ev.midi[a.second].header);
		std::stable_sort(ev.order.begin(), ev.order.end(),
		                 [](const clap_event_header_t *a, const clap_event_header_t *b) { return a->time < b->time; });

		clap_process_t pr{};
		pr.steady_time = pos;
		pr.frames_count = n;
		pr.audio_inputs = adc ? &ibuf : nullptr;
		pr.audio_inputs_count = adc ? 1 : 0;
		pr.audio_outputs = &obuf;
		pr.audio_outputs_count = 1;
		pr.in_events = &ev.api;
		pr.out_events = &oev;
		plug->process(plug, &pr);
		for (uint32_t i = 0; i < n; i++) {
			// vst3probe と同じ丸め方
			pcm.push_back(int16_t(std::lround(std::clamp(bl[i], -1.0f, 1.0f) * 32767.0f)));
			pcm.push_back(int16_t(std::lround(std::clamp(br[i], -1.0f, 1.0f) * 32767.0f)));
		}
		pos += n;
	}

	plug->stop_processing(plug);
	plug->deactivate(plug);
	plug->destroy(plug);
	entry->deinit();

	write_wav(wav, pcm, uint32_t(rate));
	long peak = 0;
	double sum = 0;
	for (int16_t v : pcm) { peak = std::max<long>(peak, std::labs(v)); sum += std::fabs(double(v)); }
	std::printf("最大 %ld  平均 %.1f\n", peak, pcm.empty() ? 0.0 : sum / double(pcm.size()));
	std::printf("書き出した: %s（%.1f 秒 / %.0f Hz）\n", wav.c_str(), double(pcm.size() / 2) / rate, rate);
	return 0;
}
