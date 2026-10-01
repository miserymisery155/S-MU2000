// license:BSD-3-Clause
//
// The Linux window system: the SDL3 window, its event pump and the panel
// painted through Dear ImGui. The Windows and macOS twins are
// ui/window_win.* and ui/window_mac.*; the declarations live in
// ui/window_sdl.h and nothing here is seen by gui_linux.cpp, which keeps
// only main().
//
// SDL has no menus, so the popups are drawn into the window itself
// (ui/sdl_popup) over the live panel; the shared menu content comes from
// ui::app::context_menu, dispatched by ui::app::menu_chosen.

#include "window_sdl.h"

#include "app_linux.h"
#include "compat/gdi.h"
#include "mu2000.h"
#include "ui/engine.h"
#include "ui/panel.h"
#include "ui/pc_host.h"
#include "ui/png.h"
#include "ui/sdl_popup.h"
#include "ui/toolbar.h"
#include "ui/xg_ui.h"

#include "ui/imgui_shell_sdl.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = ui::AUDIO_RATE;

// SDL keycodes translated into the shared key space (ui/keymap.h): the four
// F-keys the app acts on, the panel characters, 0 for everything else
int sdl_key_to_shared(int k)
{
	switch (k) {
	case SDLK_F2: return ui::KEY_F2;
	case SDLK_F3: return ui::KEY_F3;
	case SDLK_F4: return ui::KEY_F4;
	case SDLK_F5: return ui::KEY_F5;
	default: break;
	}
	return k < 128 ? k : 0;
}

// The context menu at a point: the shared groups (ui::app::context_menu),
// rendered through sdl_popup by the app, then the shared dispatch. The id
// reaching menu_chosen is exactly what WM_COMMAND receives on Windows
void open_menu(ui::linux_app &gui, int x, int y)
{
	const std::vector<ui::menu_group> groups = gui.context_menu(x, y);
	if (groups.empty())
		return;
	const int id = gui.show_popup(groups, x, y);
	if (id >= 0)
		gui.menu_chosen(id);
}

} // namespace

namespace ui {

// ui::linux_app::run_list: the modal popup over the live panel. behind()
// paints the same live frame the pump shows.
int linux_app::run_list(const std::vector<sdl_popup::item> &items, int x, int y,
                        int &sub_chosen)
{
	auto behind = [&](ImDrawList *dl) { paint_main(dl, imgui_fonts, ww); };
	return sdl_popup::run(win, ren, imgui_ctx, imgui_fonts, ww, wh,
	                      behind, quit, items, x, y, sub_chosen);
}

// ---- the SDL3 window pump (ui::app::run calls it through pump_window) ------
//
// One window, one renderer, one ImGui context painting the panel. The
// PC editor windows keep their own SDL windows and eat their events first
// (ui/pc_window_linux.cpp).

int run_window(linux_app &gui, const char *title, int w, int h)
{
	SDL_Window *win = SDL_CreateWindow(title, w, h, SDL_WINDOW_RESIZABLE);
	if (!win) {
		std::fprintf(stderr, "窓を出せない: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
	if (!ren) {
		std::fprintf(stderr, "描画器を作れない: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}
	SDL_Cursor *cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
	SDL_Cursor *cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);

	gui.win = win;
	gui.ren = ren;
	gui.ww = w;
	gui.wh = h;

	ui::imshell::sdl_state im{};
	if (!ui::imshell::sdl_start(im, win, ren)) {
		std::fprintf(stderr, "ImGui 描画を始められない: %s\n", SDL_GetError());
		if (cur_hand) SDL_DestroyCursor(cur_hand);
		if (cur_arrow) SDL_DestroyCursor(cur_arrow);
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}
	gui.imgui_ctx = im.ctx;         // run_list's menu uses the same frame
	gui.imgui_fonts = im.fonts;
	gui.panel.fonts_ready();     // the context is up and no frame is open

	bool down = false;   // left button held: drags go to the panel
	const Uint64 quit_at = gui.seconds_limit > 0.0
	                           ? SDL_GetTicks() + Uint64(gui.seconds_limit * 1000.0)
	                           : 0;
	while (!gui.quit.load()) {
		if (quit_at && SDL_GetTicks() >= quit_at)
			break;   // --seconds: timed run, for smoke tests and demos
		const Uint64 frame_at = SDL_GetTicks() + 33;
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			// PC editor windows first: they own their SDL windows and eat
			// their events (including drops and the close button).
			if (pc_window::route_event(ev))
				continue;
			ImGui_ImplSDL3_ProcessEvent(&ev);   // keeps ImGui IO sane; panel input below stays shared
			switch (ev.type) {
			case SDL_EVENT_QUIT:
				gui.quit.store(true);
				break;
			case SDL_EVENT_WINDOW_RESIZED:
				gui.ww = ev.window.data1;
				gui.wh = ev.window.data2;
				gui.resized(gui.ww, gui.wh);
				break;
			case SDL_EVENT_WINDOW_FOCUS_LOST:
				gui.focus_lost();   // leaving the window releases everything
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN: {
				const int mx = int(ev.button.x), my = int(ev.button.y);
				const bool right = ev.button.button == SDL_BUTTON_RIGHT;
				const ui::mouse_out o = gui.mouse_down(mx, my, right);
				if (o.show_menu)
					open_menu(gui, mx, my);
				down = o.panel_pressed;
				break;
			}
			case SDL_EVENT_MOUSE_BUTTON_UP:
				gui.mouse_up();
				down = false;
				break;
			case SDL_EVENT_MOUSE_MOTION:
				if (down)
					gui.mouse_drag(int(ev.motion.x), int(ev.motion.y));
				else if (cur_hand && cur_arrow)
					SDL_SetCursor(gui.hand_cursor(int(ev.motion.x), int(ev.motion.y))
					                  ? cur_hand
					                  : cur_arrow);
				break;
			case SDL_EVENT_MOUSE_WHEEL: {
				float fx, fy;
				SDL_GetMouseState(&fx, &fy);
				const int steps = int(ev.wheel.y > 0 ? 1 : ev.wheel.y < 0 ? -1 : 0);
				gui.wheel(int(fx), int(fy), steps);
				break;
			}
			case SDL_EVENT_KEY_DOWN: {
				if (ev.key.repeat)
					break;   // held-key repeats are ignored
				gui.key(sdl_key_to_shared(ev.key.key), true);
				break;
			}
			case SDL_EVENT_KEY_UP:
				gui.key(sdl_key_to_shared(ev.key.key), false);
				break;
			case SDL_EVENT_DROP_FILE:
				if (ev.drop.data) {
					play_dropped_file(ev.drop.data);
					SDL_free(const_cast<char *>(ev.drop.data));
				}
				break;
			default:
				break;
			}
		}

		ui::imshell::sdl_begin(im);
		gui.paint_main(ImGui::GetBackgroundDrawList(), im.fonts, gui.ww);
		ui::imshell::sdl_present(ren);
		const Uint64 now = SDL_GetTicks();
		if (frame_at > now)
			SDL_Delay(Uint32(frame_at - now));
	}

	// Tell the editor windows we are closing, then tear their SDL
	// resources down here: SDL_Quit below would strand them. (The shared
	// shutdown runs afterwards; on closed windows its calls do nothing.)
	pc_shutdown_all(gui.list, gui.pc, gui.fx, gui.shapes, gui.master, gui.br);
	gui.list.close();
	gui.pc.close();
	gui.fx.close();
	gui.shapes.close();
	gui.master.close();
	g_linux = nullptr;

	ui::imshell::sdl_stop(im);
	gui.imgui_ctx = nullptr;
	gui.imgui_fonts = ui::im::fonts{};
	if (cur_hand) SDL_DestroyCursor(cur_hand);
	if (cur_arrow) SDL_DestroyCursor(cur_arrow);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}

} // namespace ui
