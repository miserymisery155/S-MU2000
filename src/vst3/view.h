// license:BSD-3-Clause
//
// VST3 の画面（IPlugView）。ホストから渡された親ウィンドウの中に
// 子ウィンドウを 1 枚作り、gui.exe と同じ ui::panel で描く。
//
// SDK の土台（public.sdk / VSTGUI）は使っていないので、ここは素の Win32。

#ifndef S_MU2000_VST3_VIEW_H
#define S_MU2000_VST3_VIEW_H

#pragma once

#include "engine.h"
#include "ui/panel.h"

#include "pluginterfaces/gui/iplugview.h"

#include <windows.h>

namespace smu2000 {
namespace vst3 {

class plug_view : public Steinberg::IPlugView
{
public:
	explicit plug_view(engine &eng);
	virtual ~plug_view();

	// FUnknown
	Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
	Steinberg::uint32 PLUGIN_API addRef() override;
	Steinberg::uint32 PLUGIN_API release() override;

	// IPlugView
	Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
	Steinberg::tresult PLUGIN_API attached(void *parent, Steinberg::FIDString type) override;
	Steinberg::tresult PLUGIN_API removed() override;
	Steinberg::tresult PLUGIN_API onWheel(float distance) override;
	Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 code,
	                                        Steinberg::int16 modifiers) override;
	Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 code,
	                                      Steinberg::int16 modifiers) override;
	Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect *size) override;
	Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) override;
	Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame *frame) override;
	Steinberg::tresult PLUGIN_API canResize() override;
	Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *rect) override;

private:
	static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	void paint(HWND h);
	void card_menu(HWND h, int x, int y);
	void card_command(HWND h, UINT id);

	engine &m_engine;

	HWND  m_hwnd = nullptr;
	ui::panel m_panel;

	HDC     m_mem_dc = nullptr;
	HBITMAP m_mem_bmp = nullptr;
	int     m_mem_w = 0, m_mem_h = 0;
	DWORD   m_last_flush = 0;          // SmartMedia を最後に書き戻した時刻

	int m_w = 1400, m_h = 360;
	Steinberg::int32 m_refs = 1;
	Steinberg::IPlugFrame *m_frame = nullptr;
};

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_VIEW_H
