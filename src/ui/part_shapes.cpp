// license:BSD-3-Clause

#include "part_shapes.h"

#include "overview.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui {

using namespace xgui;

namespace {

// パートは口 A-D の 64（C・D は実機では USB だけの口）
constexpr int PARTS = XG_PARTS;

// 「グラフ ○ つまみ」の切り替え。見出しの行の右端に描き、押されたら true。
// 見出しと並べて入らなければ字を外して切り替えだけにし、それでも入らなければ見出しを切る
bool title_toggle(const char *title, const char *id, bool knobs)
{
	const float fs = ImGui::GetFontSize();
	const char *l = "グラフ", *r = "つまみ";
	const float lw = ImGui::CalcTextSize(l).x, rw = ImGui::CalcTextSize(r).x;
	const float th = fs * 0.9f, tw = th * 1.8f, gap = fs * 0.3f;
	const float room = ImGui::GetContentRegionAvail().x;
	const float title_w = ImGui::CalcTextSize(title).x;
	const bool words = title_w + fs + lw + gap + tw + gap + rw <= room;
	const float total = words ? lw + gap + tw + gap + rw : tw;
	const ImVec2 start = ImGui::GetCursorScreenPos();
	const float line_h = ImGui::GetTextLineHeight();
	// 見出し（切り替えにかからないところまで）
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float title_room = std::max(0.0f, room - total - fs * 0.5f);
	dl->PushClipRect(start, ImVec2(start.x + title_room, start.y + line_h), true);
	dl->AddText(start, ImGui::GetColorU32(ImGuiCol_Text), title);
	dl->PopClipRect();
	ImGui::Dummy(ImVec2(title_room, line_h));
	if (ImGui::IsItemHovered() && title_w > title_room)
		hint("%s", title);
	ImGui::SameLine(0, 0);
	ImGui::SetCursorScreenPos(ImVec2(start.x + room - total, start.y));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(total, line_h));
	hint(knobs ? "いまは「つまみ」（値の棒で触る）。クリックで「グラフ」（絵で触る）に切り替える"
	           : "いまは「グラフ」（絵で触る）。クリックで「つまみ」（値の棒で触る）に切り替える");
	const ImU32 on = ImGui::GetColorU32(ImGuiCol_Text), off = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	float sx = p.x;
	if (words) {
		dl->AddText(p, knobs ? off : on, l);
		dl->AddText(ImVec2(p.x + lw + gap * 2 + tw, p.y), knobs ? on : off, r);
		sx += lw + gap;
	}
	const float ty = p.y + (line_h - th) * 0.5f;
	const ImVec2 a(sx, ty), b(sx + tw, ty + th);
	dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), th * 0.5f);
	const float kx = knobs ? b.x - th * 0.5f : a.x + th * 0.5f;
	dl->AddCircleFilled(ImVec2(kx, ty + th * 0.5f), th * 0.38f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
	return pressed;
}

// 1 つの区画。見出しと、大きな絵か値の棒（右上の切り替えで選ぶ。index が負なら絵は無く棒だけ）
template <typename Draw>
void panel(const char *id, const char *title, float w, float h, int part, xg::model &m, bridge &br,
           std::initializer_list<const char *> keys, int index, Draw draw)
{
	const float fs = ImGui::GetFontSize();
	// 見出しを枠の上端に寄せる（上下の余白を詰める）
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, fs * 0.1f));
	const bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders);
	ImGui::PopStyleVar();
	if (!open) {
		ImGui::EndChild();
		return;
	}
	const bool knobs = index < 0 || shapes_knobs(index);
	ImGui::PushFont(nullptr, fs * 0.8f);      // 見出しは小さめに
	if (index < 0)
		ImGui::TextUnformatted(title);
	else if (title_toggle(title, "##mode", knobs))
		set_shapes_knobs(index, !knobs);
	ImGui::PopFont();
	if (!knobs) {
		// 絵だけ。区画の残りを全部使う
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		draw(part, m, br, avail.x, std::max(fs * 4.0f, avail.y));
		ImGui::EndChild();
		return;
	}
	ImGui::PushItemWidth(-fs * 6.0f);
	for (const char *k : keys)
		param_slider(k, part, m, br);
	ImGui::PopItemWidth();
	ImGui::EndChild();
}

} // namespace


void part_shapes::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	set_current_ram(&ram);            // 絵が音色の中身を読むため（ピッチ EG など）
	begin_hint_bar();                 // 絵や名前の説明は、マウスのそばでなく下の帯に出す
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("part_shapes", nullptr, wf);
	ImGui::PopStyleVar();

	// 表示の大きさ。文字も絵も同じ倍率で縮む（editor.ini に覚える）
	float &zoom = shapes_zoom();
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);

	const float fs = ImGui::GetFontSize();
	int part = shape_window_part();

	// ---- パートを選ぶ。音色の名前も出す
	ImGui::SetNextItemWidth(fs * 5);
	if (ImGui::BeginCombo("##part", part_name(part).c_str(), ImGuiComboFlags_HeightLarge)) {
		for (int p = 0; p < PARTS; p++) {
			if (p && p % 16 == 0)
				ImGui::Separator();          // 口の境目
			if (ImGui::Selectable(part_name(p).c_str(), p == part))
				set_shape_window_part(part = p);
			if (p == part && ImGui::IsWindowAppearing())
				ImGui::SetScrollHereY();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
		set_shape_window_part(part = (part + PARTS - 1) % PARTS);
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right))
		set_shape_window_part(part = (part + 1) % PARTS);
	ImGui::SameLine();
	int msb = 0, lsb = 0, prog = 0;
	std::string voice = "--";
	if (m.get(P("part.bank_msb"), part, msb) && m.get(P("part.bank_lsb"), part, lsb) && m.get(P("part.program"), part, prog)) {
		voice = voice_text(msb, lsb, prog);
		if (const xg::voice_rom *vr = voices()) {
			const std::string real = vr->name(ram.parts[part], msb, prog);
			if (!real.empty()) {
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%3d  %s", prog + 1, real.c_str());
				voice = buf;
			}
		}
	}
	ImGui::TextUnformatted(voice.c_str());

	// 表示の大きさと、説明のチェックボックスは右端へ
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fs * 18);
	if (ImGui::SmallButton("-"))
		set_shapes_zoom(zoom - 0.1f);
	ImGui::SameLine();
	ImGui::Text("%d%%", int(std::lround(zoom * 100)));
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		set_shapes_zoom(zoom + 0.1f);
	ImGui::SameLine();
	help_checkbox();

	// ---- 上のペイン: エフェクト、棒、鍵盤
	const ImGuiStyle &st = ImGui::GetStyle();
	{
		// インサーションの行、見出しと棒の行、鍵盤の行
		const float strip_h = overview::part_strip_height() + st.WindowPadding.y * 2.0f + fs * 0.2f;
		if (ImGui::BeginChild("strip", ImVec2(0, strip_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar))
			m_strip.part_strip(part, m, ram, br);
		ImGui::EndChild();
	}

	// ---- 左に音色を選ぶ面、右はタブ: 「形」は 4 つの区画（2 × 2）、「すべて」はパートのパラメータ全部
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	// 音色を選ぶ面は、左に分類・右に音色とバンク違いの 2 列（xgui::program_pane）
	const float pane_w = std::min(fs * 15.6f, avail.x * 0.3f);     // 前の 6 割
	// 下の説明の帯（4 行ぶん。入り切らなかった字の行と、説明の 3 行）を残す
	const float bar_h = ImGui::GetTextLineHeightWithSpacing() * 4.0f + st.WindowPadding.y * 2.0f;
	const float body_h = std::max(fs * 8.0f, avail.y - bar_h - st.ItemSpacing.y);

	if (ImGui::BeginChild("voicepane", ImVec2(pane_w, body_h)))
		program_pane(part, m, &ram, br);
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginGroup();
	const float top_y = ImGui::GetCursorScreenPos().y;
	if (ImGui::BeginTabBar("right")) {
		if (ImGui::BeginTabItem("形")) {
			// 3 × 2。上に VIB・FILTER・EG、下にピッチ EG・EQ・ポルタメント（絵は無く棒だけ）
			const ImVec2 room = ImGui::GetContentRegionAvail();
			const float room_h = body_h - (ImGui::GetCursorScreenPos().y - top_y);
			const float w = (room.x - st.ItemSpacing.x * 2.0f) / 3.0f;
			const float h = (room_h - st.ItemSpacing.y) * 0.5f;
			panel("vib", "ビブラート（VIB）", w, h, part, m, br, { "part.vib_rate", "part.vib_depth", "part.vib_delay" }, 0,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::vib_cell(p, mm, b, pw, ph, false); });
			ImGui::SameLine();
			panel("filter", "フィルタ（FILTER）", w, h, part, m, br, { "part.cutoff", "part.resonance", "part.hpf_cutoff" }, 1,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::filter_cell(p, mm, b, pw, ph, false); });
			ImGui::SameLine();
			panel("eg", "音量の形（EG）", w, h, part, m, br, { "part.attack", "part.decay", "part.release" }, 2,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::eg_cell(p, mm, b, pw, ph, false); });
			panel("peg", "音程の形（ピッチ EG）", w, h, part, m, br,
			      { "part.peg_init_level", "part.peg_attack_time", "part.peg_rel_level", "part.peg_rel_time" }, 3,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::peg_cell(p, mm, b, pw, ph, false); });
			ImGui::SameLine();
			panel("eq", "パートの EQ", w, h, part, m, br,
			      { "part.eq_bass_gain", "part.eq_bass_freq", "part.eq_treble_gain", "part.eq_treble_freq" }, 4,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::eq_cell(p, mm, b, pw, ph, false); });
			ImGui::SameLine();
			panel("porta", "ポルタメント", w, h, part, m, br, { "part.porta_switch", "part.porta_time" }, -1,
			      [](int, xg::model &, bridge &, float pw, float ph) { ImGui::Dummy(ImVec2(pw, ph)); });
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("すべて")) {
			// エディタのパートの面と同じ組（xgui::PART_GROUPS）を、幅に合わせた列数で並べる
			if (ImGui::BeginChild("all", ImVec2(0, body_h - (ImGui::GetCursorScreenPos().y - top_y)))) {
				const int columns = std::clamp(int(ImGui::GetContentRegionAvail().x / (fs * 16.0f)), 1, 3);
				if (ImGui::BeginTable("groups", columns, ImGuiTableFlags_SizingStretchSame)) {
					int n = 0;
					for (const part_group &g : PART_GROUPS) {
						if (n++ % columns == 0)
							ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::SeparatorText(g.title);
						ImGui::PushItemWidth(-fs * 6.0f);
						for (const char *key : g.keys) {
							if (!key)
								break;
							param_slider(key, part, m, br);
						}
						ImGui::PopItemWidth();
						ImGui::Spacing();
					}
					ImGui::EndTable();
				}
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::EndGroup();

	// ---- 説明の帯。カーソルを載せた絵・値・名前の説明（無ければ使い方のひとこと）
	if (ImGui::BeginChild("hint", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		// 絵に入り切らなかった点の字は、いつもここの頭に
		const std::string &hv = hidden_values();
		if (!hv.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 214, 120, 255));
			ImGui::TextWrapped("%s", hv.c_str());
			ImGui::PopStyleColor();
		}
		const std::string &t = hint_text();
		if (t.empty())
			ImGui::TextDisabled("絵の点や値、名前にカーソルを載せると、ここに説明が出る");
		else
			ImGui::TextWrapped("%s", t.c_str());
	}
	ImGui::EndChild();
	end_hint_bar();

	ImGui::PopFont();
	ImGui::End();
}

} // namespace ui
