// license:BSD-3-Clause
// Small VST2 host used to verify the DLL ABI, MIDI path, state, and editor.

#include "vst2_abi.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace smu2000::vsti;

namespace {

using entry_fn = effect *(SMU_VSTCALLBACK *)(host_callback);

vintptr SMU_VSTCALLBACK host(effect *, vint32 opcode, vint32, vintptr, void *, float)
{
	return opcode == host_version ? 2400 : 0;
}

bool finite_audio(const std::vector<float> &v)
{
	for (float x : v)
		if (!std::isfinite(x))
			return false;
	return true;
}

// **冷えた起動の試験**（--coldstate。issue #51）。vstmididrv のように、起動が終わる前に
// 保存した状態（chunk）を戻し、すぐ曲頭（XG System On → 音色の指定 → ノートオン）を流す。
// 状態を戻さない台と出力が同じなら、曲頭の音色が消えていない
std::vector<float> cold_run(entry_fn entry, const std::vector<std::uint8_t> *chunk)
{
	effect *fx = entry(host);
	fx->dispatcher(fx, eff_open, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_set_sample_rate, 0, 0, nullptr, 44100.0f);
	fx->dispatcher(fx, eff_set_block_size, 0, 512, nullptr, 0);
	if (chunk)
		fx->dispatcher(fx, eff_set_chunk, 0, vintptr(chunk->size()),
		               const_cast<std::uint8_t *>(chunk->data()), 0);
	fx->dispatcher(fx, eff_mains_changed, 0, 1, nullptr, 0);

	static char xg_on[] = { char(0xf0), 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, char(0xf7) };
	sysex_event sx{};
	sx.type = sysex_type;
	sx.byte_size = sysex_event_byte_size;
	sx.dump_bytes = vint32(sizeof(xg_on));
	sx.dump = xg_on;
	midi_event pc{}, on{};
	pc.type = on.type = midi_type;
	pc.byte_size = on.byte_size = midi_event_byte_size;
	pc.midi_data[0] = char(0xc0);
	pc.midi_data[1] = 48;                         // Strings
	on.midi_data[0] = char(0x90);
	on.midi_data[1] = 60;
	on.midi_data[2] = 100;
	struct { vint32 count; vintptr reserved; event *items[3]; } list{};
	list.count = 3;
	list.items[0] = reinterpret_cast<event *>(&sx);
	list.items[1] = reinterpret_cast<event *>(&pc);
	list.items[2] = reinterpret_cast<event *>(&on);
	fx->dispatcher(fx, eff_process_events, 0, 0, &list, 0);

	std::vector<float> out;
	std::vector<float> l(512), r(512);
	float *outputs[2] = { l.data(), r.data() };
	for (int blk = 0; blk < 44100 * 3 / 512; blk++) {
		fx->process_replacing(fx, nullptr, outputs, 512);
		out.insert(out.end(), l.begin(), l.end());
	}
	fx->dispatcher(fx, eff_mains_changed, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "build/S-MU2000.dll";
	const bool require_audio = argc > 2 && !std::strcmp(argv[2], "--audio");
	const bool coldstate = argc > 2 && !std::strcmp(argv[2], "--coldstate");
	HMODULE module = LoadLibraryA(path);
	if (!module) {
		std::fprintf(stderr, "cannot load %s (%lu)\n", path, GetLastError());
		return 1;
	}
	auto entry = reinterpret_cast<entry_fn>(GetProcAddress(module, "VSTPluginMain"));
	if (!entry) {
		std::fprintf(stderr, "VSTPluginMain is missing\n");
		FreeLibrary(module);
		return 1;
	}
	effect *fx = entry(host);
	if (!fx || fx->magic != effect_magic || !fx->dispatcher || !fx->process_replacing) {
		std::fprintf(stderr, "invalid VST2 effect\n");
		FreeLibrary(module);
		return 1;
	}
	if (fx->num_outputs != 2 || !(fx->flags & is_synth) ||
	    fx->dispatcher(fx, eff_get_plug_category, 0, 0, nullptr, 0) != category_synth) {
		std::fprintf(stderr, "plugin is not a stereo instrument\n");
		FreeLibrary(module);
		return 1;
	}

	fx->dispatcher(fx, eff_open, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_set_sample_rate, 0, 0, nullptr, 44100.0f);
	fx->dispatcher(fx, eff_set_block_size, 0, 64, nullptr, 0);
	fx->dispatcher(fx, eff_mains_changed, 0, 1, nullptr, 0);

	midi_event note{};
	note.type = midi_type;
	// The VST2 ABI reports the event payload size, not sizeof(VstMidiEvent).
	// Hosts such as FL Studio send the canonical value 24.
	note.byte_size = midi_event_byte_size;
	note.delta_frames = 7;
	note.midi_data[0] = 0x90;
	note.midi_data[1] = 60;
	note.midi_data[2] = 100;
	events incoming{};
	incoming.count = 1;
	incoming.items[0] = reinterpret_cast<event *>(&note);
	if (!fx->dispatcher(fx, eff_process_events, 0, 0, &incoming, 0)) {
		std::fprintf(stderr, "MIDI event was rejected\n");
		return 1;
	}

	std::vector<float> left(64), right(64);
	float *outputs[2] = { left.data(), right.data() };
	fx->process_replacing(fx, nullptr, outputs, 64);
	if (!finite_audio(left) || !finite_audio(right)) {
		std::fprintf(stderr, "non-finite audio\n");
		return 1;
	}
	if (require_audio) {
		bool audible = false;
		for (int block = 0; block < 30000 && !audible; block++) {
			fx->process_replacing(fx, nullptr, outputs, 64);
			for (int i = 0; i < 64; i++)
				if (std::fabs(left[i]) > 1.0e-7f || std::fabs(right[i]) > 1.0e-7f) {
					audible = true;
					break;
				}
		}
		if (!audible) {
			std::fprintf(stderr, "MIDI produced no audio after firmware boot\n");
			return 1;
		}
	}

	void *chunk = nullptr;
	const vintptr chunk_size = fx->dispatcher(fx, eff_get_chunk, 0, 0, &chunk, 0);
	if (chunk_size < 16 || !chunk) {
		std::fprintf(stderr, "state chunk is missing\n");
		return 1;
	}
	std::vector<std::uint8_t> saved(static_cast<std::uint8_t *>(chunk),
	                                static_cast<std::uint8_t *>(chunk) + chunk_size);
	if (coldstate) {
		// 起動済みの台の状態（ピアノのまま）を、起動中の台に戻してから曲頭を流す
		// 波形は内部の位相で揃わないので、100ms ごとの音量の動きで比べる
		// （Strings はゆっくり立ち上がって伸び、ピアノは一気に立って減衰する）
		const std::vector<float> with = cold_run(entry, &saved);
		const std::vector<float> ref = cold_run(entry, nullptr);
		auto env = [](const std::vector<float> &x) {
			std::vector<double> v;
			for (size_t at = 0; at + 4410 <= x.size(); at += 4410) {
				double e = 0;
				for (size_t i = at; i < at + 4410; i++)
					e += double(x[i]) * x[i];
				v.push_back(10.0 * std::log10(e / 4410.0 + 1e-20));
			}
			return v;
		};
		const std::vector<double> a = env(with), b = env(ref);
		double worst = 0;
		std::printf("冷えた起動（100ms ごとの音量 dB。上が状態を戻した台、下が戻さない台）\n");
		for (size_t i = 0; i < a.size() && i < b.size(); i++) {
			std::printf("  %5.1f / %5.1f\n", a[i], b[i]);
			if (b[i] > -80.0)
				worst = std::max(worst, std::fabs(a[i] - b[i]));
		}
		std::printf("いちばん違った所 %.1f dB（1 dB より小さければ同じ音色）\n", worst);
		fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
		FreeLibrary(module);
		return worst < 1.0 ? 0 : 3;
	}
	if (!fx->dispatcher(fx, eff_set_chunk, 0, chunk_size, saved.data(), 0)) {
		std::fprintf(stderr, "state chunk was rejected\n");
		return 1;
	}

	rect *editor_rect = nullptr;
	if (!fx->dispatcher(fx, eff_edit_get_rect, 0, 0, &editor_rect, 0) || !editor_rect ||
	    editor_rect->right <= editor_rect->left || editor_rect->bottom <= editor_rect->top) {
		std::fprintf(stderr, "editor rectangle is invalid\n");
		return 1;
	}
	HWND parent = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPED,
	                              0, 0, editor_rect->right, editor_rect->bottom,
	                              nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	if (parent) {
		if (!fx->dispatcher(fx, eff_edit_open, 0, 0, parent, 0)) {
			std::fprintf(stderr, "editor could not attach\n");
			return 1;
		}
		fx->dispatcher(fx, eff_edit_close, 0, 0, nullptr, 0);
		DestroyWindow(parent);
	}

	char name[64] = {};
	fx->dispatcher(fx, eff_get_effect_name, 0, 0, name, 0);
	fx->dispatcher(fx, eff_mains_changed, 0, 0, nullptr, 0);
	fx->dispatcher(fx, eff_close, 0, 0, nullptr, 0);
	FreeLibrary(module);
	std::printf("VSTi probe passed: %s, stereo, MIDI%s, state, editor\n",
	            name, require_audio ? "/audio" : "");
	return 0;
}
