// license:BSD-3-Clause
//
// ImGui の画面（imgui_view）を載せる窓（gui.exe 用）。Win32 の窓に Direct3D 11 で描く。
//
// 窓は gui の画面の糸で作り、gui のタイマーから frame() を呼んで描く。
// 閉じても消さずに隠すだけなので、開き直すと同じ状態で出る。
// **窓ごとに ImGui の文脈を持つ**ので、エディタと一覧を同時に開ける。

#ifndef S_MU2000_UI_PC_WINDOW_H
#define S_MU2000_UI_PC_WINDOW_H

#pragma once

#include "xg_ui.h"

#include <memory>
#include <string>

#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ImGuiContext;

namespace ui {

class pc_window
{
public:
	explicit pc_window(std::unique_ptr<imgui_view> view) : m_view(std::move(view)) {}
	~pc_window();

	// 出す。初めてなら窓と描画装置を作る。失敗したら err に理由
	bool show(HINSTANCE inst, std::string &err);
	bool visible() const;
	// gui を終えるとき。中身に「閉じた」と知らせる（ミュートを外すなど）
	void shutdown(bridge &br);

	// タイマーから。見えていなければ何もしない
	void frame(xg::model &m, const xg_snapshot &ram, bridge &br);

	// ファイルを窓に落とされたときに呼ぶ先（gui が MIDI ファイルを流す）。
	// 窓を作る前に決めておく。決めていなければ落とせない
	static void set_drop_handler(void (*fn)(const std::wstring &path)) { s_drop = fn; }

private:
	bool create(HINSTANCE inst, std::string &err);
	bool create_device(std::string &err);
	void make_target();
	void drop_target();
	void destroy();
	static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	static inline void (*s_drop)(const std::wstring &) = nullptr;

	std::unique_ptr<imgui_view> m_view;
	HWND m_hwnd = nullptr;
	ID3D11Device           *m_dev = nullptr;
	ID3D11DeviceContext    *m_ctx = nullptr;
	IDXGISwapChain         *m_swap = nullptr;
	ID3D11RenderTargetView *m_rtv = nullptr;
	ImGuiContext           *m_imgui = nullptr;
	UINT m_resize_w = 0, m_resize_h = 0;     // WM_SIZE で受けて、次に描く前に直す
	bool m_was_visible = false;              // 前のコマで見えていたか（隠れた瞬間を知る）
};

} // namespace ui

#endif // S_MU2000_UI_PC_WINDOW_H
