// license:BSD-3-Clause
//
// パートの音色の窓（doc/pc-editor.md）。一覧の VIB・FILTER・EG・EQ の小さな絵をダブルクリックすると開く。
// 同じ絵を大きく描き（点をつまんで動かすのは一覧と同じ）、その下に値の棒を並べて数でも合わせられるようにする。

#ifndef S_MU2000_UI_PART_SHAPES_H
#define S_MU2000_UI_PART_SHAPES_H

#pragma once

#include "xg_ui.h"

namespace ui {

class part_shapes : public imgui_view
{
public:
	const wchar_t *title() const override { return L"S-MU2000 パートの音色"; }
	int default_width() const override  { return 900; }
	int default_height() const override { return 640; }
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;
};

} // namespace ui

#endif // S_MU2000_UI_PART_SHAPES_H
