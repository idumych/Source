/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


// The read-only first step of gfx8test: logs the state the 3D engine, the
// memory controller and the SMU are in before we touch anything.
//
// Declared in Steps.h as:
//	status_t Probe(Gpu& gpu);
//
// It never writes a GPU register, with one exception: reading an SMU
// register goes through the mmMP0PUB_IND_INDEX/DATA pair, so the index
// register is written. Nothing else in the system uses that pair while
// radeon_hd drives the display (radeon_hd does not touch it), and it only
// selects what the data register shows.
//
// The register meanings and the SMU firmware header location follow
// Linux v6.12 (gmc_v8_0.c, gfx_v8_0.c, smu8_smumgr.c); see amd/README.


#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Steps.h"

#include "bif_5_0_d.h"
#include "gfx_8_0_d.h"
#include "gfx_8_0_sh_mask.h"
#include "gmc_8_1_d.h"
#include "gmc_8_1_sh_mask.h"
#include "oss_3_0_d.h"
#include "oss_3_0_sh_mask.h"
#include "smu_8_0_d.h"
#include "cz_ppsmc.h"
#include "smu8.h"
#include "smu_ucode_xfer_cz.h"


namespace gfx8 {


struct BitName {
	uint32		mask;
	const char*	name;
};


static const BitName kGrbmStatusBits[] = {
	{ GRBM_STATUS__SRBM_RQ_PENDING_MASK, "SRBM_RQ_PENDING" },
	{ GRBM_STATUS__ME0PIPE0_CF_RQ_PENDING_MASK, "CF_RQ_PENDING" },
	{ GRBM_STATUS__ME0PIPE0_PF_RQ_PENDING_MASK, "PF_RQ_PENDING" },
	{ GRBM_STATUS__GDS_DMA_RQ_PENDING_MASK, "GDS_DMA_RQ_PENDING" },
	{ GRBM_STATUS__DB_CLEAN_MASK, "DB_CLEAN" },
	{ GRBM_STATUS__CB_CLEAN_MASK, "CB_CLEAN" },
	{ GRBM_STATUS__TA_BUSY_MASK, "TA" },
	{ GRBM_STATUS__GDS_BUSY_MASK, "GDS" },
	{ GRBM_STATUS__VGT_BUSY_MASK, "VGT" },
	{ GRBM_STATUS__IA_BUSY_MASK, "IA" },
	{ GRBM_STATUS__SX_BUSY_MASK, "SX" },
	{ GRBM_STATUS__WD_BUSY_MASK, "WD" },
	{ GRBM_STATUS__SPI_BUSY_MASK, "SPI" },
	{ GRBM_STATUS__BCI_BUSY_MASK, "BCI" },
	{ GRBM_STATUS__SC_BUSY_MASK, "SC" },
	{ GRBM_STATUS__PA_BUSY_MASK, "PA" },
	{ GRBM_STATUS__DB_BUSY_MASK, "DB" },
	{ GRBM_STATUS__CP_COHERENCY_BUSY_MASK, "CP_COHERENCY" },
	{ GRBM_STATUS__CP_BUSY_MASK, "CP" },
	{ GRBM_STATUS__CB_BUSY_MASK, "CB" },
	{ GRBM_STATUS__GUI_ACTIVE_MASK, "GUI_ACTIVE" },
	{ 0, NULL }
};

static const BitName kGrbmStatus2Bits[] = {
	{ GRBM_STATUS2__ME1PIPE0_RQ_PENDING_MASK, "ME1PIPE0_RQ" },
	{ GRBM_STATUS2__ME2PIPE0_RQ_PENDING_MASK, "ME2PIPE0_RQ" },
	{ GRBM_STATUS2__RLC_RQ_PENDING_MASK, "RLC_RQ_PENDING" },
	{ GRBM_STATUS2__RLC_BUSY_MASK, "RLC" },
	{ GRBM_STATUS2__TC_BUSY_MASK, "TC" },
	{ GRBM_STATUS2__TCC_CC_RESIDENT_MASK, "TCC_CC_RESIDENT" },
	{ GRBM_STATUS2__CPF_BUSY_MASK, "CPF" },
	{ GRBM_STATUS2__CPC_BUSY_MASK, "CPC" },
	{ GRBM_STATUS2__CPG_BUSY_MASK, "CPG" },
	{ 0, NULL }
};

static const BitName kSrbmStatusBits[] = {
	{ SRBM_STATUS__UVD_RQ_PENDING_MASK, "UVD_RQ" },
	{ SRBM_STATUS__SMU_RQ_PENDING_MASK, "SMU_RQ" },
	{ SRBM_STATUS__GRBM_RQ_PENDING_MASK, "GRBM_RQ" },
	{ SRBM_STATUS__HI_RQ_PENDING_MASK, "HI_RQ" },
	{ SRBM_STATUS__VMC_BUSY_MASK, "VMC" },
	{ SRBM_STATUS__MCB_BUSY_MASK, "MCB" },
	{ SRBM_STATUS__MCB_NON_DISPLAY_BUSY_MASK, "MCB_NON_DISPLAY" },
	{ SRBM_STATUS__MCC_BUSY_MASK, "MCC" },
	{ SRBM_STATUS__MCD_BUSY_MASK, "MCD" },
	{ SRBM_STATUS__VMC1_BUSY_MASK, "VMC1" },
	{ SRBM_STATUS__SEM_BUSY_MASK, "SEM" },
	{ SRBM_STATUS__IH_BUSY_MASK, "IH" },
	{ SRBM_STATUS__UVD_BUSY_MASK, "UVD" },
	{ SRBM_STATUS__GCATCL2_BUSY_MASK, "GCATCL2" },
	{ SRBM_STATUS__OSATCL2_BUSY_MASK, "OSATCL2" },
	{ SRBM_STATUS__BIF_BUSY_MASK, "BIF" },
	{ 0, NULL }
};

static const BitName kSrbmStatus2Bits[] = {
	{ SRBM_STATUS2__SDMA_RQ_PENDING_MASK, "SDMA_RQ" },
	{ SRBM_STATUS2__SDMA_BUSY_MASK, "SDMA" },
	{ SRBM_STATUS2__VCE0_BUSY_MASK, "VCE0" },
	{ SRBM_STATUS2__XDMA_BUSY_MASK, "XDMA" },
	{ 0, NULL }
};

static const BitName kCpStatBits[] = {
	{ CP_STAT__ROQ_RING_BUSY_MASK, "ROQ_RING" },
	{ CP_STAT__DC_BUSY_MASK, "DC" },
	{ CP_STAT__PFP_BUSY_MASK, "PFP" },
	{ CP_STAT__MEQ_BUSY_MASK, "MEQ" },
	{ CP_STAT__ME_BUSY_MASK, "ME" },
	{ CP_STAT__DMA_BUSY_MASK, "DMA" },
	{ CP_STAT__CE_BUSY_MASK, "CE" },
	{ CP_STAT__CP_BUSY_MASK, "CP" },
	{ 0, NULL }
};

static const BitName kCpMeCntlBits[] = {
	{ CP_ME_CNTL__CE_PIPE0_RESET_MASK, "CE_RESET" },
	{ CP_ME_CNTL__PFP_PIPE0_RESET_MASK, "PFP_RESET" },
	{ CP_ME_CNTL__ME_PIPE0_RESET_MASK, "ME_RESET" },
	{ CP_ME_CNTL__CE_HALT_MASK, "CE_HALT" },
	{ CP_ME_CNTL__PFP_HALT_MASK, "PFP_HALT" },
	{ CP_ME_CNTL__ME_HALT_MASK, "ME_HALT" },
	{ 0, NULL }
};

static const BitName kCpMecCntlBits[] = {
	{ CP_MEC_CNTL__MEC_ME1_HALT_MASK, "MEC1_HALT" },
	{ CP_MEC_CNTL__MEC_ME2_HALT_MASK, "MEC2_HALT" },
	{ CP_MEC_CNTL__MEC_ME1_PIPE0_RESET_MASK, "MEC1_PIPE0_RESET" },
	{ 0, NULL }
};

static const BitName kRlcGpmStatBits[] = {
	{ RLC_GPM_STAT__RLC_BUSY_MASK, "RLC_BUSY" },
	{ RLC_GPM_STAT__GFX_POWER_STATUS_MASK, "GFX_POWER_ON" },
	{ RLC_GPM_STAT__GFX_CLOCK_STATUS_MASK, "GFX_CLOCK_ON" },
	{ RLC_GPM_STAT__GFX_LS_STATUS_MASK, "GFX_LIGHT_SLEEP" },
	{ RLC_GPM_STAT__GFX_PIPELINE_POWER_STATUS_MASK, "PIPELINE_POWER_ON" },
	{ RLC_GPM_STAT__SAVING_REGISTERS_MASK, "SAVING_REGS" },
	{ RLC_GPM_STAT__RESTORING_REGISTERS_MASK, "RESTORING_REGS" },
	{ RLC_GPM_STAT__GFX3D_BLOCKS_CHANGING_POWER_STATE_MASK,
		"GFX3D_CHANGING_POWER" },
	{ RLC_GPM_STAT__ABORTED_PD_SEQUENCE_MASK, "ABORTED_POWER_DOWN" },
	{ 0, NULL }
};

static const BitName kRlcPgCntlBits[] = {
	{ RLC_PG_CNTL__GFX_POWER_GATING_ENABLE_MASK, "GFX_PG" },
	{ RLC_PG_CNTL__GFX_POWER_GATING_SRC_MASK, "PG_SRC" },
	{ RLC_PG_CNTL__DYN_PER_CU_PG_ENABLE_MASK, "DYN_CU_PG" },
	{ RLC_PG_CNTL__STATIC_PER_CU_PG_ENABLE_MASK, "STATIC_CU_PG" },
	{ RLC_PG_CNTL__GFX_PIPELINE_PG_ENABLE_MASK, "PIPELINE_PG" },
	{ RLC_PG_CNTL__PG_OVERRIDE_MASK, "PG_OVERRIDE" },
	{ RLC_PG_CNTL__CP_PG_DISABLE_MASK, "CP_PG_DISABLE" },
	{ RLC_PG_CNTL__SMU_HANDSHAKE_ENABLE_MASK, "SMU_HANDSHAKE" },
	{ RLC_PG_CNTL__QUICK_PG_ENABLE_MASK, "QUICK_PG" },
	{ 0, NULL }
};

static const BitName kUcodeLoadBits[] = {
	{ UCODE_ID_SDMA0_MASK, "SDMA0" },
	{ UCODE_ID_SDMA1_MASK, "SDMA1" },
	{ UCODE_ID_CP_CE_MASK, "CE" },
	{ UCODE_ID_CP_PFP_MASK, "PFP" },
	{ UCODE_ID_CP_ME_MASK, "ME" },
	{ UCODE_ID_CP_MEC_JT1_MASK, "MEC_JT1" },
	{ UCODE_ID_CP_MEC_JT2_MASK, "MEC_JT2" },
	{ UCODE_ID_RLC_G_MASK, "RLC_G" },
	{ 0, NULL }
};


// Writes the names of the set bits (or "none") to buffer.
static const char*
DecodeBits(uint32 value, const BitName* bits, char* buffer, size_t size)
{
	buffer[0] = '\0';
	size_t length = 0;
	for (; bits->name != NULL; bits++) {
		if ((value & bits->mask) == 0)
			continue;
		int written = snprintf(buffer + length, size - length, "%s%s",
			length > 0 ? " " : "", bits->name);
		if (written < 0 || (size_t)written >= size - length)
			break;
		length += written;
	}
	if (length == 0)
		strlcpy(buffer, "none", size);
	return buffer;
}


static uint32
Field(uint32 value, uint32 mask)
{
	if (mask == 0)
		return 0;
	return (value & mask) >> __builtin_ctz(mask);
}


static const char*
SmcResponseName(uint32 response)
{
	switch (response) {
		case 0:
			return "no response yet / idle";
		case PPSMC_Result_OK:
			return "OK";
		case PPSMC_Result_NoMore:
			return "no more";
		case PPSMC_Result_NotNow:
			return "not now";
		case PPSMC_Result_Failed:
			return "failed";
		case PPSMC_Result_UnknownCmd:
			return "unknown command";
		case PPSMC_Result_UnknownVT:
			return "unknown VT";
		default:
			return "?";
	}
}


static void
ProbeMemoryController(Gpu& gpu)
{
	uint32 fbLocation = gpu.Read(mmMC_VM_FB_LOCATION);
	uint64 fbBase = (uint64)Field(fbLocation,
		MC_VM_FB_LOCATION__FB_BASE_MASK) << 24;
	uint64 fbTop = ((uint64)Field(fbLocation,
		MC_VM_FB_LOCATION__FB_TOP_MASK) << 24) | 0xffffff;
	Log("mc: FB_LOCATION %#010" B_PRIx32 ": VRAM at MC %#" B_PRIx64
		"..%#" B_PRIx64 " (%" B_PRIu64 " MB)", fbLocation, fbBase, fbTop,
		(fbTop + 1 - fbBase) >> 20);

	uint32 fbOffset = gpu.Read(mmMC_VM_FB_OFFSET);
	Log("mc: FB_OFFSET %#010" B_PRIx32 ": carve-out at system address %#"
		B_PRIx64, fbOffset, (uint64)fbOffset << 22);

	uint32 configMemSize = gpu.Read(mmCONFIG_MEMSIZE);
	Log("mc: CONFIG_MEMSIZE %#010" B_PRIx32 " (%" B_PRIu32 " MB)",
		configMemSize, configMemSize);

	uint32 apertureLow = gpu.Read(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR);
	uint32 apertureHigh = gpu.Read(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR);
	uint32 apertureDefault = gpu.Read(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR);
	Log("mc: SYSTEM_APERTURE LOW %#010" B_PRIx32 " HIGH %#010" B_PRIx32
		" DEFAULT %#010" B_PRIx32 ": MC %#" B_PRIx64 "..%#" B_PRIx64
		", unmapped accesses go to %#" B_PRIx64, apertureLow, apertureHigh,
		apertureDefault, (uint64)apertureLow << 12,
		((uint64)apertureHigh << 12) | 0xfff, (uint64)apertureDefault << 12);

	uint32 agpBase = gpu.Read(mmMC_VM_AGP_BASE);
	uint32 agpBot = gpu.Read(mmMC_VM_AGP_BOT);
	uint32 agpTop = gpu.Read(mmMC_VM_AGP_TOP);
	Log("mc: AGP BASE %#010" B_PRIx32 " BOT %#010" B_PRIx32 " TOP %#010"
		B_PRIx32 ": %s", agpBase, agpBot, agpTop,
		agpBot > agpTop ? "AGP aperture disabled (BOT > TOP)"
			: "AGP aperture open");

	uint32 l1TlbCntl = gpu.Read(mmMC_VM_MX_L1_TLB_CNTL);
	Log("mc: MX_L1_TLB_CNTL %#010" B_PRIx32 ": L1 TLB %s, system access "
		"mode %" B_PRIu32, l1TlbCntl,
		(l1TlbCntl & MC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK) != 0
			? "on" : "off",
		Field(l1TlbCntl, MC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK));

	uint32 vmContext0 = gpu.Read(mmVM_CONTEXT0_CNTL);
	Log("mc: VM_CONTEXT0_CNTL %#010" B_PRIx32 ": GART context %s, page table "
		"depth %" B_PRIu32, vmContext0,
		(vmContext0 & VM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK) != 0
			? "enabled" : "disabled",
		Field(vmContext0, VM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK));

	if (fbBase != gpu.VramStart()) {
		Log("mc: note: harness VramStart %#" B_PRIx64 " differs from "
			"FB_LOCATION base", gpu.VramStart());
	}
}


// Returns false if GFX registers look unreachable (all ones).
static bool
ProbeGfx(Gpu& gpu)
{
	char names[256];

	uint32 grbmStatus = gpu.Read(mmGRBM_STATUS);
	Log("gfx: GRBM_STATUS %#010" B_PRIx32 ": %s; cmd fifo avail %" B_PRIu32
		" (%s)", grbmStatus,
		DecodeBits(grbmStatus, kGrbmStatusBits, names, sizeof(names)),
		Field(grbmStatus, GRBM_STATUS__ME0PIPE0_CMDFIFO_AVAIL_MASK),
		(grbmStatus & GRBM_STATUS__GUI_ACTIVE_MASK) != 0
			? "3D engine busy" : "3D engine idle");

	uint32 grbmStatus2 = gpu.Read(mmGRBM_STATUS2);
	Log("gfx: GRBM_STATUS2 %#010" B_PRIx32 ": %s", grbmStatus2,
		DecodeBits(grbmStatus2, kGrbmStatus2Bits, names, sizeof(names)));

	uint32 grbmStatusSe0 = gpu.Read(mmGRBM_STATUS_SE0);
	Log("gfx: GRBM_STATUS_SE0 %#010" B_PRIx32 "%s", grbmStatusSe0,
		(grbmStatusSe0 & 0xfffffffe) == 0 ? " (shader engine 0 idle)" : "");

	uint32 srbmStatus = gpu.Read(mmSRBM_STATUS);
	Log("gfx: SRBM_STATUS %#010" B_PRIx32 ": %s", srbmStatus,
		DecodeBits(srbmStatus, kSrbmStatusBits, names, sizeof(names)));

	uint32 srbmStatus2 = gpu.Read(mmSRBM_STATUS2);
	Log("gfx: SRBM_STATUS2 %#010" B_PRIx32 ": %s", srbmStatus2,
		DecodeBits(srbmStatus2, kSrbmStatus2Bits, names, sizeof(names)));

	uint32 cpStat = gpu.Read(mmCP_STAT);
	Log("cp: CP_STAT %#010" B_PRIx32 ": busy: %s", cpStat,
		DecodeBits(cpStat, kCpStatBits, names, sizeof(names)));

	uint32 meCntl = gpu.Read(mmCP_ME_CNTL);
	bool cpHalted = (meCntl & (CP_ME_CNTL__ME_HALT_MASK
		| CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK))
			== (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK
				| CP_ME_CNTL__CE_HALT_MASK);
	Log("cp: CP_ME_CNTL %#010" B_PRIx32 ": %s (%s)", meCntl,
		DecodeBits(meCntl, kCpMeCntlBits, names, sizeof(names)),
		cpHalted ? "gfx CP halted" : "gfx CP not fully halted");

	uint32 mecCntl = gpu.Read(mmCP_MEC_CNTL);
	Log("cp: CP_MEC_CNTL %#010" B_PRIx32 ": %s", mecCntl,
		DecodeBits(mecCntl, kCpMecCntlBits, names, sizeof(names)));

	uint32 rbCntl = gpu.Read(mmCP_RB0_CNTL);
	uint32 rbBase = gpu.Read(mmCP_RB0_BASE);
	uint32 rbBaseHi = gpu.Read(mmCP_RB0_BASE_HI);
	Log("cp: CP_RB0_BASE %#010" B_PRIx32 " BASE_HI %#010" B_PRIx32
		": ring at MC %#" B_PRIx64, rbBase, rbBaseHi,
		(((uint64)rbBaseHi << 32) | rbBase) << 8);
	Log("cp: CP_RB0_CNTL %#010" B_PRIx32 ": size %" B_PRIu32 " dwords, "
		"rptr writeback %s", rbCntl,
		2u << Field(rbCntl, CP_RB0_CNTL__RB_BUFSZ_MASK),
		(rbCntl & CP_RB0_CNTL__RB_NO_UPDATE_MASK) != 0 ? "off" : "on");
	Log("cp: CP_RB0_RPTR %#" B_PRIx32 " WPTR %#" B_PRIx32 " RPTR_ADDR %#"
		B_PRIx32 " RPTR_ADDR_HI %#" B_PRIx32, gpu.Read(mmCP_RB0_RPTR),
		gpu.Read(mmCP_RB0_WPTR), gpu.Read(mmCP_RB0_RPTR_ADDR),
		gpu.Read(mmCP_RB0_RPTR_ADDR_HI));

	uint32 rlcCntl = gpu.Read(mmRLC_CNTL);
	Log("rlc: RLC_CNTL %#010" B_PRIx32 ": RLC microcontroller %s", rlcCntl,
		(rlcCntl & RLC_CNTL__RLC_ENABLE_F32_MASK) != 0
			? "running" : "stopped");

	uint32 rlcStat = gpu.Read(mmRLC_STAT);
	Log("rlc: RLC_STAT %#010" B_PRIx32 ": %s%s%s", rlcStat,
		(rlcStat & RLC_STAT__RLC_BUSY_MASK) != 0 ? "busy " : "idle ",
		(rlcStat & RLC_STAT__RLC_GPM_BUSY_MASK) != 0 ? "GPM_BUSY " : "",
		(rlcStat & RLC_STAT__RLC_SPM_BUSY_MASK) != 0 ? "SPM_BUSY" : "");

	uint32 gpmStat = gpu.Read(mmRLC_GPM_STAT);
	Log("rlc: RLC_GPM_STAT %#010" B_PRIx32 ": %s; PG error %#" B_PRIx32,
		gpmStat, DecodeBits(gpmStat, kRlcGpmStatBits, names, sizeof(names)),
		Field(gpmStat, RLC_GPM_STAT__PG_ERROR_STATUS_MASK));

	uint32 pgCntl = gpu.Read(mmRLC_PG_CNTL);
	Log("rlc: RLC_PG_CNTL %#010" B_PRIx32 ": %s (%s)", pgCntl,
		DecodeBits(pgCntl, kRlcPgCntlBits, names, sizeof(names)),
		(pgCntl & (RLC_PG_CNTL__GFX_POWER_GATING_ENABLE_MASK
			| RLC_PG_CNTL__STATIC_PER_CU_PG_ENABLE_MASK
			| RLC_PG_CNTL__DYN_PER_CU_PG_ENABLE_MASK
			| RLC_PG_CNTL__GFX_PIPELINE_PG_ENABLE_MASK)) != 0
			? "some power gating enabled" : "power gating off");

	uint32 cgcg = gpu.Read(mmRLC_CGCG_CGLS_CTRL);
	Log("rlc: RLC_CGCG_CGLS_CTRL %#010" B_PRIx32 ": coarse clock gating %s, "
		"light sleep %s, idle threshold %#" B_PRIx32, cgcg,
		(cgcg & RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK) != 0 ? "on" : "off",
		(cgcg & RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK) != 0 ? "on" : "off",
		Field(cgcg, RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD_MASK));

	uint32 addrConfig = gpu.Read(mmGB_ADDR_CONFIG);
	Log("gfx: GB_ADDR_CONFIG %#010" B_PRIx32 ": %" B_PRIu32 " pipes, %"
		B_PRIu32 " shader engines, pipe interleave %" B_PRIu32 " bytes, "
		"row size %" B_PRIu32 " KB (Linux's golden value for Stoney is "
		"0x22010001)", addrConfig,
		1u << Field(addrConfig, GB_ADDR_CONFIG__NUM_PIPES_MASK),
		Field(addrConfig, GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK) + 1,
		256u << Field(addrConfig, GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK),
		1u << Field(addrConfig, GB_ADDR_CONFIG__ROW_SIZE_MASK));

	// Stoney has one shader engine, one array, at most three CUs
	// (gfx_v8_0_gpu_early_init(), gfx_v8_0_get_cu_active_bitmap()).
	uint32 ccShader = gpu.Read(mmCC_GC_SHADER_ARRAY_CONFIG);
	uint32 userShader = gpu.Read(mmGC_USER_SHADER_ARRAY_CONFIG);
	uint32 activeCus = ~Field(ccShader | userShader,
		CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK) & 0x7;
	Log("gfx: CC_GC_SHADER_ARRAY_CONFIG %#010" B_PRIx32
		" GC_USER_SHADER_ARRAY_CONFIG %#010" B_PRIx32 ": active CU mask %#"
		B_PRIx32 " (%d of 3 CUs)", ccShader, userShader, activeCus,
		__builtin_popcount(activeCus));

	// Is the 3D engine powered? With power gating on and GFX gated, its
	// registers read back as 0 or garbage; all ones means the register
	// BAR is not reaching the chip at all.
	bool unreachable = grbmStatus == 0xffffffff && addrConfig == 0xffffffff;
	bool gpmPowered = (gpmStat & RLC_GPM_STAT__GFX_POWER_STATUS_MASK) != 0;
	const char* verdict;
	if (unreachable)
		verdict = "registers read all ones: the GPU is not reachable";
	else if (addrConfig == 0 && grbmStatus == 0)
		verdict = "GFX reads as zero: likely powered off or gated";
	else if ((pgCntl & RLC_PG_CNTL__GFX_POWER_GATING_ENABLE_MASK) != 0
		&& !gpmPowered)
		verdict = "GFX power gating is on and the RLC reports GFX off";
	else if (gpmPowered)
		verdict = "GFX looks powered (RLC reports power on)";
	else
		verdict = "GFX registers answer; RLC power status bit is clear (RLC "
			"not running yet, so the bit may not mean much)";
	Log("gfx: power: %s; engine %s", verdict,
		(grbmStatus & GRBM_STATUS__GUI_ACTIVE_MASK) != 0 ? "busy" : "idle");

	return !unreachable;
}


static void
ProbeSmu(Gpu& gpu)
{
	const uint32 header = SMN_MP1_SRAM_START_ADDR
		+ SMU8_FIRMWARE_HEADER_LOCATION;

	// smu8_start_smu(): the version is the first dword of the firmware
	// header in MP1 SRAM; printed as major.minor.patch from bits 23..0.
	uint32 version = gpu.ReadSmc(header
		+ offsetof(struct SMU8_Firmware_Header, Version));
	Log("smu: firmware version %#010" B_PRIx32 " = %" B_PRIu32 ".%" B_PRIu32
		".%" B_PRIu32, version, (version >> 16) & 0xff,
		(version >> 8) & 0xff, version & 0xff);

	uint32 loadStatus = gpu.ReadSmc(header
		+ offsetof(struct SMU8_Firmware_Header, UcodeLoadStatus));
	char names[128];
	Log("smu: UcodeLoadStatus %#010" B_PRIx32 ": already loaded: %s",
		loadStatus,
		DecodeBits(loadStatus, kUcodeLoadBits, names, sizeof(names)));

	uint32 signature = gpu.ReadSmc(header
		+ offsetof(struct SMU8_Firmware_Header, Signature));
	Log("smu: header ImageSize %#" B_PRIx32 " Signature %#010" B_PRIx32,
		gpu.ReadSmc(header + offsetof(struct SMU8_Firmware_Header,
			ImageSize)), signature);

	uint32 response = gpu.Read(mmSMU_MP1_SRBM2P_RESP_0);
	Log("smu: SMU_MP1_SRBM2P_RESP_0 %#" B_PRIx32 " (%s), MSG_0 %#" B_PRIx32
		", ARG_0 %#" B_PRIx32, response, SmcResponseName(response),
		gpu.Read(mmSMU_MP1_SRBM2P_MSG_0), gpu.Read(mmSMU_MP1_SRBM2P_ARG_0));
}


// Returns the number of firmware files that could not be used.
static int
ProbeFirmware(Gpu& gpu)
{
	static const char* const kNames[] = {
		"stoney_ce.bin", "stoney_pfp.bin", "stoney_me.bin", "stoney_mec.bin",
		"stoney_rlc.bin", "stoney_sdma.bin"
	};

	int missing = 0;
	for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
		Firmware fw;
		memset(&fw, 0, sizeof(fw));
		status_t status = gpu.LoadFirmware(kNames[i], fw);
		if (status != B_OK) {
			Log("fw: %s: not readable: %s", kNames[i], strerror(status));
			missing++;
			continue;
		}

		// gfx, rlc v2 and sdma headers all keep ucode_feature_version
		// right after the 32 byte common header.
		uint32 feature = 0;
		if (fw.size >= 0x24)
			memcpy(&feature, fw.data + 0x20, sizeof(feature));

		bool fits = fw.ucodeOffset <= fw.size
			&& fw.ucodeSize <= fw.size - fw.ucodeOffset;
		Log("fw: %s: %" B_PRIuSIZE " bytes, header %" B_PRIu32 ".%" B_PRIu32
			", ucode version %" B_PRIu32 " (%#" B_PRIx32 "), feature %"
			B_PRIu32 ", ucode %" B_PRIu32 " bytes at %#" B_PRIx32
			", jump table %" B_PRIu32 "+%" B_PRIu32 " dwords%s", kNames[i],
			fw.size, fw.headerMajor, fw.headerMinor, fw.ucodeVersion,
			fw.ucodeVersion, feature, fw.ucodeSize, fw.ucodeOffset,
			fw.jtOffset, fw.jtSize, fits ? "" : " -- BROKEN: ucode past end");
		if (!fits)
			missing++;
		Gpu::FreeFirmware(fw);
	}
	return missing;
}


status_t
Probe(Gpu& gpu)
{
	uint32 pciId = gpu.PciId();
	Log("probe: PCI id %#010" B_PRIx32 "%s", pciId,
		(pciId & 0xffff) == 0x98e4 || (pciId >> 16) == 0x98e4
			? " (Stoney Ridge)" : " (not the expected 1002:98e4)");

	ProbeMemoryController(gpu);
	bool reachable = ProbeGfx(gpu);
	if (reachable)
		ProbeSmu(gpu);
	int missing = ProbeFirmware(gpu);

	if (!reachable) {
		Log("probe: FAILED: GPU registers unreachable");
		return B_IO_ERROR;
	}
	if (missing > 0) {
		Log("probe: FAILED: %d firmware file(s) missing or broken", missing);
		return B_ENTRY_NOT_FOUND;
	}
	Log("probe: done, nothing was changed");
	return B_OK;
}


}	// namespace gfx8
