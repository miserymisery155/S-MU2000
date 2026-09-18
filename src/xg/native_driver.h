// license:BSD-3-Clause
//
// **firmware を走らせずに音を鳴らす口**（doc/native-engine.md の段 2）。
//
// 考え方はこう。
//
//   * 起動と、音色を選ぶところ（プログラムチェンジ・SysEx）は firmware に任せる。
//     そこは曲の頭で数回しか起きないので、重さに効かない
//   * **その音色の 1 音目も firmware に鳴らさせて、スロットに書かれた値を写し取る**
//     （voice_cal）。式が分かっていない所（フィルタ・素通しの量など）はこれで埋まる
//   * 2 音目からは CPU を止めたまま、この口が式でレジスタを作って鳴らす
//
// 鍵と強さで動くもの（音程・波形・包絡線・音量）は式で出すので、写し取りは
// 音色あたり 1 回で足りる。覚えるのは利用者の ROM から起こした値で、配らない。

#ifndef S_MU2000_XG_NATIVE_DRIVER_H
#define S_MU2000_XG_NATIVE_DRIVER_H

#pragma once

#include "xg/native_voice.h"
#include "xg/ram.h"

#include <array>
#include <functional>
#include <unordered_map>
#include <vector>

namespace xg {

class native_driver
{
public:
	static constexpr int PARTS = 64;
	static constexpr int SLOTS = 64;

	// SWP30 のマスタへ 1 レジスタ書く口
	using poke_fn = std::function<void(u32 reg, u16 value)>;

	void set_poke(poke_fn f) { m_poke = std::move(f); }
	void set_rom(const u8 *rom) { m_rom = rom; }
	// ワーク RAM（firmware が音色を選んだ結果を読む）
	void set_ram(const u8 *ram) { m_ram = ram; }

	void reset()
	{
		m_cal.clear();
		for (auto &s : m_slot)
			s = slot_use();
		m_age = 0;
	}

	// その音色の写し取りがもう有るか
	bool calibrated(u32 rec) const { return m_cal.find(rec) != m_cal.end(); }

	// firmware に鳴らさせた 1 音から写し取る
	void learn(u32 rec, std::vector<nv::voice_cal> cals)
	{
		if (!cals.empty())
			m_cal[rec] = std::move(cals);
	}

	// パートの音色の記録を、ワーク RAM から読む（firmware が入れた値）
	u32 record_of(int part) const
	{
		if (!m_ram || part < 0 || part >= PARTS)
			return 0;
		const u8 *p = m_ram + ram::part_base(part);
		const u32 r = u32(p[ram::PART_VOICE]) << 24 | u32(p[ram::PART_VOICE + 1]) << 16 |
		              u32(p[ram::PART_VOICE + 2]) << 8 | p[ram::PART_VOICE + 3];
		return (r >= 0x200ee0 && r + 16 <= 0x23cece) ? r : 0;
	}

	// 鍵を押す。写し取りが無ければ false（呼んだ側が firmware に回す）
	bool note_on(int part, int note, int vel)
	{
		const u32 rec = record_of(part);
		if (!rec || !m_rom)
			return false;
		const auto it = m_cal.find(rec);
		if (it == m_cal.end())
			return false;
		const std::vector<nv::voice_cal> &cals = it->second;

		const int nelem = nv::element_count(m_rom, rec);
		u64 keymask = 0;
		int used = 0;
		for (int k = 0; k < nelem; k++) {
			const u8 *el = nv::element(m_rom, rec, k);
			if (!nv::element_active(el, note, vel))
				continue;
			// 波形の番地で、写し取ったスロットと結び付ける
			const u8 *we = nv::wave_entry(m_rom, nv::wave_set(el), note);
			const nv::voice_cal *c = we ? nv::match_cal(cals, nv::read_wave(we).format_addr) : nullptr;
			if (!c && size_t(used) < cals.size())
				c = &cals[used];
			used++;
			const int slot = take_slot(part, note);
			if (slot < 0)
				break;
			const int att = nv::volume_att(m_rom, el, c ? c->base_level : 64, note, vel);
			write_slot(slot, nv::build_note(m_rom, el, note, att, c));
			m_slot[slot].elem = el;
			m_slot[slot].att = att;
			keymask |= u64(1) << slot;
		}
		if (!keymask)
			return false;
		key_on(keymask);
		return true;
	}

	// 鍵を離す。鳴っていなければ false
	bool note_off(int part, int note)
	{
		bool any = false;
		for (int i = 0; i < SLOTS; i++) {
			slot_use &s = m_slot[i];
			if (!s.on || s.part != part || s.note != note)
				continue;
			if (s.elem)
				m_poke(u32(i) * 64 + 9, nv::release_reg(m_rom, s.elem, note, s.att));
			s.on = false;
			any = true;
		}
		return any;
	}

	// そのパートの音を全部止める
	void all_off(int part)
	{
		for (int i = 0; i < SLOTS; i++)
			if (m_slot[i].on && m_slot[i].part == part)
				note_off(part, m_slot[i].note);
	}

private:
	struct slot_use {
		bool on = false;
		int part = -1, note = -1, att = 0;
		const u8 *elem = nullptr;
		u64 age = 0;
	};

	// 空きスロットを取る。無ければ一番古い声を止めて使う。
	// **上から**取る。firmware は下から使うので、写し取りのために firmware が
	// 鳴らしている音とぶつかりにくい
	int take_slot(int part, int note)
	{
		int oldest = -1;
		u64 oldest_age = ~u64(0);
		for (int n2 = 0; n2 < SLOTS; n2++) {
			const int i = SLOTS - 1 - n2;
			if (!m_slot[i].on) {
				m_slot[i] = slot_use{ true, part, note, 0, nullptr, ++m_age };
				return i;
			}
			if (m_slot[i].age < oldest_age) {
				oldest_age = m_slot[i].age;
				oldest = i;
			}
		}
		if (oldest < 0)
			return -1;
		m_slot[oldest] = slot_use{ true, part, note, 0, nullptr, ++m_age };
		return oldest;
	}

	void write_slot(int slot, const nv::slot_regs &r)
	{
		for (int i = 0; i < 0x40; i++)
			if (r.write & (u64(1) << i))
				m_poke(u32(slot) * 64 + u32(i), r.v[i]);
	}

	void key_on(u64 mask)
	{
		static const u32 MASK_REG[4] = { 0x1cf, 0x1ce, 0x18f, 0x18e };
		for (int i = 0; i < 4; i++)
			m_poke(MASK_REG[i], u16((mask >> (i * 16)) & 0xffff));
		m_poke(0x20e, 1);
	}

	poke_fn m_poke;
	const u8 *m_rom = nullptr;
	const u8 *m_ram = nullptr;
	std::unordered_map<u32, std::vector<nv::voice_cal>> m_cal;
	std::array<slot_use, SLOTS> m_slot;
	u64 m_age = 0;
};

} // namespace xg

#endif // S_MU2000_XG_NATIVE_DRIVER_H
