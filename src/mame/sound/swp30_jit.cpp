// license:BSD-3-Clause
//
// S-MU2000: MEG のプログラムをその場で x86-64 の機械語にする（JIT）。
//
// firmware が MEG に書いたプログラムを、プログラムか番地の割り当てが変わるたびに訳し直す。
// 訳した機械語は meg_state::run_program() と**ビット単位で同じこと**をする:
//
//   * 命令ごとの判定（ALU の種類・読み先・書き先・メモリ操作）は訳すときに決める
//   * 3 命令遅れの書き込み・2 命令遅れのメモリポートと t の値は、遅れの輪のどの枠かが
//     訳すときに決まるので、反映する命令の位置に直に置く。サンプルを跨ぐ分
//     （頭の 3 命令が読む枠と、終わりの 3 命令が書く枠）だけ輪そのものを使う
//   * 定数・番地表・LFO は firmware が動かしている最中にも書くので、実行時に読む
//   * 乱数（ディザ）は同じ順に同じ回数だけ引く
//
// 分岐（bit 0x3f）を含むプログラムは訳さずに run_program() に任せる（LO-FI と DYNA 系だけ）。
// 訳した物はどこにも保存しない（firmware 由来のものを配らない。実行時に作って捨てる）。
//
// Windows の x86-64 だけ。ほかでは build() が false を返し、今までどおり解釈実行する。

#include "swp30.h"

#if defined(_WIN32) && defined(__x86_64__)
#define SMU2000_MEG_JIT 1
#include <windows.h>
#else
#define SMU2000_MEG_JIT 0
#endif

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#if SMU2000_MEG_JIT

// ---- 小さな x86-64 の組み立て器 ------------------------------------------------------

enum : u8 { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, NOREG = 0xff };

struct mem {
	u8 base;
	u8 index = NOREG;
	u8 scale = 1;
	s32 disp = 0;
};

class assembler
{
public:
	std::vector<u8> code;

	void byte(u8 b) { code.push_back(b); }
	void d32(u32 v) { for (int i = 0; i < 4; i++) byte(u8(v >> (8 * i))); }
	void d64(u64 v) { for (int i = 0; i < 8; i++) byte(u8(v >> (8 * i))); }

	// 命令の本体。prefix（0 なら無し）、REX.W、命令バイト列、ModRM の reg 欄、相手
	void rr(u8 prefix, bool w, std::initializer_list<u8> opc, u8 reg, u8 rm)
	{
		if (prefix) byte(prefix);
		u8 rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((rm >> 3) & 1);
		if (rex != 0x40) byte(rex);
		for (u8 o : opc) byte(o);
		byte(u8(0xc0 | ((reg & 7) << 3) | (rm & 7)));
	}
	void rm(u8 prefix, bool w, std::initializer_list<u8> opc, u8 reg, const mem &m)
	{
		if (prefix) byte(prefix);
		const u8 x = m.index == NOREG ? 0 : (m.index >> 3) & 1;
		u8 rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | x << 1 | ((m.base >> 3) & 1);
		if (rex != 0x40) byte(rex);
		for (u8 o : opc) byte(o);
		// いつも disp32 の形にする（長さが決まっていて楽）
		if (m.index == NOREG && (m.base & 7) != RSP) {
			byte(u8(0x80 | ((reg & 7) << 3) | (m.base & 7)));
		} else {
			byte(u8(0x80 | ((reg & 7) << 3) | 4));
			const u8 ss = m.scale == 1 ? 0 : m.scale == 2 ? 1 : m.scale == 4 ? 2 : 3;
			const u8 idx = m.index == NOREG ? 4 : (m.index & 7);
			byte(u8((ss << 6) | (idx << 3) | (m.base & 7)));
		}
		d32(u32(m.disp));
	}

	void mov64(u8 d, u8 s)            { rr(0, true, {0x8b}, d, s); }
	void load64(u8 d, const mem &m)   { rm(0, true, {0x8b}, d, m); }
	void store64(const mem &m, u8 s)  { rm(0, true, {0x89}, s, m); }
	void load32(u8 d, const mem &m)   { rm(0, false, {0x8b}, d, m); }
	void store32(const mem &m, u8 s)  { rm(0, false, {0x89}, s, m); }
	void loads32(u8 d, const mem &m)  { rm(0, true, {0x63}, d, m); }          // movsxd
	void loads16(u8 d, const mem &m)  { rm(0, true, {0x0f, 0xbf}, d, m); }    // movsx r64, m16
	void loadu16(u8 d, const mem &m)  { rm(0, false, {0x0f, 0xb7}, d, m); }   // movzx r32, m16
	void loadu8(u8 d, const mem &m)   { rm(0, false, {0x0f, 0xb6}, d, m); }   // movzx r32, m8
	void store16(const mem &m, u8 s)  { rm(0x66, false, {0x89}, s, m); }
	void store8i(const mem &m, u8 v)  { rm(0, false, {0xc6}, 0, m); byte(v); }
	void imm64(u8 d, u64 v)
	{
		byte(u8(0x48 | ((d >> 3) & 1)));
		byte(u8(0xb8 | (d & 7)));
		d64(v);
	}
	void imm32(u8 d, u32 v)
	{
		if (d >= 8) byte(0x41);
		byte(u8(0xb8 | (d & 7)));
		d32(v);
	}
	void add64(u8 d, u8 s) { rr(0, true, {0x01}, s, d); }
	void sub64(u8 d, u8 s) { rr(0, true, {0x29}, s, d); }
	void and64(u8 d, u8 s) { rr(0, true, {0x21}, s, d); }
	void cmp64(u8 a, u8 b) { rr(0, true, {0x39}, b, a); }          // cmp a, b
	void test64(u8 a, u8 b) { rr(0, true, {0x85}, b, a); }
	void test32(u8 a, u8 b) { rr(0, false, {0x85}, b, a); }
	void xor32(u8 d, u8 s) { rr(0, false, {0x31}, s, d); }
	void add32(u8 d, u8 s) { rr(0, false, {0x01}, s, d); }
	void sub32(u8 d, u8 s) { rr(0, false, {0x29}, s, d); }
	void imul64(u8 d, u8 s) { rr(0, true, {0x0f, 0xaf}, d, s); }
	void imul32i(u8 d, u8 s, u32 v) { rr(0, false, {0x69}, d, s); d32(v); }
	void add32i(u8 d, u32 v) { rr(0, false, {0x81}, 0, d); d32(v); }
	void and32i(u8 d, u32 v) { rr(0, false, {0x81}, 4, d); d32(v); }
	void shl64(u8 d, u8 n) { rr(0, true, {0xc1}, 4, d); byte(n); }
	void sar64(u8 d, u8 n) { rr(0, true, {0xc1}, 7, d); byte(n); }
	void shl32(u8 d, u8 n) { rr(0, false, {0xc1}, 4, d); byte(n); }
	void sar32(u8 d, u8 n) { rr(0, false, {0xc1}, 7, d); byte(n); }
	void rol32(u8 d, u8 n) { rr(0, false, {0xc1}, 0, d); byte(n); }
	void neg64(u8 d) { rr(0, true, {0xf7}, 3, d); }
	void cmovl64(u8 d, u8 s) { rr(0, true, {0x0f, 0x4c}, d, s); }
	void cmovg64(u8 d, u8 s) { rr(0, true, {0x0f, 0x4f}, d, s); }
	void cmovs64(u8 d, u8 s) { rr(0, true, {0x0f, 0x48}, d, s); }
	void setl_mem(const mem &m) { rm(0, false, {0x0f, 0x9c}, 0, m); }
	void sete_mem(const mem &m) { rm(0, false, {0x0f, 0x94}, 0, m); }
	void call_reg(u8 r) { rr(0, false, {0xff}, 2, r); }
	void push(u8 r) { if (r >= 8) byte(0x41); byte(u8(0x50 | (r & 7))); }
	void pop(u8 r)  { if (r >= 8) byte(0x41); byte(u8(0x58 | (r & 7))); }
	void subrsp(u32 v) { rr(0, true, {0x81}, 5, RSP); d32(v); }
	void addrsp(u32 v) { rr(0, true, {0x81}, 0, RSP); d32(v); }
	void ret() { byte(0xc3); }
	// jz で先へ飛ぶ。飛び先は後で patch() で埋める
	size_t jz_fwd() { byte(0x0f); byte(0x84); d32(0); return code.size(); }
	void patch(size_t at) { const u32 rel = u32(code.size() - at); std::memcpy(&code[at - 4], &rel, 4); }
	void call_abs(void *fn) { imm64(RAX, u64(uintptr_t(fn))); call_reg(RAX); }
};

#endif

} // namespace


struct swp30_device::meg_jit {
	using fn_t = void (*)(meg_state *, swp30_device *, u16 *);
	fn_t fn = nullptr;
	void *buf = nullptr;
	size_t buf_size = 0;
	u32 d3 = 0, d2 = 0;

	~meg_jit()
	{
#if SMU2000_MEG_JIT
		if (buf)
			VirtualFree(buf, 0, MEM_RELEASE);
#endif
	}

#if SMU2000_MEG_JIT
	static u32 call_lfo(meg_state *ms, u32 lfo) { return ms->get_lfo(int(lfo)); }
	static s64 call_expand(s64 v) { return ms_expand(s16(v)); }
	static s64 ms_expand(s16 v) { return meg_state::m1_expand(v); }
	static u32 call_encode(u32 v) { return meg_state::revram_encode(v); }
	static u32 call_decode(u32 v) { return meg_state::revram_decode(u16(v)); }
#endif

	bool build(meg_state &ms, const meg_state::op *ops, swp30_device &swp);
};

void swp30_device::meg_jit_delete(meg_jit *j)
{
	delete j;
}

bool swp30_device::meg_jit_enabled()
{
#if SMU2000_MEG_JIT
	static const bool on = [] {
		const char *e = std::getenv("SMU2000_MEG_JIT");
		return !(e && e[0] == '0');
	}();
	return on;
#else
	return false;
#endif
}

void swp30_device::meg_jit_rebuild()
{
	if (!meg_jit_enabled()) {
		m_jit.reset();
		return;
	}
	if (!m_jit)
		m_jit.reset(new meg_jit);
	if (!m_jit->build(*m_meg, m_meg_ops.data(), *this))
		m_jit->fn = nullptr;
}

bool swp30_device::meg_jit_run()
{
	meg_jit *j = m_jit.get();
	if (!j || !j->fn || j->d3 != m_meg->m_delay_3 || j->d2 != m_meg->m_delay_2)
		return false;
	j->fn(m_meg, this, m_reverb_ram.data());
	m_meg->m_pc = 0;
	m_meg->m_icount -= 0x180;
	return true;
}

#if !SMU2000_MEG_JIT

bool swp30_device::meg_jit::build(meg_state &, const meg_state::op *, swp30_device &)
{
	return false;
}

#else

bool swp30_device::meg_jit::build(meg_state &ms, const meg_state::op *ops, swp30_device &swp)
{
	fn = nullptr;
	for (u32 pc = 0; pc != 0x180; pc++)
		if (ops[pc].jump)
			return false;
	if (swp.m_reverb_ram.size() < 0x40000)
		return false;

	d3 = ms.m_delay_3;
	d2 = ms.m_delay_2;
	const auto slot3 = [&](u32 k) { return (d3 + k) % 3; };
	const auto slot2 = [&](u32 k) { return (d2 + k) % 2; };

	// 要素の位置（meg_state と swp30_device の中）
	const auto off = [](const void *base, const void *field) { return s32(intptr_t(field) - intptr_t(base)); };
	const s32 o_m        = off(&ms, ms.m_m.data());
	const s32 o_r        = off(&ms, ms.m_r.data());
	const s32 o_t        = off(&ms, ms.m_t.data());
	const s32 o_p        = off(&ms, &ms.m_p);
	const s32 o_const    = off(&ms, ms.m_const.data());
	const s32 o_offset   = off(&ms, ms.m_offset.data());
	const s32 o_mw_value = off(&ms, ms.m_mw_value.data());
	const s32 o_mw_reg   = off(&ms, ms.m_mw_reg.data());
	const s32 o_rw_value = off(&ms, ms.m_rw_value.data());
	const s32 o_rw_reg   = off(&ms, ms.m_rw_reg.data());
	const s32 o_ix_value = off(&ms, ms.m_index_value.data());
	const s32 o_ix_act   = off(&ms, ms.m_index_active.data());
	const s32 o_memw_val = off(&ms, ms.m_memw_value.data());
	const s32 o_memr_val = off(&ms, ms.m_memr_value.data());
	const s32 o_t_value  = off(&ms, ms.m_t_value.data());
	const s32 o_memw_act = off(&ms, ms.m_memw_active.data());
	const s32 o_memr_act = off(&ms, ms.m_memr_active.data());
	const s32 o_ram_read = off(&ms, &ms.m_ram_read);
	const s32 o_ram_write = off(&ms, &ms.m_ram_write);
	const s32 o_ram_index = off(&ms, &ms.m_ram_index);
	const s32 o_sample   = off(&ms, &ms.m_sample_counter);
	const s32 o_seed     = off(&swp, &swp.m_rand_seed);
	const s32 o_flag_n   = off(&swp, &swp.m_meg_flag_n);
	const s32 o_flag_z   = off(&swp, &swp.m_meg_flag_z);

	if (sizeof(ms.m_mw_reg[0]) != 1 || sizeof(ms.m_index_active[0]) != 1 || sizeof(ms.m_memw_active[0]) != 1 ||
	    sizeof(swp.m_meg_flag_n) != 1 || sizeof(ms.m_t_value[0]) != 2 || sizeof(ms.m_const[0]) != 2 ||
	    sizeof(ms.m_offset[0]) != 2 || sizeof(ms.m_m[0]) != 4 || sizeof(ms.m_r[0]) != 4)
		return false;

	// t の値（2 命令遅れ）を書いておく必要がある命令: 2 つ後に t を p から書く命令があるか、終わりの 2 つ
	bool need_tval[0x180] = {};
	for (u32 k = 0; k != 0x180; k++) {
		if (k >= 0x17e)
			need_tval[k] = true;
		if (k + 2 < 0x180 && ops[k + 2].t_write && ops[k + 2].t_from_p)
			need_tval[k] = true;
	}

	assembler a;
	const u8 MS = RBX, SWP = R12, P = R13, SC = R14, RAM = R15;
	const auto M = [&](s32 disp) { return mem{MS, NOREG, 1, disp}; };

	// 入口（Windows x64: rcx = ms, rdx = swp, r8 = リバーブ RAM）
	a.push(RBX); a.push(R12); a.push(R13); a.push(R14); a.push(R15);
	a.subrsp(48);                                    // 呼ぶ先の影 32 + 自分の置き場 16。rsp は 16 の倍数
	a.mov64(MS, RCX);
	a.mov64(SWP, RDX);
	a.mov64(RAM, R8);
	a.load64(P, M(o_p));
	a.load32(SC, M(o_sample));

	// p を 24bit に詰める（meg_pack24）。入力 rax、出力 eax
	const auto pack24 = [&]() {
		a.sar64(RAX, 15);
		a.imm64(RCX, u64(s64(-0x800000)));
		a.cmp64(RAX, RCX);
		a.cmovl64(RAX, RCX);
		a.imm64(RCX, 0x7fffff);
		a.cmp64(RAX, RCX);
		a.cmovg64(RAX, RCX);
	};
	// 乱数を 1 つ引く（swp30_device::rand）。出力 eax
	const auto rnd = [&]() {
		a.load32(RAX, mem{SWP, NOREG, 1, o_seed});
		a.imul32i(RAX, RAX, 1664525);
		a.add32i(RAX, 1013904223);
		a.store32(mem{SWP, NOREG, 1, o_seed}, RAX);
		a.rol32(RAX, 16);
	};
	// p に雑音を足して詰める（dm の 6 番、dr の p）。出力 eax
	const auto p_packed = [&](bool noise) {
		if (noise) {
			rnd();
			a.and32i(RAX, 0x07e0);
			a.mov64(RDX, RAX);
			a.mov64(RAX, P);
			a.add64(RAX, RDX);
		} else
			a.mov64(RAX, P);
		pack24();
	};

	for (u32 k = 0; k != 0x180; k++) {
		const meg_state::op &o = ops[k];

		// ---- 反映（遅れて入る書き込み）----
		if (k < 3) {
			const u32 s = slot3(k);
			// m
			a.loadu8(RAX, M(o_mw_reg + s));
			a.test32(RAX, RAX);
			size_t j1 = a.jz_fwd();
			a.load32(RCX, M(o_mw_value + 4 * s));
			a.store32(mem{MS, RAX, 4, o_m}, RCX);
			a.patch(j1);
			// r
			a.loadu8(RAX, M(o_rw_reg + s));
			a.test32(RAX, RAX);
			size_t j2 = a.jz_fwd();
			a.load32(RCX, M(o_rw_value + 4 * s));
			a.store32(mem{MS, RAX, 4, o_r}, RCX);
			a.patch(j2);
			// index
			a.loadu8(RAX, M(o_ix_act + s));
			a.test32(RAX, RAX);
			size_t j3 = a.jz_fwd();
			a.load32(RCX, M(o_ix_value + 4 * s));
			a.store32(M(o_ram_index), RCX);
			a.patch(j3);
		} else {
			const meg_state::op &w = ops[k - 3];
			const u32 s = slot3(k);
			if (w.dm) {
				a.load32(RCX, M(o_mw_value + 4 * s));
				a.store32(M(o_m + 4 * w.dm), RCX);
			}
			if (w.dr) {
				a.load32(RCX, M(o_rw_value + 4 * s));
				a.store32(M(o_r + 4 * w.dr), RCX);
			}
			if (w.index) {
				a.load32(RCX, M(o_ix_value + 4 * s));
				a.store32(M(o_ram_index), RCX);
			}
		}
		if (k < 2) {
			const u32 s = slot2(k);
			a.loadu8(RAX, M(o_memw_act + s));
			a.test32(RAX, RAX);
			size_t j1 = a.jz_fwd();
			a.load32(RCX, M(o_memw_val + 4 * s));
			a.store32(M(o_ram_write), RCX);
			a.store8i(M(o_memw_act + s), 0);
			a.patch(j1);
			a.loadu8(RAX, M(o_memr_act + s));
			a.test32(RAX, RAX);
			size_t j2 = a.jz_fwd();
			a.load32(RCX, M(o_memr_val + 4 * s));
			a.store32(M(o_ram_read), RCX);
			a.store8i(M(o_memr_act + s), 0);
			a.patch(j2);
		} else {
			const meg_state::op &w = ops[k - 2];
			const u32 s = slot2(k);
			if (w.memw) {
				a.load32(RCX, M(o_memw_val + 4 * s));
				a.store32(M(o_ram_write), RCX);
			}
			if (w.memop == 2 || w.memop == 3) {
				a.load32(RCX, M(o_memr_val + 4 * s));
				a.store32(M(o_ram_read), RCX);
			}
		}

		// ---- ALU ----
		if (o.alu) {
			if (o.m1_from_t)
				a.loads16(RAX, M(o_t + 2 * o.t));
			else
				a.loads16(RAX, M(o_const + 2 * s32(k)));
			if (o.m1_expand) {
				a.mov64(RCX, RAX);
				a.call_abs(reinterpret_cast<void *>(&meg_jit::call_expand));
			}
			switch (o.mmode) {
			case 1:
				a.shl64(RAX, 8 + 15);
				break;
			case 2:
				a.loads32(RCX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.imul64(RAX, RCX);
				break;
			default:
				a.loads32(RAX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.shl64(RAX, 15);
				break;
			}
			switch (o.asel) {
			case 0: a.mov64(RCX, P); break;
			case 1: a.loads32(RCX, M(o_r + 4 * o.sr)); a.shl64(RCX, 15); break;
			case 2: a.loads32(RCX, M(o_m + 4 * o.sm)); a.shl64(RCX, 15); break;
			case 3: a.mov64(RCX, P); a.sar64(RCX, 15); break;
			default: a.xor32(RCX, RCX); break;
			}
			switch (o.rop) {
			case 0: a.add64(RAX, RCX); break;
			case 1: a.sub64(RAX, RCX); break;
			case 2:
				a.mov64(RDX, RCX);
				a.neg64(RDX);
				a.cmovs64(RDX, RCX);
				a.add64(RAX, RDX);
				break;
			default: a.and64(RAX, RCX); break;
			}
			if (o.shift)
				a.shl64(RAX, o.shift);
			a.shl64(RAX, 22);
			a.sar64(RAX, 22);
			switch (o.clamp) {
			case 0: break;
			case 1:
				a.imm64(RCX, u64(s64(-0x4000000000)));
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x3fffffffff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				break;
			case 2:
				a.xor32(RCX, RCX);
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x3fffffffff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				break;
			default:
				a.mov64(RDX, RAX);
				a.neg64(RDX);
				a.cmovs64(RDX, RAX);
				a.mov64(RAX, RDX);
				a.imm64(RCX, 0x3fffffffff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				break;
			}
			a.mov64(P, RAX);
			if (o.latch) {
				a.test64(P, P);
				a.setl_mem(mem{SWP, NOREG, 1, o_flag_n});
				a.test64(P, P);
				a.sete_mem(mem{SWP, NOREG, 1, o_flag_z});
			}
		}

		// ---- dm ----
		if (o.dm) {
			switch (o.dm_src) {
			case 0: case 1: case 2: case 3:
				a.mov64(RCX, MS);
				a.imm32(RDX, o.lfo);
				a.call_abs(reinterpret_cast<void *>(&meg_jit::call_lfo));
				break;
			case 4:
				a.load32(RAX, M(o_ram_read));
				break;
			case 5:
				rnd();
				a.shl32(RAX, 8);
				a.sar32(RAX, 8);
				break;
			case 6:
				p_packed(!o.no_noise);
				break;
			default:
				a.load32(RAX, M(o_m + 4 * o.sm));
				break;
			}
			a.store32(M(o_mw_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d)
			a.store8i(M(o_mw_reg + slot3(k)), o.dm);

		// ---- dr ----
		if (o.dr) {
			if (o.dr_from_r)
				a.load32(RAX, M(o_r + 4 * o.sr));
			else
				p_packed(!o.no_noise);
			a.store32(M(o_rw_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d)
			a.store8i(M(o_rw_reg + slot3(k)), o.dr);

		// ---- メモリへの書き値 ----
		if (o.memw) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15);
			a.store32(M(o_memw_val + 4 * slot2(k)), RAX);
		}
		if (k >= 0x17e)
			a.store8i(M(o_memw_act + slot2(k)), o.memw ? 1 : 0);

		// ---- index ----
		if (o.index) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15 + 8);
			a.store32(M(o_ix_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d)
			a.store8i(M(o_ix_act + slot3(k)), o.index ? 1 : 0);

		// ---- t ----
		if (o.t_write) {
			if (o.t_from_p)
				a.loadu16(RAX, M(o_t_value + 2 * slot2(k)));
			else
				a.loadu16(RAX, M(o_const + 2 * s32(k)));
			a.store16(M(o_t + 2 * o.t), RAX);
		}
		if (need_tval[k]) {
			a.mov64(RAX, P);
			if (o.index) {
				a.sar64(RAX, 8);
				a.and32i(RAX, 0x7fff);
			} else {
				a.sar64(RAX, 15 + 8);
				a.imm64(RCX, u64(s64(-0x8000)));
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x7fff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
			}
			a.store16(M(o_t_value + 2 * slot2(k)), RAX);
		}

		// ---- メモリ操作 ----
		if (o.memop) {
			a.loadu16(RAX, M(o_offset + 2 * s32(o.offset_index)));
			if (o.mem_use_index) {
				a.load32(RCX, M(o_ram_index));
				a.add32(RAX, RCX);
			}
			a.sub32(RAX, SC);
			if (o.memop == 3)
				a.add32i(RAX, 1);
			a.and32i(RAX, o.addr_mask);
			a.add32i(RAX, o.addr_base);
			a.and32i(RAX, 0x3ffff);
			if (o.memop == 1) {
				a.store64(mem{RSP, NOREG, 1, 40}, RAX);
				a.load32(RCX, M(o_ram_write));
				a.call_abs(reinterpret_cast<void *>(&meg_jit::call_encode));
				a.load64(RCX, mem{RSP, NOREG, 1, 40});
				a.store16(mem{RAM, RCX, 2, 0}, RAX);
			} else {
				a.loadu16(RCX, mem{RAM, RAX, 2, 0});
				a.call_abs(reinterpret_cast<void *>(&meg_jit::call_decode));
				a.store32(M(o_memr_val + 4 * slot2(k)), RAX);
			}
		}
		if (k >= 0x17e)
			a.store8i(M(o_memr_act + slot2(k)), (o.memop == 2 || o.memop == 3) ? 1 : 0);
	}

	// 出口
	a.store64(M(o_p), P);
	a.addrsp(48);
	a.pop(R15); a.pop(R14); a.pop(R13); a.pop(R12); a.pop(RBX);
	a.ret();

	if (a.code.size() > buf_size) {
		if (buf)
			VirtualFree(buf, 0, MEM_RELEASE);
		buf_size = (a.code.size() + 0xffff) & ~size_t(0xffff);
		buf = VirtualAlloc(nullptr, buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!buf) {
			buf_size = 0;
			return false;
		}
	}
	DWORD old;
	VirtualProtect(buf, buf_size, PAGE_READWRITE, &old);
	std::memcpy(buf, a.code.data(), a.code.size());
	VirtualProtect(buf, buf_size, PAGE_EXECUTE_READ, &old);
	FlushInstructionCache(GetCurrentProcess(), buf, a.code.size());
	fn = reinterpret_cast<fn_t>(buf);
	return true;
}

#endif
