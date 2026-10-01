// license:BSD-3-Clause
//
// The SDL family of the shared ImGui plumbing (ui/imgui_shell.h): main
// window, popup menu and headless shot on Linux, headless shot on macOS.
// Split out so macOS plug-ins do not gain an SDL dependency through the
// shared header.

#ifndef S_MU2000_UI_IMGUI_SHELL_SDL_H
#define S_MU2000_UI_IMGUI_SHELL_SDL_H

#pragma once

#include "ui/imgui_shell.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>


namespace ui {
namespace imshell {

struct sdl_state {
	ImGuiContext *ctx = nullptr;
	im::fonts fonts{};
};

inline bool sdl_start(sdl_state &st, SDL_Window *win, SDL_Renderer *ren)
{
	st.ctx = new_context();
	st.fonts = panel_fonts();
	if (!ImGui_ImplSDL3_InitForSDLRenderer(win, ren)) {
		ImGui::DestroyContext(st.ctx);
		st.ctx = nullptr;
		return false;
	}
	if (!ImGui_ImplSDLRenderer3_Init(ren)) {
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext(st.ctx);
		st.ctx = nullptr;
		return false;
	}
	return true;
}

inline void sdl_stop(sdl_state &st)
{
	if (!st.ctx)
		return;
	ImGui::SetCurrentContext(st.ctx);
	im::drop_user_textures();      // the panel's textures, see imgui_shell.h
	ImGui_ImplSDLRenderer3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext(st.ctx);
	st.ctx = nullptr;
	st.fonts = im::fonts{};
}

// Backend NewFrame for the caller's paint. Split from presenting so modal
// loops (the popup menu) can paint menu rows over the panel in one frame.
inline void sdl_begin(ImGuiContext *ctx)
{
	ImGui::SetCurrentContext(ctx);
	ImGui_ImplSDLRenderer3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

inline void sdl_begin(sdl_state &st) { sdl_begin(st.ctx); }

inline void sdl_present(SDL_Renderer *ren)
{
	ImGui::Render();
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
	SDL_RenderPresent(ren);
}

} // namespace imshell
} // namespace ui

#endif // S_MU2000_UI_IMGUI_SHELL_SDL_H
