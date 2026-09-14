// license:BSD-3-Clause
//
// Windows の MIDI 入力を受けて、音声スレッドへバイトで渡す。
// 書くのは MIDI のコールバック、読むのは音声スレッドの一本ずつなので、
// 添字を atomic にしておけば錠は要らない（音声スレッドで錠を待つのは禁物）。
//
// SysEx（マスターボリュームや XG のパラメータチェンジ）を受けるには、
// **入れ物をあらかじめ Windows へ渡しておく**必要がある。渡していないと
// MIM_LONGDATA は一度も来ず、SysEx だけが黙って消える。

#ifndef S_MU2000_UI_MIDI_IN_H
#define S_MU2000_UI_MIDI_IN_H

#pragma once

#include "compat/mamecompat.h"

#include <atomic>
#include <string>
#include <vector>

namespace ui {

class midi_in
{
public:
	~midi_in() { close(); }

	static std::vector<std::string> list();

	// 番号が負なら開かない（MIDI 無しで動かす）
	bool open(int device, std::string &err);
	void close();

	bool  is_open() const { return m_handle != nullptr; }
	std::string device_name() const { return m_name; }
	u64   bytes() const { return m_bytes.load(); }

	// 音声スレッドから。溜まっているバイトを 1 つずつ取り出す
	bool pop(u8 &v);

	// コールバックから。Windows が渡してきたものを積む。
	// **読んでよい位置を進めるのは、メッセージが終わったときだけ**。音声の糸に
	// 途中までのメッセージを見せない（前は 1 バイトずつ進めていたので、ブロックの
	// 境目で前半だけ読まれ、画面から送った SysEx がその途中に挟まることがあった）。
	// 積みきれなかったメッセージは丸ごと捨てる
	void on_short(u32 msg);                  // MIM_DATA
	void on_long(const u8 *p, size_t n);     // MIM_LONGDATA（SysEx の切れ端）

	// SysEx 用の入れ物。バルクダンプも来るので少し大きめに取る
	static constexpr int    SYSEX_BUFFERS = 4;
	static constexpr size_t SYSEX_SIZE    = 8192;

	bool closing() const { return m_closing.load(std::memory_order_acquire); }
	void requeue(void *hdr);   // コールバックから。入れ物を返して次を待つ

private:
	// SysEx が来ると一度に何千バイトも積まれる。輪っかは大きめに
	static constexpr size_t SIZE = 65536, MASK = SIZE - 1;
	u8 m_buf[SIZE] = {};
	std::atomic<size_t> m_read{0}, m_write{0};
	size_t m_pending = 0;             // 積みかけの書き込み位置（コールバックだけ）
	bool   m_overflow = false;        // 積みかけが溢れた
	bool   m_in_sysex = false;        // SysEx の途中か（コールバックだけ）
	void push(u8 v);
	void commit();
	void rollback();                  // 積みかけを捨てる
	std::atomic<u64>    m_bytes{0};

	void       *m_handle = nullptr;   // HMIDIIN
	std::string m_name;

	void *m_hdr[SYSEX_BUFFERS] = {};      // MIDIHDR
	u8   *m_sysex[SYSEX_BUFFERS] = {};
	std::atomic<bool> m_closing{false};
};

} // namespace ui

#endif // S_MU2000_UI_MIDI_IN_H
