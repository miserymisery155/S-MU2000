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

#if SMU2000_MEG_JIT
#include "x64asm.h"
#endif

namespace {

#if SMU2000_MEG_JIT

using namespace x64asm;

// meg_state::revram_encode と同じことをする。入力 eax（u32）、出力 eax（u16）。rcx と rdx を壊す
void emit_revram_encode(assembler &a)
{
	a.and32i(RAX, 0x7ffffff);
	a.xor32(RDX, RDX);                                   // s
	a.test32ri(RAX, 0x4000000);
	const size_t pos = a.jcc_fwd(0x84);
	a.xor32ri(RAX, 0x7ffffff);
	a.imm32(RDX, 1);
	a.patch(pos);
	// e は bit 11〜25 のうち一番上の 1 の位置 - 10。無ければ e = 0 で m = v（v < 0x800）
	a.mov32(RCX, RAX);
	a.shr32(RCX, 11);
	const size_t small = a.jcc_fwd(0x84);
	a.bsr32(RCX, RAX);
	a.sub32ri(RCX, 11);                                  // e - 1
	a.shr32cl(RAX);
	a.and32i(RAX, 0x7ff);
	a.add32ri(RCX, 1);
	a.shl32(RCX, 12);
	a.or32(RAX, RCX);
	a.patch(small);
	a.shl32(RDX, 11);
	a.or32(RAX, RDX);
}

// meg_state::m1_expand と同じことをする。入力 eax（下の 16bit が s16）、出力 rax（0〜0x7ffc）。rcx を壊す
void emit_m1_expand(assembler &a)
{
	a.test32ri(RAX, 0x8000);
	const size_t neg = a.jcc_fwd(0x85);
	a.mov32(RCX, RAX);
	a.shr32(RCX, 12);
	a.and32i(RCX, 7);                                    // s
	a.and32i(RAX, 0xfff);
	a.or32ri(RAX, 0x1000);
	a.cmp32ri(RCX, 5);
	const size_t done1 = a.jcc_fwd(0x84);                // s == 5
	const size_t less = a.jcc_fwd(0x82);                 // s < 5（jb）
	a.sub32ri(RCX, 5);
	a.shl32cl(RAX);
	const size_t done2 = a.jmp_fwd();
	a.patch(less);
	a.neg32(RCX);
	a.add32ri(RCX, 5);
	a.shr32cl(RAX);
	const size_t done3 = a.jmp_fwd();
	a.patch(neg);
	a.xor32(RAX, RAX);
	a.patch(done1);
	a.patch(done2);
	a.patch(done3);
}

// meg_state::revram_decode と同じことをする。入力 eax（u16）、出力 eax。rcx rdx r8 を壊す
void emit_revram_decode(assembler &a)
{
	a.mov32(R8, RAX);                                    // v
	a.mov32(RCX, RAX);
	a.shr32(RCX, 12);                                    // e
	a.and32i(RAX, 0x7ff);                                // m
	a.test32(RCX, RCX);
	const size_t e0 = a.jcc_fwd(0x84);
	a.or32ri(RAX, 0x800);
	a.sub32ri(RCX, 1);
	a.shl32cl(RAX);
	a.imm32(RDX, 0xffffffff);
	a.shl32cl(RDX);
	const size_t join = a.jmp_fwd();
	a.patch(e0);
	a.imm32(RDX, 0xffffffff);
	a.patch(join);
	a.test32ri(R8, 0x800);
	const size_t no_sign = a.jcc_fwd(0x84);
	a.xor32(RAX, RDX);
	a.patch(no_sign);
}

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

// 機械語にしたリバーブ RAM の詰め方・戻し方を、meg_state の関数と全部の入力で突き合わせる。
// 食い違った入力の数を返す（JIT が無い環境では 0）。make test の verify から呼ぶ
u64 swp30_device::meg_jit_selftest()
{
#if SMU2000_MEG_JIT
	u64 bad = 0;
	for (int which = 0; which < 3; which++) {
		assembler a;
		a.mov32(RAX, RCX);
		if (which == 0) emit_revram_encode(a); else if (which == 1) emit_revram_decode(a); else emit_m1_expand(a);
		a.ret();
		void *buf = VirtualAlloc(nullptr, a.code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!buf)
			return ~u64(0);
		std::memcpy(buf, a.code.data(), a.code.size());
		const auto fn = reinterpret_cast<u32 (*)(u32)>(buf);
		if (which == 0) {
			for (u32 v = 0; v < 0x8000000; v++)        // encode は下の 27bit しか見ない
				if ((fn(v) & 0xffff) != meg_state::revram_encode(v))
					bad++;
			for (u32 v : { 0xffffffffu, 0x80000000u, 0xf8000001u })
				if ((fn(v) & 0xffff) != meg_state::revram_encode(v))
					bad++;
		} else if (which == 1) {
			for (u32 v = 0; v < 0x10000; v++)
				if (fn(v) != meg_state::revram_decode(u16(v)))
					bad++;
		} else {
			// 呼ぶ側は loads16 で 64bit に符号拡張した値を渡す。出力は 64bit のまま使う
			const auto fn64 = reinterpret_cast<s64 (*)(s64)>(buf);
			for (s32 v = -0x8000; v < 0x8000; v++)
				if (fn64(v) != s64(meg_state::m1_expand(s16(v))))
					bad++;
		}
		VirtualFree(buf, 0, MEM_RELEASE);
	}
	return bad;
#else
	return 0;
#endif
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
		// meg_pack24 と同じく 0 の側へ切り捨てる（負なら 0x7fff を足してから右へ）
		a.mov64(RCX, RAX);
		a.sar64(RCX, 63);
		a.and32i(RCX, 0x7fff);
		a.add64(RAX, RCX);
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
			if (o.m1_expand)
				emit_m1_expand(a);                           // meg_state::m1_expand を機械語で
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
				// meg_state::revram_encode を機械語で（関数は呼ばない）。番地は r8 に取っておく
				a.mov64(R8, RAX);
				a.load32(RAX, M(o_ram_write));
				emit_revram_encode(a);
				a.store16(mem{RAM, R8, 2, 0}, RAX);
			} else {
				// meg_state::revram_decode を機械語で（関数は呼ばない）
				a.loadu16(RAX, mem{RAM, RAX, 2, 0});
				emit_revram_decode(a);
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
