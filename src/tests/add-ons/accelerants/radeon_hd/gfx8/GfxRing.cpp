/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */

// The 3D engine of Stoney Ridge (GFX8): golden registers, RLC start, CP gfx
// ring 0, and three tests on that ring. The sequences and values follow
// Linux's CHIP_STONEY paths; the firmware is already loaded through the SMU.
//
// Ported-from: Linux v6.12 drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c
// Ported-from-license: MIT


#include "Steps.h"
#include "RingInternal.h"

#include <new>
#include <string.h>

#include <OS.h>

#include "vid.h"
#include "gfx_8_0_d.h"
#include "gfx_8_0_sh_mask.h"
#include "gfx_8_0_enum.h"
#include "gmc_8_1_d.h"
#include "gmc_8_1_sh_mask.h"
#include "oss_3_0_d.h"
#include "bif_5_0_d.h"
#include "clearstate_defs.h"
#include "clearstate_vi.h"


namespace gfx8 {


// REG_SET_FIELD for a value built from zero or read before.
// Not in our gmc_8_1 headers; from Linux's gmc/gmc_8_2_d.h and
// gfx_v8_0.c.
static const uint32 kMcFusDram0BankAddrMapping = 0xa11;
static const uint32 kMcFusDram1BankAddrMapping = 0xa12;
static const uint32 kCarrizoGbAddrConfigGolden = 0x22010001;


#define SET_FIELD(value, reg, field, x) \
	(((value) & ~reg##__##field##_MASK) \
		| (((uint32)(x) << reg##__##field##__SHIFT) & reg##__##field##_MASK))


static const uint32 kRingSize = 64 * 1024;
	// bytes, a power of two
static const uint32 kRingAlignMask = 0xff;
	// gfx_v8_0_ring_funcs_gfx.align_mask
static const uint32 kRingNop = PACKET3(PACKET3_NOP, 0x3fff);

// Dword offsets in Ring::writeBack.
enum {
	kWbRptr			= 0,
	kWbWptrPoll		= 16,
	kWbFence		= 32,
	kWbTestData		= 48,
};

// Stoney (gfx_v8_0_gpu_early_init)
static const uint32 kMaxHwContexts = 8;
static const uint32 kScPrimFifoSizeFrontend = 0x20;
static const uint32 kScPrimFifoSizeBackend = 0x100;
static const uint32 kScHizTileFifoSize = 0x30;
static const uint32 kScEarlyzTileFifoSize = 0x130;

// Register/and-mask/or-value triples, as gfx_v8_0.c has them for Stoney.
static const uint32 kStoneyMgcgCgcgInit[] = {
	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	mmRLC_CGCG_CGLS_CTRL, 0xffffffff, 0x0020003f,
	mmCP_MEM_SLP_CNTL, 0xffffffff, 0x00020201,
	mmRLC_MEM_SLP_CNTL, 0xffffffff, 0x00020201,
	mmCGTS_SM_CTRL_REG, 0xffffffff, 0x96940200,
};

static const uint32 kStoneyGoldenSettingsA11[] = {
	mmDB_DEBUG2, 0xf00fffff, 0x00000400,
	mmGB_GPU_ID, 0x0000000f, 0x00000000,
	mmPA_SC_ENHANCE, 0xffffffff, 0x20000001,
	mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000,
	mmRLC_CGCG_CGLS_CTRL, 0x00000003, 0x0001003c,
	mmTA_CNTL_AUX, 0x000f000f, 0x000b0000,
	mmTCC_CTRL, 0x00100000, 0xf31fff7f,
	mmTCC_EXE_DISABLE, 0x00000002, 0x00000002,
	mmTCP_ADDR_CONFIG, 0x0000000f, 0x000000f1,
	mmTCP_CHAN_STEER_LO, 0xffffffff, 0x10101010,
};

static const uint32 kStoneyGoldenCommonAll[] = {
	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	mmPA_SC_RASTER_CONFIG, 0xffffffff, 0x00000000,
	mmPA_SC_RASTER_CONFIG_1, 0xffffffff, 0x00000000,
	mmGB_ADDR_CONFIG, 0xffffffff, 0x12010001,
	mmSPI_RESOURCE_RESERVE_CU_0, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_CU_1, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_EN_CU_0, 0xffffffff, 0x00FF7FBF,
	mmSPI_RESOURCE_RESERVE_EN_CU_1, 0xffffffff, 0x00FF7FAF,
};


//	#pragma mark - helpers


static inline volatile uint32*
wb(Ring& ring, uint32 index)
{
	return (volatile uint32*)ring.writeBack.cpu + index;
}


// Writes a register and logs its old and new value.
static void
write_logged(Gpu& gpu, const char* name, uint32 reg, uint32 value)
{
	uint32 old = gpu.Read(reg);
	gpu.Write(reg, value);
	Log("gfx: %s (0x%04" B_PRIx32 ") 0x%08" B_PRIx32 " -> 0x%08" B_PRIx32
		" (reads 0x%08" B_PRIx32 ")", name, reg, old, value, gpu.Read(reg));
}

#define WRITE_LOGGED(gpu, reg, value) write_logged(gpu, #reg, mm##reg, value)


// amdgpu_device_program_register_sequence() for pre-AI families: a full
// mask writes the value, otherwise the masked bits are cleared and the whole
// value (not only its masked bits) is or'ed in.
static void
program_register_sequence(Gpu& gpu, const char* name, const uint32* table,
	size_t count)
{
	Log("gfx: golden %s, %u registers", name, (unsigned)(count / 3));
	for (size_t i = 0; i + 2 < count; i += 3) {
		uint32 reg = table[i];
		uint32 andMask = table[i + 1];
		uint32 orMask = table[i + 2];
		uint32 old = gpu.Read(reg);
		uint32 value;
		if (andMask == 0xffffffff)
			value = orMask;
		else
			value = (old & ~andMask) | orMask;
		gpu.Write(reg, value);
		Log("gfx:   0x%04" B_PRIx32 " 0x%08" B_PRIx32 " -> 0x%08" B_PRIx32
			" (reads 0x%08" B_PRIx32 ")", reg, old, value, gpu.Read(reg));
	}
}


// CPU writes through the BAR go through the HDP; make them visible to the
// GPU (vi_flush_hdp) or drop stale HDP read data (vi_invalidate_hdp).
void
flush_hdp(Gpu& gpu)
{
	__sync_synchronize();
	gpu.Write(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
	gpu.Read(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
}


void
invalidate_hdp(Gpu& gpu)
{
	gpu.Write(mmHDP_DEBUG0, 1);
	gpu.Read(mmHDP_DEBUG0);
}


void
dump_state(Gpu& gpu, Ring& ring, const char* why)
{
	Log("gfx: --- state: %s ---", why);
	Log("gfx: GRBM_STATUS 0x%08" B_PRIx32 " GRBM_STATUS2 0x%08" B_PRIx32
		" GRBM_STATUS_SE0 0x%08" B_PRIx32, gpu.Read(mmGRBM_STATUS),
		gpu.Read(mmGRBM_STATUS2), gpu.Read(mmGRBM_STATUS_SE0));
	Log("gfx: SRBM_STATUS 0x%08" B_PRIx32 " SRBM_STATUS2 0x%08" B_PRIx32,
		gpu.Read(mmSRBM_STATUS), gpu.Read(mmSRBM_STATUS2));
	Log("gfx: CP_STAT 0x%08" B_PRIx32 " CP_BUSY_STAT 0x%08" B_PRIx32
		" CP_STALLED_STAT1 0x%08" B_PRIx32 " CP_CPF_STATUS 0x%08" B_PRIx32,
		gpu.Read(mmCP_STAT), gpu.Read(mmCP_BUSY_STAT),
		gpu.Read(mmCP_STALLED_STAT1), gpu.Read(mmCP_CPF_STATUS));
	uint32 wbRptr = ring.writeBack.cpu != NULL ? *wb(ring, kWbRptr) : 0;
	Log("gfx: CP_RB0_RPTR 0x%" B_PRIx32 " CP_RB0_WPTR 0x%" B_PRIx32
		" (ours 0x%" B_PRIx32 ", rptr write-back 0x%" B_PRIx32 ")",
		gpu.Read(mmCP_RB0_RPTR), gpu.Read(mmCP_RB0_WPTR), ring.wptr, wbRptr);
	Log("gfx: CP_RB0_CNTL 0x%08" B_PRIx32 " CP_RB0_BASE 0x%08" B_PRIx32
		" BASE_HI 0x%08" B_PRIx32 " CP_RB_VMID 0x%" B_PRIx32,
		gpu.Read(mmCP_RB0_CNTL), gpu.Read(mmCP_RB0_BASE),
		gpu.Read(mmCP_RB0_BASE_HI), gpu.Read(mmCP_RB_VMID));
	Log("gfx: CP_ME_CNTL 0x%08" B_PRIx32 " CP_MEC_CNTL 0x%08" B_PRIx32
		" CP_INT_STATUS 0x%08" B_PRIx32 " CP_INT_CNTL_RING0 0x%08" B_PRIx32,
		gpu.Read(mmCP_ME_CNTL), gpu.Read(mmCP_MEC_CNTL),
		gpu.Read(mmCP_INT_STATUS), gpu.Read(mmCP_INT_CNTL_RING0));
	// The header dumps return the most recent packet headers one by one.
	for (int i = 0; i < 4; i++) {
		Log("gfx: header dump %d: PFP 0x%08" B_PRIx32 " ME 0x%08" B_PRIx32
			" CE 0x%08" B_PRIx32, i, gpu.Read(mmCP_PFP_HEADER_DUMP),
			gpu.Read(mmCP_ME_HEADER_DUMP), gpu.Read(mmCP_CE_HEADER_DUMP));
	}
	Log("gfx: RLC_CNTL 0x%08" B_PRIx32 " RLC_STAT 0x%08" B_PRIx32
		" RLC_GPM_STAT 0x%08" B_PRIx32 " RLC_PG_CNTL 0x%08" B_PRIx32,
		gpu.Read(mmRLC_CNTL), gpu.Read(mmRLC_STAT), gpu.Read(mmRLC_GPM_STAT),
		gpu.Read(mmRLC_PG_CNTL));
	Log("gfx: RLC_CGCG_CGLS_CTRL 0x%08" B_PRIx32 " SCRATCH_REG0 0x%08"
		B_PRIx32, gpu.Read(mmRLC_CGCG_CGLS_CTRL), gpu.Read(mmSCRATCH_REG0));
	Log("gfx: VM_CONTEXT0_PROTECTION_FAULT_STATUS 0x%08" B_PRIx32
		" _ADDR 0x%08" B_PRIx32 " MC_VM_MX_L1_TLB_CNTL 0x%08" B_PRIx32,
		gpu.Read(mmVM_CONTEXT0_PROTECTION_FAULT_STATUS),
		gpu.Read(mmVM_CONTEXT0_PROTECTION_FAULT_ADDR),
		gpu.Read(mmMC_VM_MX_L1_TLB_CNTL));
}


// Polls a dword the GPU writes to memory.
static status_t
wait_memory(Gpu& gpu, volatile uint32* address, uint32 value,
	bigtime_t timeout = 1000000)
{
	bigtime_t end = system_time() + timeout;
	while (true) {
		invalidate_hdp(gpu);
		if (*address == value)
			return B_OK;
		if (system_time() > end)
			return B_TIMED_OUT;
		snooze(10);
	}
}


// Waits until `count` dwords (plus the commit padding) fit in the ring.
status_t
ring_alloc(Gpu& gpu, Ring& ring, uint32 count)
{
	uint32 size = ring.buffer.size / 4;
	uint32 need = count + kRingAlignMask + 1;
	if (need >= size)
		return B_BAD_VALUE;

	bigtime_t end = system_time() + 1000000;
	while (true) {
		uint32 rptr = gpu.Read(mmCP_RB0_RPTR) & ring_mask(ring);
		uint32 used = (ring.wptr - rptr) & ring_mask(ring);
		if (size - 1 - used >= need)
			return B_OK;
		if (system_time() > end) {
			Log("gfx: ring full: rptr 0x%" B_PRIx32 " wptr 0x%" B_PRIx32
				", need %" B_PRIu32 " dwords", rptr, ring.wptr, count);
			return B_TIMED_OUT;
		}
		snooze(10);
	}
}


// Pads to the ring's alignment with NOPs (amdgpu_ring_commit), then hands
// the new wptr to the CP (gfx_v8_0_ring_set_wptr_gfx, without doorbell).
void
ring_commit(Gpu& gpu, Ring& ring)
{
	uint32 count = (kRingAlignMask + 1 - (ring.wptr & kRingAlignMask))
		& kRingAlignMask;
	for (uint32 i = 0; i < count; i++)
		ring_write(ring, kRingNop);

	// Keep the wptr poll dword in step too, in case the CP polls it.
	*wb(ring, kWbWptrPoll) = ring.wptr;
	flush_hdp(gpu);

	gpu.Write(mmCP_RB0_WPTR, ring.wptr);
	gpu.Read(mmCP_RB0_WPTR);
}


// Waits until the CP read everything up to our wptr.
static status_t
ring_wait_idle(Gpu& gpu, Ring& ring, bigtime_t timeout = 1000000)
{
	return gpu.Wait(mmCP_RB0_RPTR, ring_mask(ring), ring.wptr, timeout);
}


static uint32
csb_size()
{
	// gfx_v8_0_get_csb_size()
	uint32 count = 2 + 3;
	for (const cs_section_def* sect = vi_cs_data; sect->section != NULL;
			sect++) {
		for (const cs_extent_def* ext = sect->section; ext->extent != NULL;
				ext++) {
			if (sect->id != SECT_CONTEXT)
				return 0;
			count += 2 + ext->reg_count;
		}
	}
	return count + 4 + 2 + 2;
}


// The clear state packets, as gfx_v8_0_get_csb_buffer() builds them for the
// RLC and gfx_v8_0_cp_gfx_start() puts them in the ring. Returns the count.
static uint32
build_clear_state(uint32* buffer, uint32 rasterConfig, uint32 rasterConfig1)
{
	uint32 count = 0;
	buffer[count++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
	buffer[count++] = PACKET3_PREAMBLE_BEGIN_CLEAR_STATE;

	buffer[count++] = PACKET3(PACKET3_CONTEXT_CONTROL, 1);
	buffer[count++] = 0x80000000;
	buffer[count++] = 0x80000000;

	for (const cs_section_def* sect = vi_cs_data; sect->section != NULL;
			sect++) {
		for (const cs_extent_def* ext = sect->section; ext->extent != NULL;
				ext++) {
			if (sect->id != SECT_CONTEXT)
				continue;
			buffer[count++] = PACKET3(PACKET3_SET_CONTEXT_REG,
				ext->reg_count);
			buffer[count++] = ext->reg_index - PACKET3_SET_CONTEXT_REG_START;
			for (uint32 i = 0; i < ext->reg_count; i++)
				buffer[count++] = ext->extent[i];
		}
	}

	buffer[count++] = PACKET3(PACKET3_SET_CONTEXT_REG, 2);
	buffer[count++] = mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START;
	buffer[count++] = rasterConfig;
	buffer[count++] = rasterConfig1;

	buffer[count++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
	buffer[count++] = PACKET3_PREAMBLE_END_CLEAR_STATE;

	buffer[count++] = PACKET3(PACKET3_CLEAR_STATE, 0);
	buffer[count++] = 0;
	return count;
}


// gfx_v8_0_ring_emit_fence_gfx(): a dummy EOP with seq - 1 (a cache flush
// workaround), then the real one. 32-bit data, no interrupt.
static void
emit_fence(Ring& ring, uint64 address, uint32 sequence)
{
	for (int i = 0; i < 2; i++) {
		ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
		ring_write(ring, EOP_TCL1_ACTION_EN | EOP_TC_ACTION_EN
			| EOP_TC_WB_ACTION_EN | EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT)
			| EVENT_INDEX(5));
		ring_write(ring, (uint32)address & 0xfffffffc);
		ring_write(ring, ((uint32)(address >> 32) & 0xffff) | DATA_SEL(1)
			| INT_SEL(0));
		ring_write(ring, i == 0 ? sequence - 1 : sequence);
		ring_write(ring, 0);
	}
}


// Emits a fence, commits and waits for it.
status_t
fence_and_wait(Gpu& gpu, Ring& ring, const char* what)
{
	status_t status = ring_alloc(gpu, ring, 12);
	if (status != B_OK)
		return status;

	uint32 sequence = ++ring.fenceSequence;
	uint64 address = ring.writeBack.gpu + kWbFence * 4;
	emit_fence(ring, address, sequence);
	ring_commit(gpu, ring);

	bigtime_t start = system_time();
	status = wait_memory(gpu, wb(ring, kWbFence), sequence);
	if (status != B_OK) {
		Log("gfx: %s: fence %" B_PRIu32 " at 0x%" B_PRIx64 " timed out, "
			"memory has 0x%08" B_PRIx32, what, sequence, address,
			*wb(ring, kWbFence));
		return status;
	}
	Log("gfx: %s: fence %" B_PRIu32 " signalled after %" B_PRId64 " us",
		what, sequence, system_time() - start);
	return B_OK;
}


static void
select_se_sh(Gpu& gpu, uint32 se, uint32 sh)
{
	// gfx_v8_0_select_se_sh(), all instances
	uint32 value = GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK;
	if (se == 0xffffffff)
		value |= GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;
	else
		value = SET_FIELD(value, GRBM_GFX_INDEX, SE_INDEX, se);
	if (sh == 0xffffffff)
		value |= GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK;
	else
		value = SET_FIELD(value, GRBM_GFX_INDEX, SH_INDEX, sh);
	gpu.Write(mmGRBM_GFX_INDEX, value);
}


//	#pragma mark - StartGfx parts


// gfx_v8_0_constants_init(), the parts the gfx ring needs. Stoney has one
// SE with one SH and one RB, so the raster configs stay 0 (golden).
static void
constants_init(Gpu& gpu)
{
	WRITE_LOGGED(gpu, GRBM_CNTL, SET_FIELD(gpu.Read(mmGRBM_CNTL), GRBM_CNTL,
		READ_TIMEOUT, 0xff));

	// GB_ADDR_CONFIG as gfx_v8_0_gpu_early_init computes it for APUs:
	// CARRIZO_GB_ADDR_CONFIG_GOLDEN with ROW_SIZE from the DIMM address maps,
	// 2 KB if any DIMM maps 8 GB (map 11), else 1 KB. Only tiled surfaces
	// depend on it. Linux also writes it to HDP_ADDR_CONFIG and
	// DMIF_ADDR_CALC; those stay as radeon_hd left them, since the display
	// is running from them.
	uint32 maps[2] = { gpu.Read(kMcFusDram0BankAddrMapping),
		gpu.Read(kMcFusDram1BankAddrMapping) };
	uint32 rowSize = 0;		// 1 KB
	for (int i = 0; i < 2; i++) {
		if ((maps[i] & 0xf) == 11 || ((maps[i] >> 4) & 0xf) == 11)
			rowSize = 1;	// 2 KB
	}
	uint32 addrConfig = SET_FIELD(kCarrizoGbAddrConfigGolden, GB_ADDR_CONFIG,
		ROW_SIZE, rowSize);
	Log("gfx: MC_FUS_DRAM0/1_BANK_ADDR_MAPPING 0x%08" B_PRIx32 " 0x%08"
		B_PRIx32 ": row size %u KB", maps[0], maps[1], 1u << rowSize);
	WRITE_LOGGED(gpu, GB_ADDR_CONFIG, addrConfig);
	Log("gfx: HDP_ADDR_CONFIG 0x%08" B_PRIx32 " (not touched)",
		gpu.Read(mmHDP_ADDR_CONFIG));

	uint32 value = SET_FIELD(0, SH_STATIC_MEM_CONFIG, SWIZZLE_ENABLE, 1);
	value = SET_FIELD(value, SH_STATIC_MEM_CONFIG, ELEMENT_SIZE, 1);
	value = SET_FIELD(value, SH_STATIC_MEM_CONFIG, INDEX_STRIDE, 3);
	WRITE_LOGGED(gpu, SH_STATIC_MEM_CONFIG, value);

	// VMID 0 only (vi_srbm_select(0, 0, 0, 0))
	WRITE_LOGGED(gpu, SRBM_GFX_CNTL, 0);
	value = SET_FIELD(0, SH_MEM_CONFIG, DEFAULT_MTYPE, MTYPE_UC);
	value = SET_FIELD(value, SH_MEM_CONFIG, APE1_MTYPE, MTYPE_UC);
	value = SET_FIELD(value, SH_MEM_CONFIG, ALIGNMENT_MODE,
		SH_MEM_ALIGNMENT_MODE_UNALIGNED);
	WRITE_LOGGED(gpu, SH_MEM_CONFIG, value);
	WRITE_LOGGED(gpu, SH_MEM_BASES, 0);
	WRITE_LOGGED(gpu, SH_MEM_APE1_BASE, 1);
	WRITE_LOGGED(gpu, SH_MEM_APE1_LIMIT, 0);

	select_se_sh(gpu, 0xffffffff, 0xffffffff);

	value = (kScPrimFifoSizeFrontend
			<< PA_SC_FIFO_SIZE__SC_FRONTEND_PRIM_FIFO_SIZE__SHIFT)
		| (kScPrimFifoSizeBackend
			<< PA_SC_FIFO_SIZE__SC_BACKEND_PRIM_FIFO_SIZE__SHIFT)
		| (kScHizTileFifoSize << PA_SC_FIFO_SIZE__SC_HIZ_TILE_FIFO_SIZE__SHIFT)
		| (kScEarlyzTileFifoSize
			<< PA_SC_FIFO_SIZE__SC_EARLYZ_TILE_FIFO_SIZE__SHIFT);
	WRITE_LOGGED(gpu, PA_SC_FIFO_SIZE, value);

	value = gpu.Read(mmSPI_ARB_PRIORITY);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS0, 2);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS1, 2);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS2, 2);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS3, 2);
	WRITE_LOGGED(gpu, SPI_ARB_PRIORITY, value);
}


// Without GART or GPUVM, VMID 0 must reach the carve-out by MC address.
// Logs the MC state and, like gmc_v8_0_mc_program(), makes the system
// aperture cover the frame buffer if it doesn't. MC_VM_FB_LOCATION stays.
static status_t
check_aperture(Gpu& gpu, const Ring& ring)
{
	uint32 fbLocation = gpu.Read(mmMC_VM_FB_LOCATION);
	uint64 fbStart = (uint64)(fbLocation & 0xffff) << 24;
	uint64 fbEnd = ((uint64)(fbLocation >> 16) << 24) | 0xffffff;
	Log("gfx: MC_VM_FB_LOCATION 0x%08" B_PRIx32 ": 0x%" B_PRIx64 " - 0x%"
		B_PRIx64 ", Gpu::VramStart 0x%" B_PRIx64, fbLocation, fbStart, fbEnd,
		gpu.VramStart());
	Log("gfx: MC_VM_AGP_BASE 0x%" B_PRIx32 " BOT 0x%" B_PRIx32 " TOP 0x%"
		B_PRIx32, gpu.Read(mmMC_VM_AGP_BASE), gpu.Read(mmMC_VM_AGP_BOT),
		gpu.Read(mmMC_VM_AGP_TOP));
	Log("gfx: MC_VM_MX_L1_TLB_CNTL 0x%08" B_PRIx32 " VM_L2_CNTL 0x%08"
		B_PRIx32 " VM_CONTEXT0_CNTL 0x%08" B_PRIx32 " PT_START 0x%" B_PRIx32,
		gpu.Read(mmMC_VM_MX_L1_TLB_CNTL), gpu.Read(mmVM_L2_CNTL),
		gpu.Read(mmVM_CONTEXT0_CNTL),
		gpu.Read(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR));

	uint32 low = gpu.Read(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR);
	uint32 high = gpu.Read(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR);
	Log("gfx: MC_VM_SYSTEM_APERTURE LOW 0x%" B_PRIx32 " HIGH 0x%" B_PRIx32
		" DEFAULT 0x%" B_PRIx32 " (4 KB pages)", low, high,
		gpu.Read(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR));
	if ((uint64)low > (fbStart >> 12) || (uint64)high < (fbEnd >> 12)) {
		Log("gfx: system aperture does not cover the frame buffer, setting "
			"it as gmc_v8_0_mc_program does");
		WRITE_LOGGED(gpu, MC_VM_SYSTEM_APERTURE_LOW_ADDR,
			(uint32)(fbStart >> 12));
		WRITE_LOGGED(gpu, MC_VM_SYSTEM_APERTURE_HIGH_ADDR,
			(uint32)(fbEnd >> 12));
	}

	const GpuMemory* memory[] = { &ring.buffer, &ring.writeBack };
	for (size_t i = 0; i < 2; i++) {
		if (memory[i]->gpu < fbStart
			|| memory[i]->gpu + memory[i]->size - 1 > fbEnd) {
			Log("gfx: GPU memory 0x%" B_PRIx64 " (+0x%zx) is outside the "
				"frame buffer's MC range", memory[i]->gpu, memory[i]->size);
			return B_BAD_ADDRESS;
		}
	}
	return B_OK;
}


static void
enable_gui_idle_interrupt(Gpu& gpu, bool enable)
{
	uint32 value = gpu.Read(mmCP_INT_CNTL_RING0);
	value = SET_FIELD(value, CP_INT_CNTL_RING0, CNTX_BUSY_INT_ENABLE, enable);
	value = SET_FIELD(value, CP_INT_CNTL_RING0, CNTX_EMPTY_INT_ENABLE, enable);
	value = SET_FIELD(value, CP_INT_CNTL_RING0, CMP_BUSY_INT_ENABLE, enable);
	value = SET_FIELD(value, CP_INT_CNTL_RING0, GFX_IDLE_INT_ENABLE, enable);
	WRITE_LOGGED(gpu, CP_INT_CNTL_RING0, value);
}


// gfx_v8_0_wait_for_rlc_serdes(), for Stoney's single SE/SH.
static void
wait_for_rlc_serdes(Gpu& gpu)
{
	select_se_sh(gpu, 0, 0);
	if (gpu.Wait(mmRLC_SERDES_CU_MASTER_BUSY, 0xffffffff, 0) != B_OK) {
		Log("gfx: RLC serdes CU master still busy: 0x%08" B_PRIx32,
			gpu.Read(mmRLC_SERDES_CU_MASTER_BUSY));
	}
	select_se_sh(gpu, 0xffffffff, 0xffffffff);

	uint32 mask = RLC_SERDES_NONCU_MASTER_BUSY__SE_MASTER_BUSY_MASK
		| RLC_SERDES_NONCU_MASTER_BUSY__GC_MASTER_BUSY_MASK
		| RLC_SERDES_NONCU_MASTER_BUSY__TC0_MASTER_BUSY_MASK
		| RLC_SERDES_NONCU_MASTER_BUSY__TC1_MASTER_BUSY_MASK;
	if (gpu.Wait(mmRLC_SERDES_NONCU_MASTER_BUSY, mask, 0) != B_OK) {
		Log("gfx: RLC serdes non-CU master still busy: 0x%08" B_PRIx32,
			gpu.Read(mmRLC_SERDES_NONCU_MASTER_BUSY));
	}
}


// gfx_v8_0_rlc_resume() for an APU whose RLC firmware the SMU loaded:
// stop, reset, the clear state buffer from gfx_v8_0_init_pg(), start.
// The save/restore list, jump table restore and power gating setup of
// init_pg are left out: they only serve power gating, which stays off.
static status_t
rlc_resume(Gpu& gpu)
{
	Log("gfx: RLC before: RLC_CNTL 0x%08" B_PRIx32 " RLC_STAT 0x%08" B_PRIx32
		" RLC_GPM_STAT 0x%08" B_PRIx32 " RLC_PG_CNTL 0x%08" B_PRIx32,
		gpu.Read(mmRLC_CNTL), gpu.Read(mmRLC_STAT), gpu.Read(mmRLC_GPM_STAT),
		gpu.Read(mmRLC_PG_CNTL));

	// gfx_v8_0_rlc_stop()
	WRITE_LOGGED(gpu, RLC_CNTL, SET_FIELD(gpu.Read(mmRLC_CNTL), RLC_CNTL,
		RLC_ENABLE_F32, 0));
	enable_gui_idle_interrupt(gpu, false);
	wait_for_rlc_serdes(gpu);

	// gfx_v8_0_rlc_reset()
	gpu.Modify(mmGRBM_SOFT_RESET, GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK,
		GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK);
	gpu.Read(mmGRBM_SOFT_RESET);
	snooze(50);
	gpu.Modify(mmGRBM_SOFT_RESET, GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK, 0);
	gpu.Read(mmGRBM_SOFT_RESET);
	snooze(50);
	Log("gfx: RLC soft reset done, GRBM_SOFT_RESET 0x%08" B_PRIx32,
		gpu.Read(mmGRBM_SOFT_RESET));

	// Keep gfx and CP power gating off (Linux enables them later, from
	// set_powergating_state, after programming the save/restore list).
	uint32 pg = gpu.Read(mmRLC_PG_CNTL);
	pg &= ~(RLC_PG_CNTL__GFX_POWER_GATING_ENABLE_MASK
		| RLC_PG_CNTL__DYN_PER_CU_PG_ENABLE_MASK
		| RLC_PG_CNTL__STATIC_PER_CU_PG_ENABLE_MASK
		| RLC_PG_CNTL__GFX_PIPELINE_PG_ENABLE_MASK);
	pg |= RLC_PG_CNTL__CP_PG_DISABLE_MASK;
	WRITE_LOGGED(gpu, RLC_PG_CNTL, pg);

	// gfx_v8_0_init_csb()
	uint32 count = csb_size();
	GpuMemory csb;
	status_t status = gpu.Allocate(count * 4, csb);
	if (status != B_OK) {
		Log("gfx: no memory for the clear state buffer: %s", strerror(status));
		return status;
	}
	uint32 written = build_clear_state((uint32*)csb.cpu,
		gpu.Read(mmPA_SC_RASTER_CONFIG), gpu.Read(mmPA_SC_RASTER_CONFIG_1));
	flush_hdp(gpu);
	Log("gfx: clear state buffer at MC 0x%" B_PRIx64 ", %" B_PRIu32
		" dwords (%" B_PRIu32 " expected)", csb.gpu, written, count);
	WRITE_LOGGED(gpu, RLC_CSIB_ADDR_HI, (uint32)(csb.gpu >> 32));
	WRITE_LOGGED(gpu, RLC_CSIB_ADDR_LO, (uint32)csb.gpu & 0xfffffffc);
	WRITE_LOGGED(gpu, RLC_CSIB_LENGTH, written);

	// gfx_v8_0_rlc_start(); APUs leave the GUI idle interrupt off here.
	WRITE_LOGGED(gpu, RLC_CNTL, SET_FIELD(gpu.Read(mmRLC_CNTL), RLC_CNTL,
		RLC_ENABLE_F32, 1));
	snooze(50);

	Log("gfx: RLC after: RLC_CNTL 0x%08" B_PRIx32 " RLC_STAT 0x%08" B_PRIx32
		" RLC_GPM_STAT 0x%08" B_PRIx32, gpu.Read(mmRLC_CNTL),
		gpu.Read(mmRLC_STAT), gpu.Read(mmRLC_GPM_STAT));
	return B_OK;
}


static void
cp_gfx_enable(Gpu& gpu, bool enable)
{
	uint32 value = gpu.Read(mmCP_ME_CNTL);
	value = SET_FIELD(value, CP_ME_CNTL, ME_HALT, enable ? 0 : 1);
	value = SET_FIELD(value, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
	value = SET_FIELD(value, CP_ME_CNTL, CE_HALT, enable ? 0 : 1);
	WRITE_LOGGED(gpu, CP_ME_CNTL, value);
	snooze(50);
}


// gfx_v8_0_cp_gfx_start()
static status_t
cp_gfx_start(Gpu& gpu, Ring& ring)
{
	WRITE_LOGGED(gpu, CP_MAX_CONTEXT, kMaxHwContexts - 1);
	WRITE_LOGGED(gpu, CP_ENDIAN_SWAP, 0);
	WRITE_LOGGED(gpu, CP_DEVICE_ID, 1);

	cp_gfx_enable(gpu, true);

	uint32 count = csb_size();
	status_t status = ring_alloc(gpu, ring, count + 4);
	if (status != B_OK)
		return status;

	uint32* clearState = new(std::nothrow) uint32[count];
	if (clearState == NULL)
		return B_NO_MEMORY;
	uint32 written = build_clear_state(clearState,
		gpu.Read(mmPA_SC_RASTER_CONFIG), gpu.Read(mmPA_SC_RASTER_CONFIG_1));
	for (uint32 i = 0; i < written; i++)
		ring_write(ring, clearState[i]);
	delete[] clearState;

	// init the CE partitions
	ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	ring_write(ring, 0x8000);
	ring_write(ring, 0x8000);

	ring_commit(gpu, ring);
	Log("gfx: start packets committed, %" B_PRIu32 " clear state dwords, "
		"wptr 0x%" B_PRIx32, written, ring.wptr);

	status = ring_wait_idle(gpu, ring);
	if (status != B_OK) {
		dump_state(gpu, ring, "CP did not consume the start packets");
		return status;
	}
	Log("gfx: CP consumed the start packets, rptr 0x%" B_PRIx32
		" (write-back 0x%" B_PRIx32 ")", gpu.Read(mmCP_RB0_RPTR),
		*wb(ring, kWbRptr));
	return B_OK;
}


// gfx_v8_0_cp_gfx_resume(), without a doorbell.
static status_t
cp_gfx_resume(Gpu& gpu, Ring& ring)
{
	WRITE_LOGGED(gpu, CP_RB_WPTR_DELAY, 0);
	WRITE_LOGGED(gpu, CP_RB_VMID, 0);

	// order_base_2(ring_size / 8)
	uint32 bufferSize = 0;
	while ((1u << bufferSize) < ring.buffer.size / 8)
		bufferSize++;
	uint32 cntl = SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, bufferSize);
	cntl = SET_FIELD(cntl, CP_RB0_CNTL, RB_BLKSZ, bufferSize - 2);
	cntl = SET_FIELD(cntl, CP_RB0_CNTL, MTYPE, 3);
	cntl = SET_FIELD(cntl, CP_RB0_CNTL, MIN_IB_AVAILSZ, 1);
	WRITE_LOGGED(gpu, CP_RB0_CNTL, cntl);

	WRITE_LOGGED(gpu, CP_RB0_CNTL, cntl | CP_RB0_CNTL__RB_RPTR_WR_ENA_MASK);
	ring.wptr = 0;
	WRITE_LOGGED(gpu, CP_RB0_WPTR, 0);

	uint64 rptrAddress = ring.writeBack.gpu + kWbRptr * 4;
	WRITE_LOGGED(gpu, CP_RB0_RPTR_ADDR, (uint32)rptrAddress);
	WRITE_LOGGED(gpu, CP_RB0_RPTR_ADDR_HI, (uint32)(rptrAddress >> 32) & 0xff);

	uint64 wptrAddress = ring.writeBack.gpu + kWbWptrPoll * 4;
	WRITE_LOGGED(gpu, CP_RB_WPTR_POLL_ADDR_LO, (uint32)wptrAddress);
	WRITE_LOGGED(gpu, CP_RB_WPTR_POLL_ADDR_HI, (uint32)(wptrAddress >> 32));
	snooze(1000);
	WRITE_LOGGED(gpu, CP_RB0_CNTL, cntl);

	uint64 base = ring.buffer.gpu >> 8;
	WRITE_LOGGED(gpu, CP_RB0_BASE, (uint32)base);
	WRITE_LOGGED(gpu, CP_RB0_BASE_HI, (uint32)(base >> 32));

	// gfx_v8_0_set_cpg_door_bell() with use_doorbell false
	WRITE_LOGGED(gpu, CP_RB_DOORBELL_CONTROL,
		gpu.Read(mmCP_RB_DOORBELL_CONTROL)
			& ~CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK);
	Log("gfx: CP_RB_WPTR_POLL_CNTL 0x%08" B_PRIx32 " (not changed)",
		gpu.Read(mmCP_RB_WPTR_POLL_CNTL));

	// amdgpu_ring_clear_ring()
	volatile uint32* buffer = (volatile uint32*)ring.buffer.cpu;
	for (uint32 i = 0; i < ring.buffer.size / 4; i++)
		buffer[i] = kRingNop;
	flush_hdp(gpu);

	return cp_gfx_start(gpu, ring);
}


//	#pragma mark - steps


status_t
StartGfx(Gpu& gpu, Ring& ring)
{
	memset(&ring, 0, sizeof(ring));

	Log("gfx: start: GRBM_STATUS 0x%08" B_PRIx32 " CP_ME_CNTL 0x%08" B_PRIx32
		" CP_STAT 0x%08" B_PRIx32 " GB_ADDR_CONFIG 0x%08" B_PRIx32,
		gpu.Read(mmGRBM_STATUS), gpu.Read(mmCP_ME_CNTL), gpu.Read(mmCP_STAT),
		gpu.Read(mmGB_ADDR_CONFIG));

	status_t status = gpu.Allocate(kRingSize, ring.buffer);
	if (status == B_OK)
		status = gpu.Allocate(4096, ring.writeBack);
	if (status != B_OK) {
		Log("gfx: cannot allocate the ring: %s", strerror(status));
		return status;
	}
	memset(ring.writeBack.cpu, 0, ring.writeBack.size);
	Log("gfx: ring at MC 0x%" B_PRIx64 " (%zu bytes), write-back at MC 0x%"
		B_PRIx64, ring.buffer.gpu, ring.buffer.size, ring.writeBack.gpu);

	status = check_aperture(gpu, ring);
	if (status != B_OK)
		return status;

	// The CP must not run while its ring is set up.
	cp_gfx_enable(gpu, false);

	// gfx_v8_0_init_golden_registers(), CHIP_STONEY
	program_register_sequence(gpu, "stoney_mgcg_cgcg_init",
		kStoneyMgcgCgcgInit, B_COUNT_OF(kStoneyMgcgCgcgInit));
	program_register_sequence(gpu, "stoney_golden_settings_a11",
		kStoneyGoldenSettingsA11, B_COUNT_OF(kStoneyGoldenSettingsA11));
	program_register_sequence(gpu, "stoney_golden_common_all",
		kStoneyGoldenCommonAll, B_COUNT_OF(kStoneyGoldenCommonAll));

	constants_init(gpu);

	status = rlc_resume(gpu);
	if (status != B_OK) {
		dump_state(gpu, ring, "RLC resume failed");
		return status;
	}

	status = cp_gfx_resume(gpu, ring);
	if (status != B_OK)
		return status;

	Log("gfx: gfx ring 0 running");
	return B_OK;
}


status_t
TestRing(Gpu& gpu, Ring& ring)
{
	gpu.Write(mmSCRATCH_REG0, 0xcafedead);
	Log("gfx: ring test: SCRATCH_REG0 = 0x%08" B_PRIx32 " by MMIO",
		gpu.Read(mmSCRATCH_REG0));

	status_t status = ring_alloc(gpu, ring, 3);
	if (status != B_OK) {
		dump_state(gpu, ring, "ring test: no room in the ring");
		return status;
	}
	ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	ring_write(ring, mmSCRATCH_REG0 - PACKET3_SET_UCONFIG_REG_START);
	ring_write(ring, 0xdeadbeef);
	ring_commit(gpu, ring);

	bigtime_t start = system_time();
	status = gpu.Wait(mmSCRATCH_REG0, 0xffffffff, 0xdeadbeef);
	if (status != B_OK) {
		dump_state(gpu, ring, "ring test: SCRATCH_REG0 not written");
		return status;
	}
	Log("gfx: ring test passed after %" B_PRId64 " us",
		system_time() - start);
	return B_OK;
}


status_t
TestFence(Gpu& gpu, Ring& ring)
{
	volatile uint32* data = wb(ring, kWbTestData);
	*data = 0xcafedead;
	*wb(ring, kWbFence) = 0;
	flush_hdp(gpu);

	uint64 address = ring.writeBack.gpu + kWbTestData * 4;
	status_t status = ring_alloc(gpu, ring, 5);
	if (status != B_OK) {
		dump_state(gpu, ring, "fence test: no room in the ring");
		return status;
	}
	ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	ring_write(ring, WRITE_DATA_DST_SEL(5) | WR_CONFIRM);
	ring_write(ring, (uint32)address);
	ring_write(ring, (uint32)(address >> 32));
	ring_write(ring, 0xdeadbeef);
	ring_commit(gpu, ring);
	Log("gfx: fence test: WRITE_DATA 0xdeadbeef to MC 0x%" B_PRIx64, address);

	status = wait_memory(gpu, data, 0xdeadbeef);
	if (status != B_OK) {
		Log("gfx: fence test: WRITE_DATA not seen, memory has 0x%08" B_PRIx32,
			*data);
		dump_state(gpu, ring, "fence test: WRITE_DATA");
		return status;
	}
	Log("gfx: fence test: WRITE_DATA arrived");

	status = fence_and_wait(gpu, ring, "fence test");
	if (status != B_OK) {
		dump_state(gpu, ring, "fence test: EVENT_WRITE_EOP");
		return status;
	}
	return B_OK;
}


status_t
TestFill(Gpu& gpu, Ring& ring, uint32 x, uint32 y, uint32 width,
	uint32 height, uint32 color)
{
	// Assumes 32 bits per pixel; Gpu has no depth query.
	uint32 bytesPerRow = gpu.BytesPerRow();
	if (x >= gpu.Width() || y >= gpu.Height()) {
		Log("gfx: fill: (%" B_PRIu32 ", %" B_PRIu32 ") is off screen", x, y);
		return B_BAD_VALUE;
	}
	if (width > gpu.Width() - x)
		width = gpu.Width() - x;
	if (height > gpu.Height() - y)
		height = gpu.Height() - y;
	uint32 rowBytes = width * 4;
	if (width == 0 || height == 0 || rowBytes > 0x1fffff) {
		Log("gfx: fill: bad size %" B_PRIu32 "x%" B_PRIu32, width, height);
		return B_BAD_VALUE;
	}
	Log("gfx: fill: %" B_PRIu32 "x%" B_PRIu32 " at (%" B_PRIu32 ", %" B_PRIu32
		") with 0x%08" B_PRIx32 ", frame buffer MC 0x%" B_PRIx64 ", %" B_PRIu32
		" bytes per row", width, height, x, y, color, gpu.FrameBufferGpu(),
		bytesPerRow);

	// One DMA_DATA per row: source is the immediate data, destination is
	// memory through the MC (DST_SEL 0, DAS memory). CP_SYNC makes the ME
	// wait for each copy, so the fence after the last one means it landed.
	static const uint32 kRowsPerCommit = 128;
	for (uint32 row = 0; row < height; row++) {
		if (row % kRowsPerCommit == 0) {
			status_t status = ring_alloc(gpu, ring, kRowsPerCommit * 7);
			if (status != B_OK) {
				dump_state(gpu, ring, "fill: ring stuck");
				return status;
			}
		}
		uint64 destination = gpu.FrameBufferGpu()
			+ (uint64)(y + row) * bytesPerRow + x * 4;
		ring_write(ring, PACKET3(PACKET3_DMA_DATA, 5));
		ring_write(ring, PACKET3_DMA_DATA_ENGINE(0)
			| PACKET3_DMA_DATA_DST_SEL(0) | PACKET3_DMA_DATA_SRC_SEL(2)
			| PACKET3_DMA_DATA_CP_SYNC);
		ring_write(ring, color);
		ring_write(ring, 0);
		ring_write(ring, (uint32)destination);
		ring_write(ring, (uint32)(destination >> 32));
		ring_write(ring, rowBytes);
		if (row % kRowsPerCommit == kRowsPerCommit - 1 || row == height - 1)
			ring_commit(gpu, ring);
	}

	bigtime_t start = system_time();
	status_t status = fence_and_wait(gpu, ring, "fill");
	if (status != B_OK) {
		dump_state(gpu, ring, "fill: fence");
		return status;
	}
	Log("gfx: fill: %" B_PRIu32 " rows done in %" B_PRId64 " us (from the "
		"last commit)", height, system_time() - start);

	// Read back corners and the centre through the CPU.
	invalidate_hdp(gpu);
	const uint32 points[][2] = {
		{ 0, 0 }, { width - 1, 0 }, { 0, height - 1 },
		{ width - 1, height - 1 }, { width / 2, height / 2 }
	};
	int bad = 0;
	for (size_t i = 0; i < B_COUNT_OF(points); i++) {
		uint32 px = x + points[i][0];
		uint32 py = y + points[i][1];
		uint32 pixel = *(volatile uint32*)((uint8*)gpu.FrameBuffer()
			+ (size_t)py * bytesPerRow + px * 4);
		Log("gfx: fill: pixel (%" B_PRIu32 ", %" B_PRIu32 ") = 0x%08" B_PRIx32
			"%s", px, py, pixel, pixel == color ? "" : " (wrong)");
		if (pixel != color)
			bad++;
	}
	// And one just outside, which must be untouched (only logged).
	if (x + width < gpu.Width()) {
		uint32 pixel = *(volatile uint32*)((uint8*)gpu.FrameBuffer()
			+ (size_t)y * bytesPerRow + (x + width) * 4);
		Log("gfx: fill: pixel right of the rectangle = 0x%08" B_PRIx32, pixel);
	}

	if (bad != 0) {
		dump_state(gpu, ring, "fill: pixels wrong after the fence");
		return B_ERROR;
	}
	Log("gfx: fill passed");
	return B_OK;
}


status_t
TestCopy(Gpu& gpu, Ring& ring, uint32 srcX, uint32 srcY, uint32 dstX,
	uint32 dstY, uint32 width, uint32 height)
{
	// Assumes 32 bits per pixel, like TestFill.
	uint32 bytesPerRow = gpu.BytesPerRow();
	if (width == 0 || height == 0
		|| srcX + width > gpu.Width() || dstX + width > gpu.Width()
		|| srcY + height > gpu.Height() || dstY + height > gpu.Height()
		|| (srcY < dstY + height && dstY < srcY + height
			&& srcX < dstX + width && dstX < srcX + width)) {
		Log("gfx: copy: bad rectangle");
		return B_BAD_VALUE;
	}
	uint32 rowBytes = width * 4;
	Log("gfx: copy: %" B_PRIu32 "x%" B_PRIu32 " from (%" B_PRIu32 ", %"
		B_PRIu32 ") to (%" B_PRIu32 ", %" B_PRIu32 ")", width, height, srcX,
		srcY, dstX, dstY);

	// What app_server drew went in through the BAR; let the GPU see it.
	flush_hdp(gpu);

	// One DMA_DATA per row, memory to memory (SRC_SEL 0, DST_SEL 0).
	static const uint32 kRowsPerCommit = 128;
	for (uint32 row = 0; row < height; row++) {
		if (row % kRowsPerCommit == 0) {
			status_t status = ring_alloc(gpu, ring, kRowsPerCommit * 7);
			if (status != B_OK) {
				dump_state(gpu, ring, "copy: ring stuck");
				return status;
			}
		}
		uint64 source = gpu.FrameBufferGpu()
			+ (uint64)(srcY + row) * bytesPerRow + srcX * 4;
		uint64 destination = gpu.FrameBufferGpu()
			+ (uint64)(dstY + row) * bytesPerRow + dstX * 4;
		ring_write(ring, PACKET3(PACKET3_DMA_DATA, 5));
		ring_write(ring, PACKET3_DMA_DATA_ENGINE(0)
			| PACKET3_DMA_DATA_DST_SEL(0) | PACKET3_DMA_DATA_SRC_SEL(0)
			| PACKET3_DMA_DATA_CP_SYNC);
		ring_write(ring, (uint32)source);
		ring_write(ring, (uint32)(source >> 32));
		ring_write(ring, (uint32)destination);
		ring_write(ring, (uint32)(destination >> 32));
		ring_write(ring, rowBytes);
		if (row % kRowsPerCommit == kRowsPerCommit - 1 || row == height - 1)
			ring_commit(gpu, ring);
	}

	bigtime_t start = system_time();
	status_t status = fence_and_wait(gpu, ring, "copy");
	if (status != B_OK) {
		dump_state(gpu, ring, "copy: fence");
		return status;
	}
	Log("gfx: copy: %" B_PRIu32 " rows done in %" B_PRId64 " us (from the "
		"last commit)", height, system_time() - start);

	// Compare every row through the CPU.
	invalidate_hdp(gpu);
	uint8* frameBuffer = (uint8*)gpu.FrameBuffer();
	uint32 badRows = 0;
	uint32 firstBad = 0;
	for (uint32 row = 0; row < height; row++) {
		const volatile uint32* a = (const volatile uint32*)(frameBuffer
			+ (size_t)(srcY + row) * bytesPerRow + srcX * 4);
		const volatile uint32* b = (const volatile uint32*)(frameBuffer
			+ (size_t)(dstY + row) * bytesPerRow + dstX * 4);
		for (uint32 i = 0; i < width; i++) {
			if (a[i] != b[i]) {
				if (badRows++ == 0)
					firstBad = row;
				break;
			}
		}
	}
	if (badRows != 0) {
		Log("gfx: copy: %" B_PRIu32 " of %" B_PRIu32 " rows differ, first "
			"row %" B_PRIu32 " (the source may have been redrawn meanwhile)",
			badRows, height, firstBad);
		dump_state(gpu, ring, "copy: rows differ after the fence");
		return B_ERROR;
	}
	Log("gfx: copy passed, all %" B_PRIu32 " rows match", height);
	return B_OK;
}


}	// namespace gfx8
