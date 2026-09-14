// license:BSD-3-Clause
//
// 標準 MIDI ファイル（SMF）を「秒 + バイト列」の並びに開く。
// render（ファイルを WAV に）と midisend（実時間で MIDI 出力へ）で共用する。

#ifndef S_MU2000_SMF_H
#define S_MU2000_SMF_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace smf {

struct event {
	double time;              // 秒
	std::vector<u8> bytes;
	// どの MIDI の口へ出すか。SMF のメタイベント `FF 21 01 pp`
	// （ポート指定）か、`FF 09`（機器名）が「A」〜「D」「Port 1」〜「Port 4」のものを
	// トラックごとに見る。無ければ 0。MU2000 は口 0 がパート 1-16、口 1 がパート 17-32
	u8 port = 0;
};

// ファイルの口をエミュの口（0 = A、1 = B）に割り当てる。実機は USB のポート 1〜4 が A〜D（64 パート）だが、
// エミュは DIN の 2 口（A・B）しか持っていない（C・D は未対応）ので、
// 3 口目以降は fold が真なら A・B に交互に重ね（口 3 → A、口 4 → B）、偽なら鳴らさない（-1）
inline int mu_port(u8 port, bool fold)
{
	if (port < 2)
		return port;
	return fold ? port % 2 : -1;
}

// format 0/1 に対応。テンポ変化は追う。SMPTE 単位には未対応
bool load(const std::string &path, std::vector<event> &out, std::string &err);

} // namespace smf

#endif // S_MU2000_SMF_H
