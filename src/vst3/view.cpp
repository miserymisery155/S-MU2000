// license:BSD-3-Clause

#include "view.h"

#include "ui/bridge.h"
#include "ui/layout.h"

#include <algorithm>
#include <cstdio>

#include <windowsx.h>

using namespace Steinberg;

namespace smu2000 {
namespace vst3 {

namespace {

const char *kClassName = "SMU2000PlugView";

// 窓のクラスはこの DLL で 1 度だけ登録する
HINSTANCE this_module()
{
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCSTR>(&this_module), &self);
	return HINSTANCE(self);
}

void register_class(WNDPROC proc)
{
	static bool done = false;
	if (done)
		return;
	WNDCLASSA wc{};
	wc.lpfnWndProc   = proc;
	wc.hInstance     = this_module();
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kClassName;
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);
	done = true;
}

// ホストによってはキーがこちらに回ってくる。gui.exe と同じ割り当て
mu2000::button key_to_button(WPARAM vk, bool &ok)
{
	ok = true;
	switch (vk) {
	case 'A': return mu2000::button::play;
	case 'E': return mu2000::button::edit;
	case 'U': return mu2000::button::util;
	case 'F': return mu2000::button::effect;
	case 'S': return mu2000::button::mute_solo;
	case VK_OEM_6: return mu2000::button::part_plus;
	case VK_OEM_4: return mu2000::button::part_minus;
	case VK_OEM_PLUS:  return mu2000::button::value_plus;
	case VK_OEM_MINUS: return mu2000::button::value_minus;
	case VK_BACK:   return mu2000::button::exit;
	case VK_RETURN: return mu2000::button::enter;
	case VK_OEM_PERIOD: return mu2000::button::select_right;
	case VK_OEM_COMMA:  return mu2000::button::select_left;
	case 'Q': return mu2000::button::seq;
	case 'Z': return mu2000::button::audition;
	case 'X': return mu2000::button::select;
	case 'M': return mu2000::button::sampling_mode;
	default: break;
	}
	ok = false;
	return mu2000::button::count;
}

} // namespace


plug_view::plug_view(engine &eng)
	: m_engine(eng)
{
	// パネルの配置。%LOCALAPPDATA%\S-MU2000\panel.txt があれば読む
	// （doc/panel-editing.md）。無ければ組み込みの配置のまま
	const std::string lay = ui::layout::find_default();
	if (!lay.empty()) {
		std::string err;
		m_panel.lay().load(lay, err);
	}
	m_panel.resize(m_w, m_h);
}

plug_view::~plug_view()
{
	removed();
}

tresult PLUGIN_API plug_view::queryInterface(const TUID iid, void **obj)
{
	if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) ||
	    FUnknownPrivate::iidEqual(iid, IPlugView::iid)) {
		addRef();
		*obj = static_cast<IPlugView *>(this);
		return kResultOk;
	}
	*obj = nullptr;
	return kNoInterface;
}

uint32 PLUGIN_API plug_view::addRef()  { return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }

uint32 PLUGIN_API plug_view::release()
{
	if (FUnknownPrivate::atomicAdd(m_refs, -1) == 0) { delete this; return 0; }
	return uint32(m_refs);
}

tresult PLUGIN_API plug_view::isPlatformTypeSupported(FIDString type)
{
	return (type && !std::strcmp(type, kPlatformTypeHWND)) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API plug_view::attached(void *parent, FIDString type)
{
	if (isPlatformTypeSupported(type) != kResultTrue || !parent)
		return kResultFalse;
	if (m_hwnd)
		removed();

	register_class(&plug_view::wnd_proc);
	m_hwnd = CreateWindowExA(0, kClassName, "", WS_CHILD | WS_VISIBLE,
	                         0, 0, m_w, m_h, HWND(parent), nullptr,
	                         this_module(), nullptr);
	if (!m_hwnd)
		return kResultFalse;

	SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, LONG_PTR(this));
	SetTimer(m_hwnd, 1, 33, nullptr);        // 30 コマ／秒
	m_panel.resize(m_w, m_h);
	return kResultOk;
}

tresult PLUGIN_API plug_view::removed()
{
	if (m_hwnd) {
		KillTimer(m_hwnd, 1);
		SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
		DestroyWindow(m_hwnd);
		m_hwnd = nullptr;
	}
	if (m_mem_bmp) { DeleteObject(m_mem_bmp); m_mem_bmp = nullptr; }
	if (m_mem_dc)  { DeleteDC(m_mem_dc); m_mem_dc = nullptr; }
	m_mem_w = m_mem_h = 0;
	return kResultOk;
}

// ホスト経由の入力は使わない。子ウィンドウが本物のメッセージを受け取る
tresult PLUGIN_API plug_view::onWheel(float)                          { return kResultFalse; }
tresult PLUGIN_API plug_view::onKeyDown(char16, int16, int16)         { return kResultFalse; }
tresult PLUGIN_API plug_view::onKeyUp(char16, int16, int16)           { return kResultFalse; }
tresult PLUGIN_API plug_view::onFocus(TBool)                          { return kResultOk; }

tresult PLUGIN_API plug_view::getSize(ViewRect *size)
{
	if (!size)
		return kInvalidArgument;
	size->left = 0; size->top = 0;
	size->right = m_w; size->bottom = m_h;
	return kResultOk;
}

tresult PLUGIN_API plug_view::onSize(ViewRect *r)
{
	if (!r)
		return kInvalidArgument;
	m_w = std::max<int32>(r->getWidth(), 640);
	m_h = std::max<int32>(r->getHeight(), 180);
	if (m_hwnd)
		MoveWindow(m_hwnd, 0, 0, m_w, m_h, TRUE);
	m_panel.resize(m_w, m_h);
	return kResultOk;
}

tresult PLUGIN_API plug_view::setFrame(IPlugFrame *frame)
{
	m_frame = frame;
	return kResultOk;
}

tresult PLUGIN_API plug_view::canResize() { return kResultTrue; }

tresult PLUGIN_API plug_view::checkSizeConstraint(ViewRect *rect)
{
	if (!rect)
		return kInvalidArgument;
	// 横に長い機械なので、縦横比はこちらで決めてしまう
	const int w = std::max<int32>(rect->getWidth(), 640);
	const int h = std::max<int32>(w * ui::LOGICAL_H / ui::LOGICAL_W, 180);
	rect->right = rect->left + w;
	rect->bottom = rect->top + h;
	return kResultTrue;
}


LRESULT CALLBACK plug_view::wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	plug_view *self = reinterpret_cast<plug_view *>(GetWindowLongPtrA(h, GWLP_USERDATA));
	if (!self)
		return DefWindowProcA(h, msg, wp, lp);
	return self->handle(h, msg, wp, lp);
}

void plug_view::paint(HWND h)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(h, &ps);
	RECT cr;
	GetClientRect(h, &cr);
	const int w = cr.right, hh = cr.bottom;

	if (!m_mem_dc || m_mem_w != w || m_mem_h != hh) {
		if (m_mem_bmp) DeleteObject(m_mem_bmp);
		if (m_mem_dc)  DeleteDC(m_mem_dc);
		m_mem_dc = CreateCompatibleDC(dc);
		m_mem_bmp = CreateCompatibleBitmap(dc, w, hh);
		SelectObject(m_mem_dc, m_mem_bmp);
		m_mem_w = w;
		m_mem_h = hh;
	}

	ui::snapshot s;
	m_engine.panel().read(s);

	char status[160];
	std::snprintf(status, sizeof(status), "%s", m_engine.message().c_str());

	m_panel.set_volume(m_engine.panel().gain());
	m_panel.paint(m_mem_dc, s, m_engine.panel().buttons(), status);
	BitBlt(dc, 0, 0, w, hh, m_mem_dc, 0, 0, SRCCOPY);
	EndPaint(h, &ps);
}

LRESULT plug_view::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	ui::bridge &br = m_engine.panel();

	switch (msg) {
	case WM_TIMER:
		// パラメータの層: 音源の返事を読み、見えている面の読み返しを頼む
		m_panel.tick(br);
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
		paint(h);
		return 0;

	case WM_SIZE:
		m_panel.resize(LOWORD(lp), HIWORD(lp));
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_LBUTTONDOWN:
		SetCapture(h);
		if (m_panel.press(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), br))
			InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEMOVE:
		if (m_panel.drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), br))
			InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_LBUTTONUP:
		m_panel.release(br);
		ReleaseCapture();
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(h, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (delta && m_panel.wheel_at(pt.x, pt.y, delta, br))
			InvalidateRect(h, nullptr, FALSE);
		return 0;
	}

	case WM_KEYDOWN: {
		if (lp & (1 << 30))
			return 0;
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) br.press(b, true);
		return 0;
	}

	case WM_KEYUP: {
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) br.press(b, false);
		return 0;
	}

	case WM_KILLFOCUS:
		br.release_all();
		return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}

} // namespace vst3
} // namespace smu2000
