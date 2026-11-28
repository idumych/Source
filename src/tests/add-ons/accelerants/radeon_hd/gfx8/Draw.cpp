/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */

// The first draw of the 3D pipeline: one triangle, straight into the visible
// frame buffer, with a vertex and a pixel shader from Shaders.cpp.
//
// Coordinates: the vertex shader exports pre-transformed window coordinates
// (pixels, y down, w = 1). PA_CL_VTE_CNTL bypasses the viewport transform
// (VTX_XY_FMT, VTX_Z_FMT, VTX_W0_FMT set, all scales and offsets off) and
// PA_CL_CLIP_CNTL disables clipping, so the scissors alone bound the draw.
//
// Every register is set here, not left to the clear state, and logged. The
// values follow Mesa's radeonsi GFX8 paths (src/gallium/drivers/radeonsi,
// MIT) and Linux's amdgpu (drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c, MIT); the
// comments name the function a value comes from where it is not obvious.
//
// AMD has published no register reference for GFX8 (Volcanic Islands); the
// nearest is "Radeon Sea Islands 3D/Compute Register Reference Guide",
// September 2012 (https://www.x.org/docs/AMD/old/CIK_3D_registers_v2.pdf),
// which describes most of the PA, SPI, CB and DB registers used here as they
// were one generation earlier. The offsets and fields come from AMD's MIT
// register headers in amd/ (Linux v6.12).


#include "Steps.h"

#include <string.h>

#include "Diagnose.h"
#include "RingInternal.h"
#include "Shaders.h"

#include "vid.h"
#include "gfx_8_0_d.h"
#include "gfx_8_0_sh_mask.h"
#include "gfx_8_0_enum.h"


namespace gfx8 {


// RB+ (Stoney has it) registers our gfx_8_0 headers lack; offsets and
// fields from Mesa's amd/registers (R_028754, R_028758, R_02875C, R_028760).
static const uint32 mmSX_PS_DOWNCONVERT = 0xa1d5;
static const uint32 mmSX_BLEND_OPT_EPSILON = 0xa1d6;
static const uint32 mmSX_BLEND_OPT_CONTROL = 0xa1d7;
static const uint32 mmSX_MRT0_BLEND_OPT = 0xa1d8;
static const uint32 kSxOptCombBlendDisabled = 7;
	// V_028760_OPT_COMB_BLEND_DISABLED
static const uint32 kSxMrtBlendOptColorCombFcnShift = 8;
static const uint32 kSxMrtBlendOptAlphaCombFcnShift = 24;

// The tile mode index of ARRAY_LINEAR_ALIGNED, and the GB_TILE_MODE8 value
// Linux's gfx_v8_0_tiling_mode_table_init() programs for CHIP_STONEY.
static const uint32 kLinearAlignedTileIndex = 8;
static const uint32 kStoneyTileMode8
	= (ARRAY_LINEAR_ALIGNED << GB_TILE_MODE0__ARRAY_MODE__SHIFT)
		| (ADDR_SURF_P2 << GB_TILE_MODE0__PIPE_CONFIG__SHIFT);

static const uint32 kShaderAlignment = 256;
static const uint32 kMaxDrawDwords = 1024;


static inline uint32
float_bits(float value)
{
	uint32 bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}


static void
set_context_reg(Ring& ring, const char* name, uint32 reg, uint32 value)
{
	ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 1));
	ring_write(ring, reg - PACKET3_SET_CONTEXT_REG_START);
	ring_write(ring, value);
	Log("gfx: draw: context %s (0x%04" B_PRIx32 ") = 0x%08" B_PRIx32, name,
		reg, value);
}


static void
set_sh_reg(Ring& ring, const char* name, uint32 reg, uint32 value)
{
	ring_write(ring, PACKET3(PACKET3_SET_SH_REG, 1));
	ring_write(ring, reg - PACKET3_SET_SH_REG_START);
	ring_write(ring, value);
	Log("gfx: draw: sh %s (0x%04" B_PRIx32 ") = 0x%08" B_PRIx32, name, reg,
		value);
}


static void
set_uconfig_reg(Ring& ring, const char* name, uint32 reg, uint32 value)
{
	ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	ring_write(ring, reg - PACKET3_SET_UCONFIG_REG_START);
	ring_write(ring, value);
	Log("gfx: draw: uconfig %s (0x%04" B_PRIx32 ") = 0x%08" B_PRIx32, name,
		reg, value);
}

#define SET_CONTEXT(reg, value) set_context_reg(ring, #reg, mm##reg, value)
#define SET_SH(reg, value) set_sh_reg(ring, #reg, mm##reg, value)
#define SET_UCONFIG(reg, value) set_uconfig_reg(ring, #reg, mm##reg, value)


static inline uint32
read_pixel(Gpu& gpu, uint32 x, uint32 y)
{
	return *(volatile uint32*)((uint8*)gpu.FrameBuffer()
		+ (size_t)y * gpu.BytesPerRow() + x * 4);
}


// Copies a shader to GPU memory at `offset`; returns its MC address.
static uint64
upload_shader(const GpuMemory& memory, uint32 offset,
	const ShaderBinary& shader)
{
	volatile uint32* code = (volatile uint32*)((uint8*)memory.cpu + offset);
	for (uint32 i = 0; i < shader.dwords; i++)
		code[i] = shader.code[i];
	return memory.gpu + offset;
}


static status_t
fail(Gpu& gpu, const char* why, status_t status)
{
	Log("gfx: draw: %s", why);
	DumpGfxHang(gpu, why);
	return status;
}


status_t
TestTriangle(Gpu& gpu, Ring& ring, uint32 x, uint32 y, uint32 size,
	uint32 color)
{
	uint32 width = gpu.Width();
	uint32 height = gpu.Height();
	uint32 bytesPerRow = gpu.BytesPerRow();
	uint32 pitch = bytesPerRow / 4;
	uint64 frameBuffer = gpu.FrameBufferGpu();

	if (gpu.BitsPerPixel() != 32) {
		Log("gfx: draw: %" B_PRIu32 " bits per pixel is not supported",
			gpu.BitsPerPixel());
		return B_NOT_SUPPORTED;
	}
	if (size < 16 || x >= width || y >= height || size > width - x
		|| size > height - y) {
		Log("gfx: draw: bad square %" B_PRIu32 " at (%" B_PRIu32 ", %"
			B_PRIu32 ")", size, x, y);
		return B_BAD_VALUE;
	}
	// LINEAR_ALIGNED wants a pitch of a multiple of 64 pixels and a 256 byte
	// aligned base (CB_COLOR0_BASE takes address >> 8).
	if (bytesPerRow % 4 != 0 || pitch % 64 != 0 || (frameBuffer & 0xff) != 0
		|| (uint64)pitch * height % 64 != 0) {
		Log("gfx: draw: frame buffer at MC 0x%" B_PRIx64 ", %" B_PRIu32
			" bytes per row, is not usable as a linear aligned colour buffer",
			frameBuffer, bytesPerRow);
		return B_NOT_SUPPORTED;
	}
	if (kTriangleVs.userSgprs != 6 || kConstantPs.userSgprs != 4) {
		Log("gfx: draw: the shaders take %" B_PRIu32 " and %" B_PRIu32
			" user SGPRs, expected 6 and 4", kTriangleVs.userSgprs,
			kConstantPs.userSgprs);
		return B_BAD_VALUE;
	}

	// The vertices, apex first, in pixel coordinates of the frame buffer.
	float vertices[6] = {
		x + size / 2.0f, (float)y,
		(float)(x + size), (float)(y + size),
		(float)x, (float)(y + size)
	};
	float rgba[4] = {
		((color >> 16) & 0xff) / 255.0f,
		((color >> 8) & 0xff) / 255.0f,
		(color & 0xff) / 255.0f,
		((color >> 24) & 0xff) / 255.0f
	};
	// One pixel in the triangle (its centroid), one in the square but well
	// left of the left edge, which runs from (x, y + size) to the apex.
	uint32 insideX = x + size / 2;
	uint32 insideY = y + size * 2 / 3;
	uint32 outsideX = x + size / 8;
	uint32 outsideY = y + size / 8;

	Log("gfx: draw: triangle (%.1f, %.1f) (%.1f, %.1f) (%.1f, %.1f), color "
		"0x%08" B_PRIx32 ", frame buffer MC 0x%" B_PRIx64 ", pitch %" B_PRIu32
		" pixels", vertices[0], vertices[1], vertices[2], vertices[3],
		vertices[4], vertices[5], color, frameBuffer, pitch);

	// The colour buffer's tile mode: index 8 must be LINEAR_ALIGNED, as
	// Linux programs it; StartGfx does not write the tiling table.
	uint32 tileMode = gpu.Read(mmGB_TILE_MODE8);
	if (tileMode != kStoneyTileMode8) {
		gpu.Write(mmGB_TILE_MODE8, kStoneyTileMode8);
		Log("gfx: draw: GB_TILE_MODE8 (0x%04x) 0x%08" B_PRIx32 " -> 0x%08"
			B_PRIx32 " (reads 0x%08" B_PRIx32 ")", mmGB_TILE_MODE8, tileMode,
			kStoneyTileMode8, gpu.Read(mmGB_TILE_MODE8));
	} else {
		Log("gfx: draw: GB_TILE_MODE8 (0x%04x) is 0x%08" B_PRIx32
			" (LINEAR_ALIGNED)", mmGB_TILE_MODE8, tileMode);
	}

	// The shaders, each at a 256 byte boundary (SPI_SHADER_PGM_LO is >> 8).
	uint32 vsSize = (kTriangleVs.dwords * 4 + kShaderAlignment - 1)
		& ~(kShaderAlignment - 1);
	uint32 psSize = kConstantPs.dwords * 4;
	GpuMemory shaders;
	status_t status = gpu.Allocate(vsSize + psSize, shaders,
		kShaderAlignment);
	if (status != B_OK) {
		Log("gfx: draw: no GPU memory for the shaders");
		return status;
	}
	uint64 vsAddress = upload_shader(shaders, 0, kTriangleVs);
	uint64 psAddress = upload_shader(shaders, vsSize, kConstantPs);
	flush_hdp(gpu);
	Log("gfx: draw: VS %" B_PRIu32 " dwords at MC 0x%" B_PRIx64 ", PS %"
		B_PRIu32 " dwords at MC 0x%" B_PRIx64, kTriangleVs.dwords, vsAddress,
		kConstantPs.dwords, psAddress);

	// What is there before the draw.
	invalidate_hdp(gpu);
	uint32 insideBefore = read_pixel(gpu, insideX, insideY);
	uint32 outsideBefore = read_pixel(gpu, outsideX, outsideY);
	Log("gfx: draw: before: pixel (%" B_PRIu32 ", %" B_PRIu32 ") = 0x%08"
		B_PRIx32 ", pixel (%" B_PRIu32 ", %" B_PRIu32 ") = 0x%08" B_PRIx32,
		insideX, insideY, insideBefore, outsideX, outsideY, outsideBefore);

	status = ring_alloc(gpu, ring, kMaxDrawDwords);
	if (status != B_OK)
		return fail(gpu, "ring stuck before the draw", status);
	uint32 start = ring.wptr;

	// Load and shadow nothing from memory, as radeonsi starts its IBs
	// (si_begin_new_gfx_cs) and gfx_v8_0_cp_gfx_start does.
	ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	ring_write(ring, 0x80000000);
	ring_write(ring, 0x80000000);

	// Let earlier work drain before the context changes.
	ring_write(ring, PACKET3(PACKET3_EVENT_WRITE, 0));
	ring_write(ring, EVENT_TYPE(PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
	ring_write(ring, PACKET3(PACKET3_EVENT_WRITE, 0));
	ring_write(ring, EVENT_TYPE(VS_PARTIAL_FLUSH) | EVENT_INDEX(4));

	// Invalidate the shader instruction and constant caches and the TC for
	// the new code (gfx_v8_0_emit_mem_sync).
	ring_write(ring, PACKET3(PACKET3_SURFACE_SYNC, 3));
	ring_write(ring, PACKET3_TCL1_ACTION_ENA | PACKET3_TC_ACTION_ENA
		| PACKET3_SH_KCACHE_ACTION_ENA | PACKET3_SH_ICACHE_ACTION_ENA);
	ring_write(ring, 0xffffffff);	// CP_COHER_SIZE
	ring_write(ring, 0);			// CP_COHER_BASE
	ring_write(ring, 0x0000000a);	// poll interval

	// --- VGT: a plain VS-PS pipeline, no tessellation, GS or streamout.
	SET_CONTEXT(VGT_SHADER_STAGES_EN, 0);
	SET_CONTEXT(VGT_GS_MODE, 0);
	SET_CONTEXT(VGT_PRIMITIVEID_EN, 0);
	SET_CONTEXT(VGT_REUSE_OFF, 0);
	SET_CONTEXT(VGT_VTX_CNT_EN, 0);
	SET_CONTEXT(VGT_LS_HS_CONFIG, 0);
	SET_CONTEXT(VGT_STRMOUT_CONFIG, 0);
	SET_CONTEXT(VGT_MULTI_PRIM_IB_RESET_EN, 0);
	// The auto index is clamped to [MIN, MAX]; the VS picks the vertex from
	// v0, so the offset must be 0 (si_init_cs_preamble_state).
	SET_CONTEXT(VGT_MAX_VTX_INDX, 0xffffffff);
	SET_CONTEXT(VGT_MIN_VTX_INDX, 0);
	SET_CONTEXT(VGT_INDX_OFFSET, 0);
	// si_get_init_multi_vgt_param(): primgroups of 128, WD_SWITCH_ON_EOP
	// with fewer than four SEs (Stoney has one), MAX_PRIMGRP_IN_WAVE 2 on
	// GFX8.
	// With index 1 in the register offset dword, as radeonsi's
	// si_emit_ia_multi_vgt_param does on GFX7-8.
	{
		uint32 value = (127 << IA_MULTI_VGT_PARAM__PRIMGROUP_SIZE__SHIFT)
			| (1 << IA_MULTI_VGT_PARAM__WD_SWITCH_ON_EOP__SHIFT)
			| (2 << IA_MULTI_VGT_PARAM__MAX_PRIMGRP_IN_WAVE__SHIFT);
		ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 1));
		ring_write(ring, (mmIA_MULTI_VGT_PARAM
			- PACKET3_SET_CONTEXT_REG_START) | (1u << 28));
		ring_write(ring, value);
		Log("gfx: draw: context IA_MULTI_VGT_PARAM (0x%04x, index 1) = 0x%08"
			B_PRIx32, mmIA_MULTI_VGT_PARAM, value);
	}

	// --- PA: window coordinates in, no clipping, no culling, one sample.
	SET_CONTEXT(PA_CL_CLIP_CNTL, PA_CL_CLIP_CNTL__CLIP_DISABLE_MASK
		| PA_CL_CLIP_CNTL__DX_CLIP_SPACE_DEF_MASK);
	SET_CONTEXT(PA_CL_VTE_CNTL, PA_CL_VTE_CNTL__VTX_XY_FMT_MASK
		| PA_CL_VTE_CNTL__VTX_Z_FMT_MASK | PA_CL_VTE_CNTL__VTX_W0_FMT_MASK);
	SET_CONTEXT(PA_CL_VS_OUT_CNTL, 0);
	// Guard band of 1.0, unused with clipping off (si_emit_guardband).
	SET_CONTEXT(PA_CL_GB_VERT_CLIP_ADJ, float_bits(1.0f));
	SET_CONTEXT(PA_CL_GB_VERT_DISC_ADJ, float_bits(1.0f));
	SET_CONTEXT(PA_CL_GB_HORZ_CLIP_ADJ, float_bits(1.0f));
	SET_CONTEXT(PA_CL_GB_HORZ_DISC_ADJ, float_bits(1.0f));
	// si_create_rs_state(): half-pixel centres, 1/256 sub-pixel precision
	// (QUANT_MODE X_16_8_FIXED_POINT_1_256TH = 5).
	SET_CONTEXT(PA_SU_VTX_CNTL, (1 << PA_SU_VTX_CNTL__PIX_CENTER__SHIFT)
		| (5 << PA_SU_VTX_CNTL__QUANT_MODE__SHIFT));
	SET_CONTEXT(PA_SU_SC_MODE_CNTL, 0);
	SET_CONTEXT(PA_SU_HARDWARE_SCREEN_OFFSET, 0);
	SET_CONTEXT(PA_SC_MODE_CNTL_0, 0);
	SET_CONTEXT(PA_SC_MODE_CNTL_1, 0);
	SET_CONTEXT(PA_SC_LINE_CNTL, 0);
	SET_CONTEXT(PA_SC_AA_CONFIG, 0);
	SET_CONTEXT(PA_SC_AA_MASK_X0Y0_X1Y0, 0xffffffff);
	SET_CONTEXT(PA_SC_AA_MASK_X0Y1_X1Y1, 0xffffffff);
	// si_init_cs_preamble_state(): ER_TRI/POINT/RECT 0xa, ER_LINE_* 0x1a.
	SET_CONTEXT(PA_SC_EDGERULE, 0xaa99aaaa);
	SET_CONTEXT(PA_SC_CLIPRECT_RULE, 0xffff);

	// Scissors: the whole screen. BR is exclusive; TL disables the window
	// offset, as si_emit_one_scissor does.
	uint32 screenBr = (height << 16) | width;
	SET_CONTEXT(PA_SC_SCREEN_SCISSOR_TL, 0);
	SET_CONTEXT(PA_SC_SCREEN_SCISSOR_BR, screenBr);
	SET_CONTEXT(PA_SC_WINDOW_OFFSET, 0);
	SET_CONTEXT(PA_SC_WINDOW_SCISSOR_TL,
		PA_SC_WINDOW_SCISSOR_TL__WINDOW_OFFSET_DISABLE_MASK);
	SET_CONTEXT(PA_SC_WINDOW_SCISSOR_BR, screenBr);
	SET_CONTEXT(PA_SC_GENERIC_SCISSOR_TL,
		PA_SC_GENERIC_SCISSOR_TL__WINDOW_OFFSET_DISABLE_MASK);
	SET_CONTEXT(PA_SC_GENERIC_SCISSOR_BR, screenBr);
	SET_CONTEXT(PA_SC_VPORT_SCISSOR_0_TL,
		PA_SC_VPORT_SCISSOR_0_TL__WINDOW_OFFSET_DISABLE_MASK);
	SET_CONTEXT(PA_SC_VPORT_SCISSOR_0_BR, screenBr);
	SET_CONTEXT(PA_SC_VPORT_ZMIN_0, float_bits(0.0f));
	SET_CONTEXT(PA_SC_VPORT_ZMAX_0, float_bits(1.0f));

	// --- SPI: one parameter, POS0 with four components, PS with
	// PERSP_CENTER only (GFX8 needs one I/J pair enabled; Shaders.cpp keeps
	// v0/v1 for it), MRT0 as 32_ABGR, no Z export.
	SET_CONTEXT(SPI_VS_OUT_CONFIG, 0);
		// VS_EXPORT_COUNT = parameters - 1
	SET_CONTEXT(SPI_SHADER_POS_FORMAT, SPI_SHADER_4COMP);
	SET_CONTEXT(SPI_PS_INPUT_ENA, SPI_PS_INPUT_ENA__PERSP_CENTER_ENA_MASK);
	SET_CONTEXT(SPI_PS_INPUT_ADDR, SPI_PS_INPUT_ADDR__PERSP_CENTER_ENA_MASK);
	// No interpolated inputs (radeonsi's NUM_INTERP(num_interp) with 0); the
	// clear state leaves 2.
	SET_CONTEXT(SPI_PS_IN_CONTROL, 0);
	SET_CONTEXT(SPI_PS_INPUT_CNTL_0, 0 << SPI_PS_INPUT_CNTL_0__OFFSET__SHIFT);
	// si_shader_ps(): FRONT_FACE_ALL_BITS.
	SET_CONTEXT(SPI_BARYC_CNTL, SPI_BARYC_CNTL__FRONT_FACE_ALL_BITS_MASK);
	SET_CONTEXT(SPI_SHADER_Z_FORMAT, 0);	// SPI_SHADER_ZERO
	SET_CONTEXT(SPI_SHADER_COL_FORMAT,
		SPI_SHADER_32_ABGR << SPI_SHADER_COL_FORMAT__COL0_EXPORT_FORMAT__SHIFT);

	// --- CB: MRT0 only, no blending, ROP3 copy.
	SET_CONTEXT(CB_SHADER_MASK, 0xf);
	SET_CONTEXT(CB_TARGET_MASK, 0xf);
	SET_CONTEXT(CB_COLOR_CONTROL,
		(CB_NORMAL << CB_COLOR_CONTROL__MODE__SHIFT)
		| (0xcc << CB_COLOR_CONTROL__ROP3__SHIFT));
	SET_CONTEXT(CB_BLEND0_CONTROL, 0);
	// RB+: no down-conversion for a 32_ABGR export, blend optimisation off
	// for a target without blending (si_emit_cb_render_state,
	// si_create_blend_state).
	set_context_reg(ring, "SX_PS_DOWNCONVERT", mmSX_PS_DOWNCONVERT, 0);
	set_context_reg(ring, "SX_BLEND_OPT_EPSILON", mmSX_BLEND_OPT_EPSILON, 0);
	set_context_reg(ring, "SX_BLEND_OPT_CONTROL", mmSX_BLEND_OPT_CONTROL, 0);
	set_context_reg(ring, "SX_MRT0_BLEND_OPT", mmSX_MRT0_BLEND_OPT,
		(kSxOptCombBlendDisabled << kSxMrtBlendOptColorCombFcnShift)
		| (kSxOptCombBlendDisabled << kSxMrtBlendOptAlphaCombFcnShift));

	// The colour buffer: the visible frame buffer, linear aligned, 8888
	// UNORM. B_RGB32 is B, G, R, A in memory: SWAP_ALT, as radeonsi's
	// si_translate_colorswap() picks for B8G8R8A8. No CMASK, FMASK or DCC:
	// those point at the base like radeonsi does without them.
	uint32 base = (uint32)(frameBuffer >> 8);
	uint32 pitchTileMax = pitch / 8 - 1;
	uint32 sliceTileMax = pitch * height / 64 - 1;
	SET_CONTEXT(CB_COLOR0_BASE, base);
	SET_CONTEXT(CB_COLOR0_PITCH,
		(pitchTileMax << CB_COLOR0_PITCH__TILE_MAX__SHIFT)
		| (pitchTileMax << CB_COLOR0_PITCH__FMASK_TILE_MAX__SHIFT));
	SET_CONTEXT(CB_COLOR0_SLICE,
		sliceTileMax << CB_COLOR0_SLICE__TILE_MAX__SHIFT);
	SET_CONTEXT(CB_COLOR0_VIEW, 0);
	// si_initialize_color_surface(): BLEND_CLAMP for UNORM, ROUND_MODE 0.
	SET_CONTEXT(CB_COLOR0_INFO,
		(COLOR_8_8_8_8 << CB_COLOR0_INFO__FORMAT__SHIFT)
		| (NUMBER_UNORM << CB_COLOR0_INFO__NUMBER_TYPE__SHIFT)
		| (SWAP_ALT << CB_COLOR0_INFO__COMP_SWAP__SHIFT)
		| CB_COLOR0_INFO__BLEND_CLAMP_MASK
		| CB_COLOR0_INFO__SIMPLE_FLOAT_MASK);	// as radeonsi
	SET_CONTEXT(CB_COLOR0_ATTRIB,
		(kLinearAlignedTileIndex << CB_COLOR0_ATTRIB__TILE_MODE_INDEX__SHIFT)
		| (kLinearAlignedTileIndex
			<< CB_COLOR0_ATTRIB__FMASK_TILE_MODE_INDEX__SHIFT));
	SET_CONTEXT(CB_COLOR0_DCC_CONTROL, 0);
	SET_CONTEXT(CB_COLOR0_CMASK, base);
	SET_CONTEXT(CB_COLOR0_CMASK_SLICE, 0);
	SET_CONTEXT(CB_COLOR0_FMASK, base);
	SET_CONTEXT(CB_COLOR0_FMASK_SLICE, sliceTileMax);

	// --- DB: off. Hi-Z/Hi-S forced off as si_emit_db_render_state does
	// without HTILE; EARLY_Z_THEN_LATE_Z as si_shader_ps.
	SET_CONTEXT(DB_RENDER_CONTROL, 0);
	SET_CONTEXT(DB_RENDER_OVERRIDE,
		(FORCE_DISABLE << DB_RENDER_OVERRIDE__FORCE_HIZ_ENABLE__SHIFT)
		| (FORCE_DISABLE << DB_RENDER_OVERRIDE__FORCE_HIS_ENABLE0__SHIFT)
		| (FORCE_DISABLE << DB_RENDER_OVERRIDE__FORCE_HIS_ENABLE1__SHIFT));
	SET_CONTEXT(DB_DEPTH_CONTROL, 0);
	SET_CONTEXT(DB_STENCIL_CONTROL, 0);
	SET_CONTEXT(DB_Z_INFO, 0);			// FORMAT Z_INVALID
	SET_CONTEXT(DB_STENCIL_INFO, 0);	// FORMAT STENCIL_INVALID
	SET_CONTEXT(DB_SHADER_CONTROL,
		EARLY_Z_THEN_LATE_Z << DB_SHADER_CONTROL__Z_ORDER__SHIFT);

	// --- Shaders. RSRC3: all CUs, no wave limit; no late VS allocation on
	// chips with at most four CUs per SH (si_init_cs_preamble_state).
	SET_SH(SPI_SHADER_PGM_LO_VS, (uint32)(vsAddress >> 8));
	SET_SH(SPI_SHADER_PGM_HI_VS, (uint32)(vsAddress >> 40));
	SET_SH(SPI_SHADER_PGM_RSRC1_VS, kTriangleVs.rsrc1);
	SET_SH(SPI_SHADER_PGM_RSRC2_VS, kTriangleVs.rsrc2);
	SET_SH(SPI_SHADER_PGM_RSRC3_VS,
		(0xffff << SPI_SHADER_PGM_RSRC3_VS__CU_EN__SHIFT)
		| (0x3f << SPI_SHADER_PGM_RSRC3_VS__WAVE_LIMIT__SHIFT));
	SET_SH(SPI_SHADER_LATE_ALLOC_VS, 0);
	for (uint32 i = 0; i < 6; i++) {
		set_sh_reg(ring, "SPI_SHADER_USER_DATA_VS_n",
			mmSPI_SHADER_USER_DATA_VS_0 + i, float_bits(vertices[i]));
	}

	SET_SH(SPI_SHADER_PGM_LO_PS, (uint32)(psAddress >> 8));
	SET_SH(SPI_SHADER_PGM_HI_PS, (uint32)(psAddress >> 40));
	SET_SH(SPI_SHADER_PGM_RSRC1_PS, kConstantPs.rsrc1);
	SET_SH(SPI_SHADER_PGM_RSRC2_PS, kConstantPs.rsrc2);
	SET_SH(SPI_SHADER_PGM_RSRC3_PS,
		(0xffff << SPI_SHADER_PGM_RSRC3_PS__CU_EN__SHIFT)
		| (0x3f << SPI_SHADER_PGM_RSRC3_PS__WAVE_LIMIT__SHIFT));
	for (uint32 i = 0; i < 4; i++) {
		set_sh_reg(ring, "SPI_SHADER_USER_DATA_PS_n",
			mmSPI_SHADER_USER_DATA_PS_0 + i, float_bits(rgba[i]));
	}

	// --- The draw. VGT_PRIMITIVE_TYPE is a uconfig register on GFX7+
	// (0xc242); radeonsi writes it without an index on GFX8. The index type
	// and instance count go by packet before GFX9 (si_emit_draw_packets).
	SET_UCONFIG(VGT_PRIMITIVE_TYPE, DI_PT_TRILIST);
	ring_write(ring, PACKET3(PACKET3_INDEX_TYPE, 0));
	ring_write(ring, 0);	// 16-bit, unused by an auto-index draw
	Log("gfx: draw: INDEX_TYPE 0");
	ring_write(ring, PACKET3(PACKET3_NUM_INSTANCES, 0));
	ring_write(ring, 1);
	Log("gfx: draw: NUM_INSTANCES 1");
	ring_write(ring, PACKET3(PACKET3_DRAW_INDEX_AUTO, 1));
	ring_write(ring, 3);
	ring_write(ring,
		DI_SRC_SEL_AUTO_INDEX << VGT_DRAW_INITIATOR__SOURCE_SELECT__SHIFT);
	Log("gfx: draw: DRAW_INDEX_AUTO 3 vertices, initiator 0x%08x",
		DI_SRC_SEL_AUTO_INDEX << VGT_DRAW_INITIATOR__SOURCE_SELECT__SHIFT);

	uint32 written = (ring.wptr - start) & ring_mask(ring);
	if (written > kMaxDrawDwords) {
		// Too late to take it back; only possible if this file grew.
		Log("gfx: draw: wrote %" B_PRIu32 " dwords, allocated %" B_PRIu32,
			written, kMaxDrawDwords);
	}
	ring_commit(gpu, ring);
	Log("gfx: draw: %" B_PRIu32 " dwords committed, wptr 0x%" B_PRIx32,
		written, ring.wptr);

	// The fence's CACHE_FLUSH_AND_INV_TS_EVENT writes the CB back.
	status = fence_and_wait(gpu, ring, "draw");
	if (status != B_OK)
		return fail(gpu, "draw: fence timed out", status);

	invalidate_hdp(gpu);
	uint32 inside = read_pixel(gpu, insideX, insideY);
	uint32 outside = read_pixel(gpu, outsideX, outsideY);
	Log("gfx: draw: after: pixel (%" B_PRIu32 ", %" B_PRIu32 ") = 0x%08"
		B_PRIx32 " (want 0x%08" B_PRIx32 "), pixel (%" B_PRIu32 ", %" B_PRIu32
		") = 0x%08" B_PRIx32 " (want 0x%08" B_PRIx32 ")", insideX, insideY,
		inside, color, outsideX, outsideY, outside, outsideBefore);
	// Also the apex and base rows, for orientation (only logged).
	Log("gfx: draw: pixel below the apex (%" B_PRIu32 ", %" B_PRIu32
		") = 0x%08" B_PRIx32 ", pixel above the base middle (%" B_PRIu32 ", %"
		B_PRIu32 ") = 0x%08" B_PRIx32, x + size / 2, y + 2,
		read_pixel(gpu, x + size / 2, y + 2), x + size / 2, y + size - 2,
		read_pixel(gpu, x + size / 2, y + size - 2));

	if (inside != color)
		return fail(gpu, "draw: the triangle's centre has the wrong colour",
			B_ERROR);
	if (outside != outsideBefore)
		return fail(gpu, "draw: a pixel outside the triangle changed",
			B_ERROR);

	Log("gfx: draw passed");
	return B_OK;
}


}	// namespace gfx8
