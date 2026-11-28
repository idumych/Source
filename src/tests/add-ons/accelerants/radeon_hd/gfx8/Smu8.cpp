/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


// Loads the CP (ce, pfp, me, mec), RLC and SDMA microcode of a Stoney APU
// through the SMU. On Stoney, Linux's amdgpu_ucode_get_load_type() returns
// AMDGPU_FW_LOAD_SMU: the driver does not write the microcode to the engines
// itself, but copies each image into GPU memory, builds a table of contents
// (TOC) of tasks pointing at them, gives the SMU the TOC's address and asks
// it to run the jobs. The sequence follows Linux's smu8_smumgr.c (MIT,
// Advanced Micro Devices, Inc.), Stoney branches only, together with the
// image layout of amdgpu_ucode.c and amdgpu_cgs.c.
//
// Ported-from: Linux v6.12 drivers/gpu/drm/amd/pm/powerplay/smumgr/smu8_smumgr.c
// Ported-from-license: MIT


#include <new>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <OS.h>

#include "Steps.h"

#include "cz_ppsmc.h"
#include "gfx_8_0_d.h"
#include "gfx_8_0_sh_mask.h"
#include "oss_3_0_d.h"
#include "smu8.h"
#include "smu8_fusion.h"
#include "smu_8_0_d.h"
#include "smu_8_0_sh_mask.h"
#include "smu_ucode_xfer_cz.h"


namespace gfx8 {


// AMD_MAX_USEC_TIMEOUT, which powerplay uses as hwmgr->usec_timeout for both
// the message handshake and the load status check.
static const bigtime_t kSmuTimeout = 2000000;

static const size_t kPageSize = 4096;

// The SMU firmware header in the SMU's SRAM, seen through MP0PUB_IND_INDEX.
static const uint32 kSmuHeader = SMN_MP1_SRAM_START_ADDR
	+ SMU8_FIRMWARE_HEADER_LOCATION;
static const uint32 kSmuVersion = kSmuHeader
	+ offsetof(struct SMU8_Firmware_Header, Version);
static const uint32 kSmuImageSize = kSmuHeader
	+ offsetof(struct SMU8_Firmware_Header, ImageSize);
static const uint32 kSmuCodeSize = kSmuHeader
	+ offsetof(struct SMU8_Firmware_Header, CodeSize);
static const uint32 kSmuUcodeLoadStatus = kSmuHeader
	+ offsetof(struct SMU8_Firmware_Header, UcodeLoadStatus);

// What smu8_request_smu_load_fw() waits for, without SDMA1 and MEC_JT2,
// which Stoney does not have.
static const uint32 kFirmwareToCheck = UCODE_ID_RLC_G_MASK
	| UCODE_ID_SDMA0_MASK | UCODE_ID_CP_CE_MASK | UCODE_ID_CP_ME_MASK
	| UCODE_ID_CP_PFP_MASK | UCODE_ID_CP_MEC_JT1_MASK;


// From enum smu8_scratch_entry in Linux's smu8_smumgr.h.
enum scratch_entry {
	SCRATCH_UCODE_ID_SDMA0 = 0,
	SCRATCH_UCODE_ID_SDMA1,
	SCRATCH_UCODE_ID_CP_CE,
	SCRATCH_UCODE_ID_CP_PFP,
	SCRATCH_UCODE_ID_CP_ME,
	SCRATCH_UCODE_ID_CP_MEC_JT1,
	SCRATCH_UCODE_ID_CP_MEC_JT2,
	SCRATCH_UCODE_ID_GMCON_RENG,
	SCRATCH_UCODE_ID_RLC_G,
	SCRATCH_UCODE_ID_RLC_SCRATCH,
	SCRATCH_UCODE_ID_RLC_SRM_ARAM,
	SCRATCH_UCODE_ID_RLC_SRM_DRAM,
	SCRATCH_UCODE_ID_DMCU_ERAM,
	SCRATCH_UCODE_ID_DMCU_IRAM,
	SCRATCH_UCODE_ID_POWER_PROFILING,
	SCRATCH_DATA_ID_SDMA_HALT,
	SCRATCH_DATA_ID_SYS_CLOCKGATING,
	SCRATCH_DATA_ID_SDMA_RING_REGS,
	SCRATCH_DATA_ID_NONGFX_REINIT,
	SCRATCH_DATA_ID_SDMA_START,
	SCRATCH_DATA_ID_IH_REGISTERS,
	SCRATCH_SMU8_FUSION_CLKTABLE
};

// The firmware_list[] of smu8_smumgr.c, in its order.
static const scratch_entry kFirmwareList[] = {
	SCRATCH_UCODE_ID_SDMA0,
	SCRATCH_UCODE_ID_SDMA1,
	SCRATCH_UCODE_ID_CP_CE,
	SCRATCH_UCODE_ID_CP_PFP,
	SCRATCH_UCODE_ID_CP_ME,
	SCRATCH_UCODE_ID_CP_MEC_JT1,
	SCRATCH_UCODE_ID_CP_MEC_JT2,
	SCRATCH_UCODE_ID_RLC_G,
};

static const uint32 kMaxFirmware = B_COUNT_OF(kFirmwareList);
static const uint32 kMaxScratch = 5;
static const uint32 kMaxTasks = (kPageSize - sizeof(TOC)) / sizeof(SMU_Task);


// Like struct smu8_buffer_entry.
struct BufferEntry {
	scratch_entry	id;
	uint64			gpu;
	uint32			size;
};

// The state smu8_smumgr keeps in struct smu8_smumgr, for one load.
struct SmuState {
	uint8			toc[kPageSize];		// built here, then copied to tocMemory
	uint16			tocUsed;
	uint16			tocInitialize;
	uint16			tocPowerProfiling;
	uint16			tocAram;
	uint16			tocClockTable;
	bool			tocOverflow;

	GpuMemory		tocMemory;
	GpuMemory		smuMemory;
	uint32			smuUsed;

	BufferEntry		driver[kMaxFirmware];
	uint32			driverCount;
	BufferEntry		scratch[kMaxScratch];
	uint32			scratchCount;
};


// The firmware files, and where amdgpu_ucode_init_bo() puts their images.
enum {
	FW_CE = 0,
	FW_PFP,
	FW_ME,
	FW_MEC,
	FW_RLC,
	FW_SDMA,
	FW_COUNT
};

static const char* const kFirmwareFiles[FW_COUNT] = {
	"stoney_ce.bin",
	"stoney_pfp.bin",
	"stoney_me.bin",
	"stoney_mec.bin",
	"stoney_rlc.bin",
	"stoney_sdma.bin",
};

struct Image {
	Firmware		fw;
	GpuMemory		memory;
	bool			loaded;
};


static inline uint32
lower_32_bits(uint64 value)
{
	return (uint32)value;
}


static inline uint32
upper_32_bits(uint64 value)
{
	return (uint32)(value >> 32);
}


static inline uint32
align_up(uint32 value, uint32 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


static const char*
response_name(uint32 response)
{
	switch (response) {
		case PPSMC_Result_OK:
			return "OK";
		case PPSMC_Result_NoMore:
			return "NoMore";
		case PPSMC_Result_NotNow:
			return "NotNow";
		case PPSMC_Result_Failed:
			return "Failed";
		case PPSMC_Result_UnknownCmd:
			return "UnknownCmd";
		case PPSMC_Result_UnknownVT:
			return "UnknownVT";
		case 0:
			return "busy";
		default:
			return "?";
	}
}


static void
log_load_status(const char* prefix, uint32 status)
{
	static const char* const kNames[] = {
		"SDMA0", "SDMA1", "CE", "PFP", "ME", "MEC_JT1", "MEC_JT2",
		"GMCON_RENG", "RLC_G", "RLC_SCRATCH", "RLC_SRM_ARAM",
		"RLC_SRM_DRAM", "DMCU_ERAM", "DMCU_IRAM"
	};

	char loaded[160];
	char missing[160];
	loaded[0] = missing[0] = '\0';

	for (uint32 i = 0; i < B_COUNT_OF(kNames); i++) {
		uint32 bit = 1u << i;
		if ((status & bit) != 0) {
			strlcat(loaded, " ", sizeof(loaded));
			strlcat(loaded, kNames[i], sizeof(loaded));
		} else if ((kFirmwareToCheck & bit) != 0) {
			strlcat(missing, " ", sizeof(missing));
			strlcat(missing, kNames[i], sizeof(missing));
		}
	}

	Log("smu: %sUcodeLoadStatus 0x%08" B_PRIx32 " (loaded:%s; missing:%s)",
		prefix, status, loaded[0] != '\0' ? loaded : " none",
		missing[0] != '\0' ? missing : " none");
}


static void
dump_status(Gpu& gpu, const char* why)
{
	Log("smu: --- register dump (%s) ---", why);
	Log("smu: SRBM2P MSG_0 0x%08" B_PRIx32 " ARG_0 0x%08" B_PRIx32
		" RESP_0 0x%08" B_PRIx32, gpu.Read(mmSMU_MP1_SRBM2P_MSG_0),
		gpu.Read(mmSMU_MP1_SRBM2P_ARG_0), gpu.Read(mmSMU_MP1_SRBM2P_RESP_0));
	Log("smu: SMU firmware Version 0x%08" B_PRIx32 " ImageSize 0x%" B_PRIx32
		" CodeSize 0x%" B_PRIx32, gpu.ReadSmc(kSmuVersion),
		gpu.ReadSmc(kSmuImageSize), gpu.ReadSmc(kSmuCodeSize));
	log_load_status("", gpu.ReadSmc(kSmuUcodeLoadStatus));
	Log("smu: GRBM_STATUS 0x%08" B_PRIx32 " GRBM_STATUS2 0x%08" B_PRIx32
		" SRBM_STATUS 0x%08" B_PRIx32, gpu.Read(mmGRBM_STATUS),
		gpu.Read(mmGRBM_STATUS2), gpu.Read(mmSRBM_STATUS));
	Log("smu: CP_STAT 0x%08" B_PRIx32 " CP_ME_CNTL 0x%08" B_PRIx32
		" CP_MEC_CNTL 0x%08" B_PRIx32 " CP_CPC_STATUS 0x%08" B_PRIx32,
		gpu.Read(mmCP_STAT), gpu.Read(mmCP_ME_CNTL), gpu.Read(mmCP_MEC_CNTL),
		gpu.Read(mmCP_CPC_STATUS));
	Log("smu: CP_CPC_IC_BASE LO 0x%08" B_PRIx32 " HI 0x%08" B_PRIx32
		" CNTL 0x%08" B_PRIx32, gpu.Read(mmCP_CPC_IC_BASE_LO),
		gpu.Read(mmCP_CPC_IC_BASE_HI), gpu.Read(mmCP_CPC_IC_BASE_CNTL));
	Log("smu: RLC_CNTL 0x%08" B_PRIx32 " RLC_STAT 0x%08" B_PRIx32
		" RLC_GPM_STAT 0x%08" B_PRIx32, gpu.Read(mmRLC_CNTL),
		gpu.Read(mmRLC_STAT), gpu.Read(mmRLC_GPM_STAT));
	Log("smu: SDMA0_STATUS_REG 0x%08" B_PRIx32, gpu.Read(mmSDMA0_STATUS_REG));
	Log("smu: --- end of dump ---");
}


// PHM_WAIT_FIELD_UNEQUAL(SMU_MP1_SRBM2P_RESP_0, CONTENT, 0)
static status_t
wait_for_response(Gpu& gpu, uint32& response)
{
	bigtime_t deadline = system_time() + kSmuTimeout;
	do {
		response = gpu.Read(mmSMU_MP1_SRBM2P_RESP_0)
			& SMU_MP1_SRBM2P_RESP_0__CONTENT_MASK;
		if (response != 0)
			return B_OK;
	} while (system_time() < deadline);

	return B_TIMED_OUT;
}


// smu8_send_msg_to_smc_with_parameter(). Like Linux, only a timeout counts
// as failure; the response code is logged so the syslog shows refusals.
static status_t
send_message(Gpu& gpu, uint16 message, uint32 parameter, const char* name)
{
	uint32 response;
	if (wait_for_response(gpu, response) != B_OK) {
		Log("smu: %s (0x%04x) not sent: the SMU is still servicing message "
			"0x%04" B_PRIx32, name, message,
			gpu.Read(mmSMU_MP1_SRBM2P_MSG_0));
		return B_TIMED_OUT;
	}

	bigtime_t start = system_time();

	gpu.Write(mmSMU_MP1_SRBM2P_ARG_0, parameter);
	gpu.Write(mmSMU_MP1_SRBM2P_RESP_0, 0);
	gpu.Write(mmSMU_MP1_SRBM2P_MSG_0, message);

	status_t status = wait_for_response(gpu, response);
	bigtime_t elapsed = system_time() - start;

	if (status != B_OK) {
		Log("smu: %s (0x%04x, 0x%08" B_PRIx32 ") timed out after %" B_PRId64
			" us", name, message, parameter, elapsed);
		return status;
	}

	Log("smu: %s (0x%04x, 0x%08" B_PRIx32 ") -> 0x%02" B_PRIx32 " %s in %"
		B_PRId64 " us", name, message, parameter, response,
		response_name(response), elapsed);
	if (response != PPSMC_Result_OK)
		Log("smu: warning: %s was not answered with OK", name);

	return B_OK;
}


// smu8_check_fw_load_finish(), logging every change of the status word.
static status_t
check_firmware_load_finish(Gpu& gpu, uint32 firmware)
{
	bigtime_t start = system_time();
	bigtime_t deadline = start + kSmuTimeout;
	uint32 last = gpu.ReadSmc(kSmuUcodeLoadStatus);
	log_load_status("at start: ", last);

	while ((last & firmware) != firmware) {
		if (system_time() >= deadline) {
			Log("smu: firmware load not finished after %" B_PRId64 " us: "
				"want 0x%08" B_PRIx32 ", have 0x%08" B_PRIx32, kSmuTimeout,
				firmware, last);
			return B_TIMED_OUT;
		}

		uint32 status = gpu.ReadSmc(kSmuUcodeLoadStatus);
		if (status != last) {
			char prefix[48];
			snprintf(prefix, sizeof(prefix), "after %" B_PRId64 " us: ",
				system_time() - start);
			log_load_status(prefix, status);
			last = status;
		}
	}

	Log("smu: firmware load finished in %" B_PRId64 " us",
		system_time() - start);
	return B_OK;
}


// smu8_translate_firmware_enum_to_arg() for CHIP_STONEY.
static uint8
translate_firmware_enum_to_arg(scratch_entry id)
{
	switch (id) {
		case SCRATCH_UCODE_ID_SDMA0:
		case SCRATCH_UCODE_ID_SDMA1:		// Stoney has only SDMA0
			return UCODE_ID_SDMA0;
		case SCRATCH_UCODE_ID_CP_CE:
			return UCODE_ID_CP_CE;
		case SCRATCH_UCODE_ID_CP_PFP:
			return UCODE_ID_CP_PFP;
		case SCRATCH_UCODE_ID_CP_ME:
			return UCODE_ID_CP_ME;
		case SCRATCH_UCODE_ID_CP_MEC_JT1:
		case SCRATCH_UCODE_ID_CP_MEC_JT2:	// Stoney has only JT1
			return UCODE_ID_CP_MEC_JT1;
		case SCRATCH_UCODE_ID_GMCON_RENG:
			return UCODE_ID_GMCON_RENG;
		case SCRATCH_UCODE_ID_RLC_G:
			return UCODE_ID_RLC_G;
		case SCRATCH_UCODE_ID_RLC_SCRATCH:
			return UCODE_ID_RLC_SCRATCH;
		case SCRATCH_UCODE_ID_RLC_SRM_ARAM:
			return UCODE_ID_RLC_SRM_ARAM;
		case SCRATCH_UCODE_ID_RLC_SRM_DRAM:
			return UCODE_ID_RLC_SRM_DRAM;
		case SCRATCH_UCODE_ID_DMCU_ERAM:
			return UCODE_ID_DMCU_ERAM;
		case SCRATCH_UCODE_ID_DMCU_IRAM:
			return UCODE_ID_DMCU_IRAM;
		case SCRATCH_UCODE_ID_POWER_PROFILING:
			return TASK_ARG_INIT_MM_PWR_LOG;
		case SCRATCH_DATA_ID_SDMA_HALT:
		case SCRATCH_DATA_ID_SYS_CLOCKGATING:
		case SCRATCH_DATA_ID_SDMA_RING_REGS:
		case SCRATCH_DATA_ID_NONGFX_REINIT:
		case SCRATCH_DATA_ID_SDMA_START:
		case SCRATCH_DATA_ID_IH_REGISTERS:
			return TASK_ARG_REG_MMIO;
		case SCRATCH_SMU8_FUSION_CLKTABLE:
			return TASK_ARG_INIT_CLK_TABLE;
	}

	return 0;
}


// What amdgpu_cgs_get_firmware_info() returns for a UCODE_ID: the image's
// MC address and size. For MEC_JT1 that is the jump table, which
// amdgpu_ucode_patch_jt() put on the page after the MEC ucode.
static status_t
get_firmware_info(const Image* images, uint8 ucodeId, uint64& gpu,
	uint32& size)
{
	const Image* image;
	switch (ucodeId) {
		case UCODE_ID_SDMA0:
			image = &images[FW_SDMA];
			break;
		case UCODE_ID_CP_CE:
			image = &images[FW_CE];
			break;
		case UCODE_ID_CP_PFP:
			image = &images[FW_PFP];
			break;
		case UCODE_ID_CP_ME:
			image = &images[FW_ME];
			break;
		case UCODE_ID_CP_MEC_JT1:
			image = &images[FW_MEC];
			gpu = image->memory.gpu
				+ align_up(image->fw.ucodeSize, kPageSize);
			size = image->fw.jtSize * 4;
			return B_OK;
		case UCODE_ID_RLC_G:
			image = &images[FW_RLC];
			break;
		default:
			return B_ENTRY_NOT_FOUND;
	}

	gpu = image->memory.gpu;
	size = image->fw.ucodeSize;
	return B_OK;
}


// smu8_smu_populate_firmware_entries()
static void
populate_firmware_entries(SmuState& state, const Image* images)
{
	state.driverCount = 0;

	for (uint32 i = 0; i < kMaxFirmware; i++) {
		uint8 type = translate_firmware_enum_to_arg(kFirmwareList[i]);
		uint64 gpu;
		uint32 size;
		if (get_firmware_info(images, type, gpu, size) != B_OK)
			continue;

		BufferEntry& entry = state.driver[state.driverCount++];
		entry.id = kFirmwareList[i];
		entry.gpu = gpu;
		entry.size = size;
	}
}


// smu8_smu_populate_single_scratch_entry()
static void
populate_scratch_entry(SmuState& state, scratch_entry id, uint32 size)
{
	BufferEntry& entry = state.scratch[state.scratchCount++];
	entry.id = id;
	entry.gpu = state.smuMemory.gpu + state.smuUsed;
	entry.size = size;

	state.smuUsed += align_up(size, 32);
}


static SMU_Task*
next_task(SmuState& state, bool isLast)
{
	if (state.tocUsed >= kMaxTasks) {
		state.tocOverflow = true;
		return NULL;
	}

	TOC* toc = (TOC*)state.toc;
	SMU_Task* task = &toc->tasks[state.tocUsed++];
	task->next = isLast ? END_OF_TASK_LIST : state.tocUsed;
	return task;
}


// smu8_smu_populate_single_scratch_task(). The IH register restore case is
// left out: Linux never adds that entry either.
static status_t
populate_scratch_task(SmuState& state, scratch_entry id, uint8 type,
	bool isLast)
{
	SMU_Task* task = next_task(state, isLast);
	if (task == NULL)
		return B_NO_MEMORY;

	task->type = type;
	task->arg = translate_firmware_enum_to_arg(id);

	for (uint32 i = 0; i < state.scratchCount; i++) {
		if (state.scratch[i].id == id) {
			task->addr.low = lower_32_bits(state.scratch[i].gpu);
			task->addr.high = upper_32_bits(state.scratch[i].gpu);
			task->size_bytes = state.scratch[i].size;
			return B_OK;
		}
	}

	Log("smu: no scratch buffer for entry %d", id);
	return B_ENTRY_NOT_FOUND;
}


// smu8_smu_populate_single_ucode_load_task()
static status_t
populate_ucode_load_task(SmuState& state, scratch_entry id, bool isLast)
{
	SMU_Task* task = next_task(state, isLast);
	if (task == NULL)
		return B_NO_MEMORY;

	task->type = TASK_TYPE_UCODE_LOAD;
	task->arg = translate_firmware_enum_to_arg(id);

	for (uint32 i = 0; i < state.driverCount; i++) {
		if (state.driver[i].id == id) {
			task->addr.low = lower_32_bits(state.driver[i].gpu);
			task->addr.high = upper_32_bits(state.driver[i].gpu);
			task->size_bytes = state.driver[i].size;
			return B_OK;
		}
	}

	Log("smu: no firmware image for entry %d", id);
	return B_ENTRY_NOT_FOUND;
}


// smu8_smu_construct_toc() and the construct_toc_for_* functions it calls,
// CHIP_STONEY branches.
static status_t
construct_toc(SmuState& state)
{
	TOC* toc = (TOC*)state.toc;
	memset(state.toc, 0, sizeof(state.toc));
	state.tocUsed = 0;
	state.tocOverflow = false;

	status_t status = B_OK;
	#define CHECK(x) \
		do { status_t _s = (x); if (_s != B_OK && status == B_OK) \
			status = _s; } while (false)

	// smu8_smu_initialize_toc_empty_job_list()
	for (int i = 0; i < NUM_JOBLIST_ENTRIES; i++)
		toc->JobList[i] = (uint8)IGNORE_JOB;

	// smu8_smu_construct_toc_for_rlc_aram_save()
	state.tocAram = state.tocUsed;
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_RLC_SRM_ARAM,
		TASK_TYPE_UCODE_SAVE, true));

	// smu8_smu_construct_toc_for_vddgfx_enter()
	toc->JobList[JOB_GFX_SAVE] = (uint8)state.tocUsed;
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_RLC_SCRATCH,
		TASK_TYPE_UCODE_SAVE, false));
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_RLC_SRM_DRAM,
		TASK_TYPE_UCODE_SAVE, true));

	// smu8_smu_construct_toc_for_vddgfx_exit(); Stoney lists MEC_JT1 twice
	// where other chips list JT1 and JT2.
	toc->JobList[JOB_GFX_RESTORE] = (uint8)state.tocUsed;
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_CE, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_PFP, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_ME, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_MEC_JT1,
		false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_MEC_JT1,
		false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_RLC_G, false));
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_RLC_SCRATCH,
		TASK_TYPE_UCODE_LOAD, false));
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_RLC_SRM_ARAM,
		TASK_TYPE_UCODE_LOAD, false));
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_RLC_SRM_DRAM,
		TASK_TYPE_UCODE_LOAD, true));

	// smu8_smu_construct_toc_for_power_profiling()
	state.tocPowerProfiling = state.tocUsed;
	CHECK(populate_scratch_task(state, SCRATCH_UCODE_ID_POWER_PROFILING,
		TASK_TYPE_INITIALIZE, true));

	// smu8_smu_construct_toc_for_bootup(), without SDMA1 and MEC_JT2
	state.tocInitialize = state.tocUsed;
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_SDMA0, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_CE, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_PFP, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_ME, false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_CP_MEC_JT1,
		false));
	CHECK(populate_ucode_load_task(state, SCRATCH_UCODE_ID_RLC_G, true));

	// smu8_smu_construct_toc_for_clock_table()
	state.tocClockTable = state.tocUsed;
	CHECK(populate_scratch_task(state, SCRATCH_SMU8_FUSION_CLKTABLE,
		TASK_TYPE_INITIALIZE, true));

	#undef CHECK

	if (state.tocOverflow) {
		Log("smu: TOC overflow (%u tasks max)", (unsigned)kMaxTasks);
		return B_NO_MEMORY;
	}
	return status;
}


static void
log_toc(const SmuState& state)
{
	const TOC* toc = (const TOC*)state.toc;

	Log("smu: TOC at MC 0x%" B_PRIx64 ", %u tasks; jobs: aram %u, "
		"power profiling %u, initialize %u, clock table %u, GFX_SAVE %u, "
		"GFX_RESTORE %u", state.tocMemory.gpu, state.tocUsed, state.tocAram,
		state.tocPowerProfiling, state.tocInitialize, state.tocClockTable,
		toc->JobList[JOB_GFX_SAVE], toc->JobList[JOB_GFX_RESTORE]);

	for (uint32 i = 0; i < state.tocUsed; i++) {
		const SMU_Task& task = toc->tasks[i];
		Log("smu:   task %2" B_PRIu32 ": type %u arg %2u next %5u addr "
			"0x%08" B_PRIx32 "%08" B_PRIx32 " size %" B_PRIu32, i, task.type,
			task.arg, task.next, task.addr.high, task.addr.low,
			task.size_bytes);
	}
}


// amdgpu_ucode_init_single_fw() for AMDGPU_FW_LOAD_SMU: the ucode without
// its header, at the start of a page; for MEC also amdgpu_ucode_patch_jt(),
// which copies the jump table to the next page boundary after the ucode.
static status_t
load_image(Gpu& gpu, const char* name, bool hasJumpTable, Image& image)
{
	status_t status = gpu.LoadFirmware(name, image.fw);
	if (status != B_OK) {
		Log("smu: cannot load firmware %s: %s", name, strerror(status));
		return status;
	}
	image.loaded = true;

	const Firmware& fw = image.fw;
	Log("smu: %s: %zu bytes, header v%" B_PRIu32 ".%" B_PRIu32 ", ucode "
		"version 0x%" B_PRIx32 ", ucode at %" B_PRIu32 " size %" B_PRIu32
		", jump table at dword %" B_PRIu32 " size %" B_PRIu32 " dwords",
		name, fw.size, fw.headerMajor, fw.headerMinor, fw.ucodeVersion,
		fw.ucodeOffset, fw.ucodeSize, fw.jtOffset, fw.jtSize);

	if (fw.ucodeSize == 0 || fw.ucodeOffset > fw.size
		|| fw.ucodeSize > fw.size - fw.ucodeOffset) {
		Log("smu: %s: ucode lies outside the file", name);
		return B_BAD_DATA;
	}

	size_t total = align_up(fw.ucodeSize, kPageSize);
	uint64 jtStart = 0;
	uint32 jtBytes = 0;
	if (hasJumpTable) {
		jtStart = (uint64)fw.ucodeOffset + (uint64)fw.jtOffset * 4;
		jtBytes = fw.jtSize * 4;
		if (jtBytes == 0 || jtStart + jtBytes > fw.size) {
			Log("smu: %s: jump table missing or outside the file", name);
			return B_BAD_DATA;
		}
		total += align_up(jtBytes, kPageSize);
	}

	status = gpu.Allocate(total, image.memory, kPageSize);
	if (status != B_OK) {
		Log("smu: %s: cannot allocate %zu bytes of GPU memory: %s", name,
			total, strerror(status));
		return status;
	}

	memset(image.memory.cpu, 0, image.memory.size);
	memcpy(image.memory.cpu, fw.data + fw.ucodeOffset, fw.ucodeSize);
	if (hasJumpTable) {
		memcpy((uint8*)image.memory.cpu + align_up(fw.ucodeSize, kPageSize),
			fw.data + jtStart, jtBytes);
	}

	Log("smu: %s: copied to MC 0x%" B_PRIx64 " (%zu bytes)", name,
		image.memory.gpu, total);
	return B_OK;
}


// smu8_load_mec_firmware(): the SMU loads only the MEC jump table; the MEC
// fetches its ucode itself from CP_CPC_IC_BASE.
static void
load_mec_firmware(Gpu& gpu, const Image& mec)
{
	uint32 value = gpu.Read(mmCP_MEC_CNTL);
	value |= CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK;
	gpu.Write(mmCP_MEC_CNTL, value);

	value = gpu.Read(mmCP_CPC_IC_BASE_CNTL);
	value &= ~(CP_CPC_IC_BASE_CNTL__VMID_MASK | CP_CPC_IC_BASE_CNTL__ATC_MASK
		| CP_CPC_IC_BASE_CNTL__CACHE_POLICY_MASK
		| CP_CPC_IC_BASE_CNTL__MTYPE_MASK);
	value |= (1u << CP_CPC_IC_BASE_CNTL__MTYPE__SHIFT)
		& CP_CPC_IC_BASE_CNTL__MTYPE_MASK;
	gpu.Write(mmCP_CPC_IC_BASE_CNTL, value);

	gpu.Write(mmCP_CPC_IC_BASE_LO,
		lower_32_bits(mec.memory.gpu) & CP_CPC_IC_BASE_LO__IC_BASE_LO_MASK);
	gpu.Write(mmCP_CPC_IC_BASE_HI,
		upper_32_bits(mec.memory.gpu) & CP_CPC_IC_BASE_HI__IC_BASE_HI_MASK);

	Log("smu: MEC halted, CP_CPC_IC_BASE = 0x%04" B_PRIx32 "%08" B_PRIx32
		" (CNTL 0x%08" B_PRIx32 ", MEC_CNTL 0x%08" B_PRIx32 ")",
		gpu.Read(mmCP_CPC_IC_BASE_HI), gpu.Read(mmCP_CPC_IC_BASE_LO),
		gpu.Read(mmCP_CPC_IC_BASE_CNTL), gpu.Read(mmCP_MEC_CNTL));
}


// smu8_smu_init(): the TOC page and the SMU's scratch buffers.
static status_t
init_buffers(Gpu& gpu, SmuState& state)
{
	status_t status = gpu.Allocate(kPageSize, state.tocMemory, kPageSize);
	if (status != B_OK) {
		Log("smu: cannot allocate the TOC: %s", strerror(status));
		return status;
	}

	size_t smuSize = align_up(UCODE_ID_RLC_SCRATCH_SIZE_BYTE, 32)
		+ align_up(UCODE_ID_RLC_SRM_ARAM_SIZE_BYTE, 32)
		+ align_up(UCODE_ID_RLC_SRM_DRAM_SIZE_BYTE, 32)
		+ align_up(sizeof(struct SMU8_MultimediaPowerLogData), 32)
		+ align_up(sizeof(struct SMU8_Fusion_ClkTable), 32);
	status = gpu.Allocate(smuSize, state.smuMemory, kPageSize);
	if (status != B_OK) {
		Log("smu: cannot allocate %zu bytes of SMU scratch: %s", smuSize,
			strerror(status));
		return status;
	}
	memset(state.smuMemory.cpu, 0, state.smuMemory.size);

	state.smuUsed = 0;
	state.scratchCount = 0;
	populate_scratch_entry(state, SCRATCH_UCODE_ID_RLC_SCRATCH,
		UCODE_ID_RLC_SCRATCH_SIZE_BYTE);
	populate_scratch_entry(state, SCRATCH_UCODE_ID_RLC_SRM_ARAM,
		UCODE_ID_RLC_SRM_ARAM_SIZE_BYTE);
	populate_scratch_entry(state, SCRATCH_UCODE_ID_RLC_SRM_DRAM,
		UCODE_ID_RLC_SRM_DRAM_SIZE_BYTE);
	populate_scratch_entry(state, SCRATCH_UCODE_ID_POWER_PROFILING,
		sizeof(struct SMU8_MultimediaPowerLogData));
	populate_scratch_entry(state, SCRATCH_SMU8_FUSION_CLKTABLE,
		sizeof(struct SMU8_Fusion_ClkTable));

	Log("smu: TOC page at MC 0x%" B_PRIx64 ", SMU scratch at MC 0x%"
		B_PRIx64 " (%zu bytes)", state.tocMemory.gpu, state.smuMemory.gpu,
		smuSize);
	return B_OK;
}


// smu8_start_smu() and smu8_request_smu_load_fw()
static status_t
load_firmware(Gpu& gpu, SmuState& state, Image* images)
{
	uint32 version = gpu.ReadSmc(kSmuVersion);
	Log("smu: SMU firmware version %02" B_PRIu32 ".%02" B_PRIu32 ".%02"
		B_PRIu32 " (0x%08" B_PRIx32 ")", (version >> 16) & 0xff,
		(version >> 8) & 0xff, version & 0xff, version);
	Log("smu: before: RESP_0 0x%08" B_PRIx32 " MSG_0 0x%08" B_PRIx32,
		gpu.Read(mmSMU_MP1_SRBM2P_RESP_0), gpu.Read(mmSMU_MP1_SRBM2P_MSG_0));
	log_load_status("before: ", gpu.ReadSmc(kSmuUcodeLoadStatus));
	if (version == 0 || version == 0xffffffff)
		Log("smu: warning: the SMU firmware header does not look sane");

	// amdgpu_ucode_init_bo()
	for (int i = 0; i < FW_COUNT; i++) {
		status_t status = load_image(gpu, kFirmwareFiles[i], i == FW_MEC,
			images[i]);
		if (status != B_OK)
			return status;
	}

	status_t status = init_buffers(gpu, state);
	if (status != B_OK)
		return status;

	populate_firmware_entries(state, images);

	status = construct_toc(state);
	log_toc(state);
	if (status != B_OK)
		return status;

	// The TOC memory is write-combined: copy, then fence before the SMU
	// gets its address, and check that it arrived.
	memcpy(state.tocMemory.cpu, state.toc, sizeof(state.toc));
	__sync_synchronize();
	if (memcmp(state.tocMemory.cpu, state.toc, sizeof(state.toc)) != 0) {
		Log("smu: the TOC reads back differently from what was written");
		return B_IO_ERROR;
	}

	gpu.WriteSmc(kSmuUcodeLoadStatus, 0);
	Log("smu: UcodeLoadStatus cleared, reads 0x%08" B_PRIx32,
		gpu.ReadSmc(kSmuUcodeLoadStatus));

	struct {
		uint16		message;
		uint32		parameter;
		const char*	name;
	} messages[] = {
		{ PPSMC_MSG_DriverDramAddrHi, upper_32_bits(state.tocMemory.gpu),
			"DriverDramAddrHi" },
		{ PPSMC_MSG_DriverDramAddrLo, lower_32_bits(state.tocMemory.gpu),
			"DriverDramAddrLo" },
		{ PPSMC_MSG_InitJobs, 0, "InitJobs" },
		{ PPSMC_MSG_ExecuteJob, state.tocAram, "ExecuteJob(aram save)" },
		{ PPSMC_MSG_ExecuteJob, state.tocPowerProfiling,
			"ExecuteJob(power profiling)" },
		{ PPSMC_MSG_ExecuteJob, state.tocInitialize,
			"ExecuteJob(initialize)" },
	};

	for (uint32 i = 0; i < B_COUNT_OF(messages); i++) {
		status = send_message(gpu, messages[i].message, messages[i].parameter,
			messages[i].name);
		if (status != B_OK) {
			dump_status(gpu, messages[i].name);
			return status;
		}
	}

	status = check_firmware_load_finish(gpu, kFirmwareToCheck);
	if (status != B_OK) {
		dump_status(gpu, "firmware load check");
		return status;
	}

	load_mec_firmware(gpu, images[FW_MEC]);
	return B_OK;
}


status_t
LoadFirmwareThroughSmu(Gpu& gpu)
{
	Log("smu: loading CP, RLC and SDMA firmware through the SMU");

	SmuState* state = new(std::nothrow) SmuState;
	if (state == NULL)
		return B_NO_MEMORY;
	memset(state, 0, sizeof(*state));

	Image images[FW_COUNT];
	memset(images, 0, sizeof(images));

	status_t status = load_firmware(gpu, *state, images);

	for (int i = 0; i < FW_COUNT; i++) {
		if (images[i].loaded)
			Gpu::FreeFirmware(images[i].fw);
	}
	delete state;

	if (status == B_OK)
		Log("smu: firmware loaded");
	else
		Log("smu: firmware load failed: %s", strerror(status));
	return status;
}


}	// namespace gfx8
