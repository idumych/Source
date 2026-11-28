/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */

// The shaders of the first triangle. Both run in VMID 0 and touch no memory.
//
// Reference: AMD, "Graphics Core Next Architecture, Generation 3" Reference
// Guide, Revision 1.1, August 2016 (the GCN3 ISA, which Stoney's gfx810
// implements): chapter 12 for the VOP1/VOPC/VOP2 encodings, and the EXP
// instruction (export targets MRT0 = 0, POS0 = 12, PARAM0 = 32) in its
// export chapter.
// https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/gcn3-instruction-set-architecture.pdf

/* To regenerate an array, put its source (the lines after "asm:" below,
 * without the comment markers) in vs.s or ps.s and run
 *
 *	llvm-mc -arch=amdgcn -mcpu=gfx810 -show-encoding vs.s
 *
 * then pack the encoding bytes into little-endian dwords. To check the
 * arrays against the source, turn them back into bytes and disassemble:
 *
 *	sed -n '/kTriangleVsCode\[\] = {/,/};/p' Shaders.cpp \
 *		| grep -o '0x[0-9a-f]\{8\}' | python3 -c 'import sys
 *	for w in sys.stdin.read().split():
 *		v = int(w, 16); print(" ".join("0x%02x" % (v >> s & 0xff)
 *			for s in (0, 8, 16, 24)))' \
 *		| llvm-mc -arch=amdgcn -mcpu=gfx810 --disassemble
 *
 * (and the same with kConstantPsCode).
 */

// Register counts follow LLVM's AMDGPU rules for GFX8 (as radeonsi gets
// them from LLVM): the SGPR count includes the extra SGPRs the hardware
// places at the top of the allocation, VCC, FLAT_SCRATCH and, with XNACK
// on Carrizo/Stoney, XNACK_MASK, so 6 are added to the highest SGPR used.
// Export targets as in LLVM's SIDefines.h and Mesa's ac_shader_util:
// MRT0 = 0, POS0 = 12, PARAM0 = 32.


#include "Shaders.h"

#include "gfx_8_0_sh_mask.h"


namespace gfx8 {


// FLOAT_MODE as radeonsi sets it (V_00B028_FP_64_DENORMS = 0xc0): fp32
// denormals flushed, fp16/fp64 denormals kept.
static const uint32 kFloatMode = 0xc0;


// Vertex shader.
//
// Inputs: v0 = VertexID (VGPR_COMP_CNT 0 loads only v0; with DrawIndexAuto
// it is the vertex number plus VGT_INDX_OFFSET, so that must be 0).
// s0..s5 = x0, y0, x1, y1, x2, y2. No other SGPRs are loaded: scratch and
// streamout are off.
//
// asm:
//	v_mov_b32 v1, s0				// x = x0
//	v_mov_b32 v2, s1				// y = y0
//	v_mov_b32 v3, s2
//	v_mov_b32 v4, s3
//	v_cmp_eq_u32 vcc, 1, v0			// vertex 1?
//	v_cndmask_b32 v1, v1, v3, vcc	// then x = x1
//	v_cndmask_b32 v2, v2, v4, vcc	// and y = y1
//	v_mov_b32 v3, s4
//	v_mov_b32 v4, s5
//	v_cmp_eq_u32 vcc, 2, v0			// vertex 2?
//	v_cndmask_b32 v1, v1, v3, vcc	// then x = x2
//	v_cndmask_b32 v2, v2, v4, vcc	// and y = y2
//	v_mov_b32 v3, 0
//	v_mov_b32 v4, 1.0
//	exp param0 v3, v3, v3, v4		// PARAM0 = (0, 0, 0, 1)
//	exp pos0 v1, v2, v3, v4 done	// POS0 = (x, y, 0, 1), last position
//	s_endpgm
//
// Parameters are exported before the position, as radeonsi does; "done"
// marks the last position export.
static const uint32 kTriangleVsCode[] = {
	0x7e020200, 0x7e040201, 0x7e060202, 0x7e080203,
	0x7d940081, 0x00020701, 0x00040902, 0x7e060204,
	0x7e080205, 0x7d940082, 0x00020701, 0x00040902,
	0x7e060280, 0x7e0802f2, 0xc400020f, 0x04030303,
	0xc40008cf, 0x04030201, 0xbf810000
};

// v0..v4: 5 VGPRs. s0..s5 plus 6 reserved: 12 SGPRs.
static const uint32 kTriangleVsVgprs = 5;
static const uint32 kTriangleVsSgprs = 6 + 6;
static const uint32 kTriangleVsUserSgprs = 6;

const ShaderBinary kTriangleVs = {
	kTriangleVsCode,
	B_COUNT_OF(kTriangleVsCode),
	// VGPRS: allocated in blocks of 4, minus one (1 = 8 VGPRs)
	((kTriangleVsVgprs - 1) / 4) << SPI_SHADER_PGM_RSRC1_VS__VGPRS__SHIFT
	// SGPRS: allocated in blocks of 8, minus one (1 = 16 SGPRs)
	| ((kTriangleVsSgprs - 1) / 8) << SPI_SHADER_PGM_RSRC1_VS__SGPRS__SHIFT
	// FLOAT_MODE: rounding and denormal modes, as radeonsi
	| kFloatMode << SPI_SHADER_PGM_RSRC1_VS__FLOAT_MODE__SHIFT
	// DX10_CLAMP: clamp NaN to 0 where outputs are clamped, as radeonsi
	| SPI_SHADER_PGM_RSRC1_VS__DX10_CLAMP_MASK,
	// VGPR_COMP_CNT stays 0: only v0 (VertexID) is loaded
	// USER_SGPR: s0..s5 are loaded from SPI_SHADER_USER_DATA_VS_0..5;
	// SCRATCH_EN, SO_EN, SO_BASE*_EN, OC_LDS_EN stay 0
	kTriangleVsUserSgprs << SPI_SHADER_PGM_RSRC2_VS__USER_SGPR__SHIFT,
	kTriangleVsUserSgprs
};


// Pixel shader.
//
// Inputs: s0..s3 = r, g, b, a; the hardware then writes PRIM_MASK to s4.
// The hardware needs at least one of the PERSP_* or LINEAR_* inputs on
// (radeonsi forces PERSP_CENTER when a shader uses none), so the caller
// sets SPI_PS_INPUT_ENA = SPI_PS_INPUT_ADDR = PERSP_CENTER_ENA (bit 1) only.
// Its I and J then arrive in v0 and v1, which this shader leaves alone;
// the colour goes through v2..v5.
//
// asm:
//	v_mov_b32 v2, s0
//	v_mov_b32 v3, s1
//	v_mov_b32 v4, s2
//	v_mov_b32 v5, s3
//	exp mrt0 v2, v3, v4, v5 done vm	// MRT0 = (r, g, b, a), last export
//	s_endpgm
static const uint32 kConstantPsCode[] = {
	0x7e040200, 0x7e060201, 0x7e080202, 0x7e0a0203,
	0xc400180f, 0x05040302, 0xbf810000
};

// v0..v5: 6 VGPRs. s0..s4 plus 6 reserved: 11 SGPRs.
static const uint32 kConstantPsVgprs = 6;
static const uint32 kConstantPsSgprs = 5 + 6;
static const uint32 kConstantPsUserSgprs = 4;

const ShaderBinary kConstantPs = {
	kConstantPsCode,
	B_COUNT_OF(kConstantPsCode),
	// VGPRS: allocated in blocks of 4, minus one (1 = 8 VGPRs)
	((kConstantPsVgprs - 1) / 4) << SPI_SHADER_PGM_RSRC1_PS__VGPRS__SHIFT
	// SGPRS: allocated in blocks of 8, minus one (1 = 16 SGPRs)
	| ((kConstantPsSgprs - 1) / 8) << SPI_SHADER_PGM_RSRC1_PS__SGPRS__SHIFT
	// FLOAT_MODE: rounding and denormal modes, as radeonsi
	| kFloatMode << SPI_SHADER_PGM_RSRC1_PS__FLOAT_MODE__SHIFT
	// DX10_CLAMP: clamp NaN to 0 where outputs are clamped, as radeonsi
	| SPI_SHADER_PGM_RSRC1_PS__DX10_CLAMP_MASK,
	// USER_SGPR: s0..s3 are loaded from SPI_SHADER_USER_DATA_PS_0..3;
	// SCRATCH_EN, WAVE_CNT_EN, EXTRA_LDS_SIZE stay 0
	kConstantPsUserSgprs << SPI_SHADER_PGM_RSRC2_PS__USER_SGPR__SHIFT,
	kConstantPsUserSgprs
};


}	// namespace gfx8
