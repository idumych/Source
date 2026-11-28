/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */

// Hang diagnosis for the 3D engine of Stoney Ridge (GFX8). Which registers
// tell where a draw is stuck follows Linux's gfx_v8_0_check_soft_reset() and
// amdgpu's hang dumps; the wave reads follow wave_read_ind() and
// gfx_v8_0_read_wave_data(), the VM fault decoding gmc_v8_0_vm_decode_fault().
// Field names and masks come from AMD's headers in amd/.
//
// Ported-from: Linux v6.12 drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c
// Ported-from-license: MIT


#include "Diagnose.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "gfx_8_0_d.h"
#include "gfx_8_0_sh_mask.h"
#include "gmc_8_1_d.h"
#include "gmc_8_1_sh_mask.h"
#include "oss_3_0_d.h"
#include "oss_3_0_sh_mask.h"


namespace gfx8 {


struct Field {
	uint32		mask;
	const char*	name;
	bool		always;		// a count: logged even when it is 0
};

#define FLAG(reg, field)	{ reg##__##field##_MASK, #field, false }
#define COUNT(reg, field)	{ reg##__##field##_MASK, #field, true }


// Stoney: one shader engine with one shader array of at most three CUs,
// four SIMDs per CU, ten wave slots per SIMD (gfx_v8_0_gpu_early_init()).
static const uint32 kMaxCuPerSh = 3;
static const uint32 kSimdPerCu = 4;
static const uint32 kWavesPerSimd = 10;
static const int kMaxWavesLogged = 16;

// Keeps lines well below the 512 bytes Log() takes.
static const size_t kLineLength = 400;


//	#pragma mark - field tables


static const Field kGrbmStatus[] = {
	COUNT(GRBM_STATUS, ME0PIPE0_CMDFIFO_AVAIL),
	FLAG(GRBM_STATUS, SRBM_RQ_PENDING),
	FLAG(GRBM_STATUS, ME0PIPE0_CF_RQ_PENDING),
	FLAG(GRBM_STATUS, ME0PIPE0_PF_RQ_PENDING),
	FLAG(GRBM_STATUS, GDS_DMA_RQ_PENDING),
	FLAG(GRBM_STATUS, DB_CLEAN),
	FLAG(GRBM_STATUS, CB_CLEAN),
	FLAG(GRBM_STATUS, TA_BUSY),
	FLAG(GRBM_STATUS, GDS_BUSY),
	FLAG(GRBM_STATUS, WD_BUSY_NO_DMA),
	FLAG(GRBM_STATUS, VGT_BUSY),
	FLAG(GRBM_STATUS, IA_BUSY_NO_DMA),
	FLAG(GRBM_STATUS, IA_BUSY),
	FLAG(GRBM_STATUS, SX_BUSY),
	FLAG(GRBM_STATUS, WD_BUSY),
	FLAG(GRBM_STATUS, SPI_BUSY),
	FLAG(GRBM_STATUS, BCI_BUSY),
	FLAG(GRBM_STATUS, SC_BUSY),
	FLAG(GRBM_STATUS, PA_BUSY),
	FLAG(GRBM_STATUS, DB_BUSY),
	FLAG(GRBM_STATUS, CP_COHERENCY_BUSY),
	FLAG(GRBM_STATUS, CP_BUSY),
	FLAG(GRBM_STATUS, CB_BUSY),
	FLAG(GRBM_STATUS, GUI_ACTIVE),
	{ 0, NULL, false }
};

static const Field kGrbmStatus2[] = {
	COUNT(GRBM_STATUS2, ME0PIPE1_CMDFIFO_AVAIL),
	FLAG(GRBM_STATUS2, ME0PIPE1_CF_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME0PIPE1_PF_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME1PIPE0_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME1PIPE1_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME1PIPE2_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME1PIPE3_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME2PIPE0_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME2PIPE1_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME2PIPE2_RQ_PENDING),
	FLAG(GRBM_STATUS2, ME2PIPE3_RQ_PENDING),
	FLAG(GRBM_STATUS2, RLC_RQ_PENDING),
	FLAG(GRBM_STATUS2, RLC_BUSY),
	FLAG(GRBM_STATUS2, TC_BUSY),
	FLAG(GRBM_STATUS2, TCC_CC_RESIDENT),
	FLAG(GRBM_STATUS2, CPF_BUSY),
	FLAG(GRBM_STATUS2, CPC_BUSY),
	FLAG(GRBM_STATUS2, CPG_BUSY),
	{ 0, NULL, false }
};

static const Field kGrbmStatusSe0[] = {
	FLAG(GRBM_STATUS_SE0, DB_CLEAN),
	FLAG(GRBM_STATUS_SE0, CB_CLEAN),
	FLAG(GRBM_STATUS_SE0, BCI_BUSY),
	FLAG(GRBM_STATUS_SE0, VGT_BUSY),
	FLAG(GRBM_STATUS_SE0, PA_BUSY),
	FLAG(GRBM_STATUS_SE0, TA_BUSY),
	FLAG(GRBM_STATUS_SE0, SX_BUSY),
	FLAG(GRBM_STATUS_SE0, SPI_BUSY),
	FLAG(GRBM_STATUS_SE0, SC_BUSY),
	FLAG(GRBM_STATUS_SE0, DB_BUSY),
	FLAG(GRBM_STATUS_SE0, CB_BUSY),
	{ 0, NULL, false }
};

static const Field kSrbmStatus[] = {
	FLAG(SRBM_STATUS, UVD_RQ_PENDING),
	FLAG(SRBM_STATUS, SAMMSP_RQ_PENDING),
	FLAG(SRBM_STATUS, ACP_RQ_PENDING),
	FLAG(SRBM_STATUS, SMU_RQ_PENDING),
	FLAG(SRBM_STATUS, GRBM_RQ_PENDING),
	FLAG(SRBM_STATUS, HI_RQ_PENDING),
	FLAG(SRBM_STATUS, VMC_BUSY),
	FLAG(SRBM_STATUS, MCB_BUSY),
	FLAG(SRBM_STATUS, MCB_NON_DISPLAY_BUSY),
	FLAG(SRBM_STATUS, MCC_BUSY),
	FLAG(SRBM_STATUS, MCD_BUSY),
	FLAG(SRBM_STATUS, VMC1_BUSY),
	FLAG(SRBM_STATUS, SEM_BUSY),
	FLAG(SRBM_STATUS, ACP_BUSY),
	FLAG(SRBM_STATUS, IH_BUSY),
	FLAG(SRBM_STATUS, UVD_BUSY),
	FLAG(SRBM_STATUS, SAMMSP_BUSY),
	FLAG(SRBM_STATUS, GCATCL2_BUSY),
	FLAG(SRBM_STATUS, OSATCL2_BUSY),
	FLAG(SRBM_STATUS, BIF_BUSY),
	{ 0, NULL, false }
};

static const Field kSrbmStatus2[] = {
	FLAG(SRBM_STATUS2, SDMA_RQ_PENDING),
	FLAG(SRBM_STATUS2, TST_RQ_PENDING),
	FLAG(SRBM_STATUS2, SDMA1_RQ_PENDING),
	FLAG(SRBM_STATUS2, VCE0_RQ_PENDING),
	FLAG(SRBM_STATUS2, VP8_BUSY),
	FLAG(SRBM_STATUS2, SDMA_BUSY),
	FLAG(SRBM_STATUS2, SDMA1_BUSY),
	FLAG(SRBM_STATUS2, VCE0_BUSY),
	FLAG(SRBM_STATUS2, XDMA_BUSY),
	FLAG(SRBM_STATUS2, CHUB_BUSY),
	FLAG(SRBM_STATUS2, SDMA2_BUSY),
	FLAG(SRBM_STATUS2, SDMA3_BUSY),
	FLAG(SRBM_STATUS2, SAMSCP_BUSY),
	FLAG(SRBM_STATUS2, ISP_BUSY),
	FLAG(SRBM_STATUS2, VCE1_BUSY),
	FLAG(SRBM_STATUS2, ODE_BUSY),
	FLAG(SRBM_STATUS2, SDMA2_RQ_PENDING),
	FLAG(SRBM_STATUS2, SDMA3_RQ_PENDING),
	FLAG(SRBM_STATUS2, SAMSCP_RQ_PENDING),
	FLAG(SRBM_STATUS2, ISP_RQ_PENDING),
	FLAG(SRBM_STATUS2, VCE1_RQ_PENDING),
	{ 0, NULL, false }
};

static const Field kCpStat[] = {
	FLAG(CP_STAT, ROQ_RING_BUSY),
	FLAG(CP_STAT, ROQ_INDIRECT1_BUSY),
	FLAG(CP_STAT, ROQ_INDIRECT2_BUSY),
	FLAG(CP_STAT, ROQ_STATE_BUSY),
	FLAG(CP_STAT, DC_BUSY),
	FLAG(CP_STAT, ATCL2IU_BUSY),
	FLAG(CP_STAT, PFP_BUSY),
	FLAG(CP_STAT, MEQ_BUSY),
	FLAG(CP_STAT, ME_BUSY),
	FLAG(CP_STAT, QUERY_BUSY),
	FLAG(CP_STAT, SEMAPHORE_BUSY),
	FLAG(CP_STAT, INTERRUPT_BUSY),
	FLAG(CP_STAT, SURFACE_SYNC_BUSY),
	FLAG(CP_STAT, DMA_BUSY),
	FLAG(CP_STAT, RCIU_BUSY),
	FLAG(CP_STAT, SCRATCH_RAM_BUSY),
	FLAG(CP_STAT, CPC_CPG_BUSY),
	FLAG(CP_STAT, CE_BUSY),
	FLAG(CP_STAT, TCIU_BUSY),
	FLAG(CP_STAT, ROQ_CE_RING_BUSY),
	FLAG(CP_STAT, ROQ_CE_INDIRECT1_BUSY),
	FLAG(CP_STAT, ROQ_CE_INDIRECT2_BUSY),
	FLAG(CP_STAT, CP_BUSY),
	{ 0, NULL, false }
};

static const Field kCpBusyStat[] = {
	FLAG(CP_BUSY_STAT, REG_BUS_FIFO_BUSY),
	FLAG(CP_BUSY_STAT, COHER_CNT_NEQ_ZERO),
	FLAG(CP_BUSY_STAT, PFP_PARSING_PACKETS),
	FLAG(CP_BUSY_STAT, ME_PARSING_PACKETS),
	FLAG(CP_BUSY_STAT, RCIU_PFP_BUSY),
	FLAG(CP_BUSY_STAT, RCIU_ME_BUSY),
	FLAG(CP_BUSY_STAT, SEM_CMDFIFO_NOT_EMPTY),
	FLAG(CP_BUSY_STAT, SEM_FAILED_AND_HOLDING),
	FLAG(CP_BUSY_STAT, SEM_POLLING_FOR_PASS),
	FLAG(CP_BUSY_STAT, GFX_CONTEXT_BUSY),
	FLAG(CP_BUSY_STAT, ME_PARSER_BUSY),
	FLAG(CP_BUSY_STAT, EOP_DONE_BUSY),
	FLAG(CP_BUSY_STAT, STRM_OUT_BUSY),
	FLAG(CP_BUSY_STAT, PIPE_STATS_BUSY),
	FLAG(CP_BUSY_STAT, RCIU_CE_BUSY),
	FLAG(CP_BUSY_STAT, CE_PARSING_PACKETS),
	{ 0, NULL, false }
};

static const Field kCpStalledStat1[] = {
	FLAG(CP_STALLED_STAT1, RBIU_TO_DMA_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT1, RBIU_TO_SEM_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT1, RBIU_TO_MEMWR_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT1, ME_HAS_ACTIVE_CE_BUFFER_FLAG),
	FLAG(CP_STALLED_STAT1, ME_HAS_ACTIVE_DE_BUFFER_FLAG),
	FLAG(CP_STALLED_STAT1, ME_STALLED_ON_TC_WR_CONFIRM),
	FLAG(CP_STALLED_STAT1, ME_STALLED_ON_ATOMIC_RTN_DATA),
	FLAG(CP_STALLED_STAT1, ME_WAITING_ON_TC_READ_DATA),
	FLAG(CP_STALLED_STAT1, ME_WAITING_ON_REG_READ_DATA),
	FLAG(CP_STALLED_STAT1, RCIU_WAITING_ON_GDS_FREE),
	FLAG(CP_STALLED_STAT1, RCIU_WAITING_ON_GRBM_FREE),
	FLAG(CP_STALLED_STAT1, RCIU_WAITING_ON_VGT_FREE),
	FLAG(CP_STALLED_STAT1, RCIU_STALLED_ON_ME_READ),
	FLAG(CP_STALLED_STAT1, RCIU_STALLED_ON_DMA_READ),
	FLAG(CP_STALLED_STAT1, RCIU_STALLED_ON_APPEND_READ),
	FLAG(CP_STALLED_STAT1, RCIU_HALTED_BY_REG_VIOLATION),
	{ 0, NULL, false }
};

static const Field kCpStalledStat2[] = {
	FLAG(CP_STALLED_STAT2, PFP_TO_CSF_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, PFP_TO_MEQ_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, PFP_TO_RCIU_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, PFP_TO_VGT_WRITES_PENDING),
	FLAG(CP_STALLED_STAT2, PFP_RCIU_READ_PENDING),
	FLAG(CP_STALLED_STAT2, PFP_WAITING_ON_BUFFER_DATA),
	FLAG(CP_STALLED_STAT2, ME_WAIT_ON_CE_COUNTER),
	FLAG(CP_STALLED_STAT2, ME_WAIT_ON_AVAIL_BUFFER),
	FLAG(CP_STALLED_STAT2, GFX_CNTX_NOT_AVAIL_TO_ME),
	FLAG(CP_STALLED_STAT2, ME_RCIU_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, ME_TO_CONST_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, ME_WAITING_DATA_FROM_PFP),
	FLAG(CP_STALLED_STAT2, ME_WAITING_ON_PARTIAL_FLUSH),
	FLAG(CP_STALLED_STAT2, MEQ_TO_ME_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, STQ_TO_ME_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT2, ME_WAITING_DATA_FROM_STQ),
	FLAG(CP_STALLED_STAT2, PFP_STALLED_ON_TC_WR_CONFIRM),
	FLAG(CP_STALLED_STAT2, PFP_STALLED_ON_ATOMIC_RTN_DATA),
	FLAG(CP_STALLED_STAT2, EOPD_FIFO_NEEDS_SC_EOP_DONE),
	FLAG(CP_STALLED_STAT2, EOPD_FIFO_NEEDS_WR_CONFIRM),
	FLAG(CP_STALLED_STAT2, STRMO_WR_OF_PRIM_DATA_PENDING),
	FLAG(CP_STALLED_STAT2, PIPE_STATS_WR_DATA_PENDING),
	FLAG(CP_STALLED_STAT2, APPEND_RDY_WAIT_ON_CS_DONE),
	FLAG(CP_STALLED_STAT2, APPEND_RDY_WAIT_ON_PS_DONE),
	FLAG(CP_STALLED_STAT2, APPEND_WAIT_ON_WR_CONFIRM),
	FLAG(CP_STALLED_STAT2, APPEND_ACTIVE_PARTITION),
	FLAG(CP_STALLED_STAT2, APPEND_WAITING_TO_SEND_MEMWRITE),
	FLAG(CP_STALLED_STAT2, SURF_SYNC_NEEDS_IDLE_CNTXS),
	FLAG(CP_STALLED_STAT2, SURF_SYNC_NEEDS_ALL_CLEAN),
	{ 0, NULL, false }
};

static const Field kCpStalledStat3[] = {
	FLAG(CP_STALLED_STAT3, CE_TO_CSF_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT3, CE_TO_RAM_INIT_FETCHER_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT3, CE_WAITING_ON_DATA_FROM_RAM_INIT_FETCHER),
	FLAG(CP_STALLED_STAT3, CE_TO_RAM_INIT_NOT_RDY),
	FLAG(CP_STALLED_STAT3, CE_TO_RAM_DUMP_NOT_RDY),
	FLAG(CP_STALLED_STAT3, CE_TO_RAM_WRITE_NOT_RDY),
	FLAG(CP_STALLED_STAT3, CE_TO_INC_FIFO_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT3, CE_TO_WR_FIFO_NOT_RDY_TO_RCV),
	FLAG(CP_STALLED_STAT3, CE_WAITING_ON_BUFFER_DATA),
	FLAG(CP_STALLED_STAT3, CE_WAITING_ON_CE_BUFFER_FLAG),
	FLAG(CP_STALLED_STAT3, CE_WAITING_ON_DE_COUNTER),
	FLAG(CP_STALLED_STAT3, CE_WAITING_ON_DE_COUNTER_UNDERFLOW),
	FLAG(CP_STALLED_STAT3, TCIU_WAITING_ON_FREE),
	FLAG(CP_STALLED_STAT3, TCIU_WAITING_ON_TAGS),
	FLAG(CP_STALLED_STAT3, CE_STALLED_ON_TC_WR_CONFIRM),
	FLAG(CP_STALLED_STAT3, CE_STALLED_ON_ATOMIC_RTN_DATA),
	FLAG(CP_STALLED_STAT3, ATCL2IU_WAITING_ON_FREE),
	FLAG(CP_STALLED_STAT3, ATCL2IU_WAITING_ON_TAGS),
	FLAG(CP_STALLED_STAT3, ATCL1_WAITING_ON_TRANS),
	{ 0, NULL, false }
};

static const Field kCpCpfStatus[] = {
	FLAG(CP_CPF_STATUS, POST_WPTR_GFX_BUSY),
	FLAG(CP_CPF_STATUS, CSF_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_ALIGN_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_RING_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_INDIRECT1_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_INDIRECT2_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_STATE_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_CE_RING_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_CE_INDIRECT1_BUSY),
	FLAG(CP_CPF_STATUS, ROQ_CE_INDIRECT2_BUSY),
	FLAG(CP_CPF_STATUS, SEMAPHORE_BUSY),
	FLAG(CP_CPF_STATUS, INTERRUPT_BUSY),
	FLAG(CP_CPF_STATUS, TCIU_BUSY),
	FLAG(CP_CPF_STATUS, HQD_BUSY),
	FLAG(CP_CPF_STATUS, PRT_BUSY),
	FLAG(CP_CPF_STATUS, ATCL2IU_BUSY),
	FLAG(CP_CPF_STATUS, CPF_GFX_BUSY),
	FLAG(CP_CPF_STATUS, CPF_CMP_BUSY),
	FLAG(CP_CPF_STATUS, GRBM_CPF_STAT_BUSY),
	FLAG(CP_CPF_STATUS, CPC_CPF_BUSY),
	FLAG(CP_CPF_STATUS, CPF_BUSY),
	{ 0, NULL, false }
};

static const Field kCpCpfBusyStat[] = {
	FLAG(CP_CPF_BUSY_STAT, REG_BUS_FIFO_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_RING_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_INDIRECT1_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_INDIRECT2_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_STATE_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_CE_INDR1_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_CE_INDR2_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_ARBITER_BUSY),
	FLAG(CP_CPF_BUSY_STAT, CSF_INPUT_BUSY),
	FLAG(CP_CPF_BUSY_STAT, OUTSTANDING_READ_TAGS),
	FLAG(CP_CPF_BUSY_STAT, HPD_PROCESSING_EOP_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_DISPATCH_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_IQ_TIMER_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_DMA_OFFLOAD_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_WAIT_SEMAPHORE_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_SIGNAL_SEMAPHORE_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_MESSAGE_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_PQ_FETCHER_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_IB_FETCHER_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_IQ_FETCHER_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_EOP_FETCHER_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_CONSUMED_RPTR_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_FETCHER_ARB_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_ROQ_ALIGN_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_ROQ_EOP_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_ROQ_IQ_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_ROQ_PQ_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_ROQ_IB_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_WPTR_POLL_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_PQ_BUSY),
	FLAG(CP_CPF_BUSY_STAT, HQD_IB_BUSY),
	{ 0, NULL, false }
};

static const Field kCpCpfStalledStat1[] = {
	FLAG(CP_CPF_STALLED_STAT1, RING_FETCHING_DATA),
	FLAG(CP_CPF_STALLED_STAT1, INDR1_FETCHING_DATA),
	FLAG(CP_CPF_STALLED_STAT1, INDR2_FETCHING_DATA),
	FLAG(CP_CPF_STALLED_STAT1, STATE_FETCHING_DATA),
	FLAG(CP_CPF_STALLED_STAT1, TCIU_WAITING_ON_FREE),
	FLAG(CP_CPF_STALLED_STAT1, TCIU_WAITING_ON_TAGS),
	FLAG(CP_CPF_STALLED_STAT1, ATCL2IU_WAITING_ON_FREE),
	FLAG(CP_CPF_STALLED_STAT1, ATCL2IU_WAITING_ON_TAGS),
	FLAG(CP_CPF_STALLED_STAT1, ATCL1_WAITING_ON_TRANS),
	{ 0, NULL, false }
};

static const Field kCpCpcStatus[] = {
	FLAG(CP_CPC_STATUS, MEC1_BUSY),
	FLAG(CP_CPC_STATUS, MEC2_BUSY),
	FLAG(CP_CPC_STATUS, DC0_BUSY),
	FLAG(CP_CPC_STATUS, DC1_BUSY),
	FLAG(CP_CPC_STATUS, RCIU1_BUSY),
	FLAG(CP_CPC_STATUS, RCIU2_BUSY),
	FLAG(CP_CPC_STATUS, ROQ1_BUSY),
	FLAG(CP_CPC_STATUS, ROQ2_BUSY),
	FLAG(CP_CPC_STATUS, TCIU_BUSY),
	FLAG(CP_CPC_STATUS, SCRATCH_RAM_BUSY),
	FLAG(CP_CPC_STATUS, QU_BUSY),
	FLAG(CP_CPC_STATUS, ATCL2IU_BUSY),
	FLAG(CP_CPC_STATUS, CPG_CPC_BUSY),
	FLAG(CP_CPC_STATUS, CPF_CPC_BUSY),
	FLAG(CP_CPC_STATUS, CPC_BUSY),
	{ 0, NULL, false }
};

static const Field kCpMeCntl[] = {
	FLAG(CP_ME_CNTL, CE_INVALIDATE_ICACHE),
	FLAG(CP_ME_CNTL, PFP_INVALIDATE_ICACHE),
	FLAG(CP_ME_CNTL, ME_INVALIDATE_ICACHE),
	FLAG(CP_ME_CNTL, CE_PIPE0_RESET),
	FLAG(CP_ME_CNTL, PFP_PIPE0_RESET),
	FLAG(CP_ME_CNTL, ME_PIPE0_RESET),
	FLAG(CP_ME_CNTL, CE_HALT),
	FLAG(CP_ME_CNTL, CE_STEP),
	FLAG(CP_ME_CNTL, PFP_HALT),
	FLAG(CP_ME_CNTL, PFP_STEP),
	FLAG(CP_ME_CNTL, ME_HALT),
	FLAG(CP_ME_CNTL, ME_STEP),
	{ 0, NULL, false }
};

static const Field kCpIntStatus[] = {
	FLAG(CP_INT_STATUS, CP_VM_DOORBELL_WR_INT_STAT),
	FLAG(CP_INT_STATUS, CP_ECC_ERROR_INT_STAT),
	FLAG(CP_INT_STATUS, WRM_POLL_TIMEOUT_INT_STAT),
	FLAG(CP_INT_STATUS, CMP_BUSY_INT_STAT),
	FLAG(CP_INT_STATUS, CNTX_BUSY_INT_STAT),
	FLAG(CP_INT_STATUS, CNTX_EMPTY_INT_STAT),
	FLAG(CP_INT_STATUS, GFX_IDLE_INT_STAT),
	FLAG(CP_INT_STATUS, PRIV_INSTR_INT_STAT),
	FLAG(CP_INT_STATUS, PRIV_REG_INT_STAT),
	FLAG(CP_INT_STATUS, OPCODE_ERROR_INT_STAT),
	FLAG(CP_INT_STATUS, TIME_STAMP_INT_STAT),
	FLAG(CP_INT_STATUS, RESERVED_BIT_ERROR_INT_STAT),
	FLAG(CP_INT_STATUS, GENERIC2_INT_STAT),
	FLAG(CP_INT_STATUS, GENERIC1_INT_STAT),
	FLAG(CP_INT_STATUS, GENERIC0_INT_STAT),
	{ 0, NULL, false }
};

static const Field kIaCntlStatus[] = {
	FLAG(IA_CNTL_STATUS, IA_BUSY),
	FLAG(IA_CNTL_STATUS, IA_DMA_BUSY),
	FLAG(IA_CNTL_STATUS, IA_DMA_REQ_BUSY),
	FLAG(IA_CNTL_STATUS, IA_GRP_BUSY),
	FLAG(IA_CNTL_STATUS, IA_ADC_BUSY),
	{ 0, NULL, false }
};

static const Field kVgtCntlStatus[] = {
	FLAG(VGT_CNTL_STATUS, VGT_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_OUT_INDX_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_OUT_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_PT_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_TE_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_VR_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_PI_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_GS_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_HS_BUSY),
	FLAG(VGT_CNTL_STATUS, VGT_TE11_BUSY),
	{ 0, NULL, false }
};

static const Field kWdCntlStatus[] = {
	FLAG(WD_CNTL_STATUS, WD_BUSY),
	FLAG(WD_CNTL_STATUS, WD_SPL_DMA_BUSY),
	FLAG(WD_CNTL_STATUS, WD_SPL_DI_BUSY),
	FLAG(WD_CNTL_STATUS, WD_ADC_BUSY),
	{ 0, NULL, false }
};

static const Field kPaClCntlStatus[] = {
	FLAG(PA_CL_CNTL_STATUS, CL_BUSY),
	{ 0, NULL, false }
};

static const Field kPaSuCntlStatus[] = {
	FLAG(PA_SU_CNTL_STATUS, SU_BUSY),
	{ 0, NULL, false }
};

static const Field kSpiDebugBusy[] = {
	FLAG(SPI_DEBUG_BUSY, LS_BUSY),
	FLAG(SPI_DEBUG_BUSY, HS_BUSY),
	FLAG(SPI_DEBUG_BUSY, ES_BUSY),
	FLAG(SPI_DEBUG_BUSY, GS_BUSY),
	FLAG(SPI_DEBUG_BUSY, VS_BUSY),
	FLAG(SPI_DEBUG_BUSY, PS0_BUSY),
	FLAG(SPI_DEBUG_BUSY, PS1_BUSY),
	FLAG(SPI_DEBUG_BUSY, CSG_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS0_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS1_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS2_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS3_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS4_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS5_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS6_BUSY),
	FLAG(SPI_DEBUG_BUSY, CS7_BUSY),
	FLAG(SPI_DEBUG_BUSY, LDS_WR_CTL0_BUSY),
	FLAG(SPI_DEBUG_BUSY, LDS_WR_CTL1_BUSY),
	FLAG(SPI_DEBUG_BUSY, RSRC_ALLOC0_BUSY),
	FLAG(SPI_DEBUG_BUSY, RSRC_ALLOC1_BUSY),
	FLAG(SPI_DEBUG_BUSY, PC_DEALLOC_BUSY),
	FLAG(SPI_DEBUG_BUSY, EVENT_CLCTR_BUSY),
	FLAG(SPI_DEBUG_BUSY, GRBM_BUSY),
	FLAG(SPI_DEBUG_BUSY, SPIS_BUSY),
	{ 0, NULL, false }
};

static const Field kGdsCntlStatus[] = {
	FLAG(GDS_CNTL_STATUS, GDS_BUSY),
	FLAG(GDS_CNTL_STATUS, GRBM_WBUF_BUSY),
	FLAG(GDS_CNTL_STATUS, ORD_APP_BUSY),
	FLAG(GDS_CNTL_STATUS, DS_BANK_CONFLICT),
	FLAG(GDS_CNTL_STATUS, DS_ADDR_CONFLICT),
	FLAG(GDS_CNTL_STATUS, DS_WR_CLAMP),
	FLAG(GDS_CNTL_STATUS, DS_RD_CLAMP),
	FLAG(GDS_CNTL_STATUS, GRBM_RBUF_BUSY),
	FLAG(GDS_CNTL_STATUS, DS_BUSY),
	FLAG(GDS_CNTL_STATUS, GWS_BUSY),
	FLAG(GDS_CNTL_STATUS, ORD_FIFO_BUSY),
	FLAG(GDS_CNTL_STATUS, CREDIT_BUSY0),
	FLAG(GDS_CNTL_STATUS, CREDIT_BUSY1),
	FLAG(GDS_CNTL_STATUS, CREDIT_BUSY2),
	FLAG(GDS_CNTL_STATUS, CREDIT_BUSY3),
	{ 0, NULL, false }
};

static const Field kTaStatus[] = {
	FLAG(TA_STATUS, FG_PFIFO_EMPTYB),
	FLAG(TA_STATUS, FG_LFIFO_EMPTYB),
	FLAG(TA_STATUS, FG_SFIFO_EMPTYB),
	FLAG(TA_STATUS, FL_PFIFO_EMPTYB),
	FLAG(TA_STATUS, FL_LFIFO_EMPTYB),
	FLAG(TA_STATUS, FL_SFIFO_EMPTYB),
	FLAG(TA_STATUS, FA_PFIFO_EMPTYB),
	FLAG(TA_STATUS, FA_LFIFO_EMPTYB),
	FLAG(TA_STATUS, FA_SFIFO_EMPTYB),
	FLAG(TA_STATUS, IN_BUSY),
	FLAG(TA_STATUS, FG_BUSY),
	FLAG(TA_STATUS, LA_BUSY),
	FLAG(TA_STATUS, FL_BUSY),
	FLAG(TA_STATUS, TA_BUSY),
	FLAG(TA_STATUS, FA_BUSY),
	FLAG(TA_STATUS, AL_BUSY),
	FLAG(TA_STATUS, BUSY),
	{ 0, NULL, false }
};

static const Field kTcpStatus[] = {
	FLAG(TCP_STATUS, TCP_BUSY),
	FLAG(TCP_STATUS, INPUT_BUSY),
	FLAG(TCP_STATUS, ADRS_BUSY),
	FLAG(TCP_STATUS, TAGRAMS_BUSY),
	FLAG(TCP_STATUS, CNTRL_BUSY),
	FLAG(TCP_STATUS, LFIFO_BUSY),
	FLAG(TCP_STATUS, READ_BUSY),
	FLAG(TCP_STATUS, FORMAT_BUSY),
	{ 0, NULL, false }
};

static const Field kSqWaveStatus[] = {
	FLAG(SQ_WAVE_STATUS, SCC),
	FLAG(SQ_WAVE_STATUS, SPI_PRIO),
	FLAG(SQ_WAVE_STATUS, USER_PRIO),
	FLAG(SQ_WAVE_STATUS, PRIV),
	FLAG(SQ_WAVE_STATUS, TRAP_EN),
	FLAG(SQ_WAVE_STATUS, TTRACE_EN),
	FLAG(SQ_WAVE_STATUS, EXPORT_RDY),
	FLAG(SQ_WAVE_STATUS, EXECZ),
	FLAG(SQ_WAVE_STATUS, VCCZ),
	FLAG(SQ_WAVE_STATUS, IN_TG),
	FLAG(SQ_WAVE_STATUS, IN_BARRIER),
	FLAG(SQ_WAVE_STATUS, HALT),
	FLAG(SQ_WAVE_STATUS, TRAP),
	FLAG(SQ_WAVE_STATUS, TTRACE_CU_EN),
	FLAG(SQ_WAVE_STATUS, VALID),
	FLAG(SQ_WAVE_STATUS, ECC_ERR),
	FLAG(SQ_WAVE_STATUS, SKIP_EXPORT),
	FLAG(SQ_WAVE_STATUS, PERF_EN),
	FLAG(SQ_WAVE_STATUS, COND_DBG_USER),
	FLAG(SQ_WAVE_STATUS, COND_DBG_SYS),
	FLAG(SQ_WAVE_STATUS, ALLOW_REPLAY),
	FLAG(SQ_WAVE_STATUS, INST_ATC),
	FLAG(SQ_WAVE_STATUS, MUST_EXPORT),
	{ 0, NULL, false }
};

// SQ_WAVE_TRAPSTS.EXCP, one bit per exception (GCN3 ISA, "TRAPSTS").
static const Field kSqWaveTrapstsExcp[] = {
	{ 1u << 0, "EXCP_INVALID", false },
	{ 1u << 1, "EXCP_INPUT_DENORM", false },
	{ 1u << 2, "EXCP_FLOAT_DIV0", false },
	{ 1u << 3, "EXCP_OVERFLOW", false },
	{ 1u << 4, "EXCP_UNDERFLOW", false },
	{ 1u << 5, "EXCP_INEXACT", false },
	{ 1u << 6, "EXCP_INT_DIV0", false },
	{ 1u << 7, "EXCP_ADDR_WATCH", false },
	{ 1u << 8, "EXCP_MEM_VIOL", false },
	FLAG(SQ_WAVE_TRAPSTS, SAVECTX),
	FLAG(SQ_WAVE_TRAPSTS, EXCP_CYCLE),
	FLAG(SQ_WAVE_TRAPSTS, DP_RATE),
	{ 0, NULL, false }
};


//	#pragma mark -


static inline uint32
get_field(uint32 value, uint32 mask)
{
	return (value & mask) >> __builtin_ctz(mask);
}


static void
append(char* line, size_t& length, const char* text)
{
	size_t textLength = strlen(text);
	memcpy(line + length, text, textLength + 1);
	length += textLength;
}


/*!	Logs "hang: NAME 0xVALUE:" followed by the names of the set fields (with
	their values when they are wider than a bit), continuing on more lines
	when they do not fit. Bits no field covers are logged as "other".
*/
static void
log_decoded(const char* name, uint32 value, const Field* fields)
{
	char line[kLineLength + 128];
	size_t length = snprintf(line, sizeof(line), "hang: %s 0x%08" B_PRIx32
		":", name, value);
	if (value == 0xffffffff) {
		Log("%s all ones (register not reachable or block powered off)",
			line);
		return;
	}

	uint32 known = 0;
	char item[96];
	for (const Field* field = fields; field->name != NULL; field++) {
		known |= field->mask;
		uint32 fieldValue = get_field(value, field->mask);
		if (fieldValue == 0 && !field->always)
			continue;

		bool wide = (field->mask & (field->mask - 1)) != 0;
		if (field->always || wide) {
			snprintf(item, sizeof(item), " %s=%" B_PRIu32, field->name,
				fieldValue);
		} else
			snprintf(item, sizeof(item), " %s", field->name);

		if (length + strlen(item) > kLineLength) {
			Log("%s", line);
			length = snprintf(line, sizeof(line), "hang:   %s (cont.):", name);
		}
		append(line, length, item);
	}

	if ((value & ~known) != 0) {
		snprintf(item, sizeof(item), " other=0x%" B_PRIx32, value & ~known);
		append(line, length, item);
	}
	if (value == 0)
		append(line, length, " -");

	Log("%s", line);
}


static void
log_register(Gpu& gpu, const char* name, uint32 reg, const Field* fields)
{
	log_decoded(name, gpu.Read(reg), fields);
}


#define LOG_REGISTER(gpu, reg, fields) \
	log_register(gpu, #reg, mm##reg, fields)


// The CP's pointers into the ring and the indirect buffers.
static void
log_cp_pointers(Gpu& gpu)
{
	uint32 rptr = gpu.Read(mmCP_RB0_RPTR);
	uint32 wptr = gpu.Read(mmCP_RB0_WPTR);
	Log("hang: CP_RB0_RPTR 0x%" B_PRIx32 " CP_RB0_WPTR 0x%" B_PRIx32 " (%s)",
		rptr, wptr, rptr == wptr ? "CP fetched the whole ring"
			: "CP has not fetched the whole ring");

	// The IB being executed: its base, and BUFSZ, the dwords not fetched
	// yet.
	uint64 ib1 = ((uint64)(gpu.Read(mmCP_IB1_BASE_HI)
			& CP_IB1_BASE_HI__IB1_BASE_HI_MASK) << 32)
		| gpu.Read(mmCP_IB1_BASE_LO);
	uint64 ib2 = ((uint64)(gpu.Read(mmCP_IB2_BASE_HI)
			& CP_IB2_BASE_HI__IB2_BASE_HI_MASK) << 32)
		| gpu.Read(mmCP_IB2_BASE_LO);
	Log("hang: CP_IB1 base 0x%" B_PRIx64 " BUFSZ %" B_PRIu32 " OFFSET 0x%"
		B_PRIx32 "; CP_IB2 base 0x%" B_PRIx64 " BUFSZ %" B_PRIu32 " OFFSET 0x%"
		B_PRIx32, ib1,
		gpu.Read(mmCP_IB1_BUFSZ) & CP_IB1_BUFSZ__IB1_BUFSZ_MASK,
		gpu.Read(mmCP_IB1_OFFSET) & CP_IB1_OFFSET__IB1_OFFSET_MASK, ib2,
		gpu.Read(mmCP_IB2_BUFSZ) & CP_IB2_BUFSZ__IB2_BUFSZ_MASK,
		gpu.Read(mmCP_IB2_OFFSET) & CP_IB2_OFFSET__IB2_OFFSET_MASK);

	// The queues between the fetcher, PFP and ME.
	uint32 meq = gpu.Read(mmCP_MEQ_STAT);
	uint32 roqRb = gpu.Read(mmCP_ROQ_RB_STAT);
	uint32 roqIb1 = gpu.Read(mmCP_ROQ_IB1_STAT);
	uint32 roqIb2 = gpu.Read(mmCP_ROQ_IB2_STAT);
	Log("hang: CP_MEQ_STAT rptr %" B_PRIu32 " wptr %" B_PRIu32
		", CP_ROQ_RB_STAT rptr %" B_PRIu32 " wptr %" B_PRIu32
		", CP_ROQ_IB1_STAT rptr %" B_PRIu32 " wptr %" B_PRIu32
		", CP_ROQ_IB2_STAT rptr %" B_PRIu32 " wptr %" B_PRIu32,
		get_field(meq, CP_MEQ_STAT__MEQ_RPTR_MASK),
		get_field(meq, CP_MEQ_STAT__MEQ_WPTR_MASK),
		get_field(roqRb, CP_ROQ_RB_STAT__ROQ_RPTR_PRIMARY_MASK),
		get_field(roqRb, CP_ROQ_RB_STAT__ROQ_WPTR_PRIMARY_MASK),
		get_field(roqIb1, CP_ROQ_IB1_STAT__ROQ_RPTR_INDIRECT1_MASK),
		get_field(roqIb1, CP_ROQ_IB1_STAT__ROQ_WPTR_INDIRECT1_MASK),
		get_field(roqIb2, CP_ROQ_IB2_STAT__ROQ_RPTR_INDIRECT2_MASK),
		get_field(roqIb2, CP_ROQ_IB2_STAT__ROQ_WPTR_INDIRECT2_MASK));

	// The most recent packet headers, one per read.
	uint32 pfp[8];
	uint32 me[8];
	for (int i = 0; i < 8; i++) {
		pfp[i] = gpu.Read(mmCP_PFP_HEADER_DUMP);
		me[i] = gpu.Read(mmCP_ME_HEADER_DUMP);
	}
	Log("hang: PFP headers %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
		" %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
		" %08" B_PRIx32, pfp[0], pfp[1], pfp[2], pfp[3], pfp[4], pfp[5],
		pfp[6], pfp[7]);
	Log("hang: ME headers  %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
		" %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
		" %08" B_PRIx32, me[0], me[1], me[2], me[3], me[4], me[5], me[6],
		me[7]);
}


/*!	gmc_v8_0_vm_decode_fault(). Both contexts' status registers have the
	same layout. They stay set until cleared, which nothing here does, so a
	fault can also be from an earlier run since boot.
*/
static void
log_vm_fault(Gpu& gpu, int context, uint32 statusReg, uint32 addressReg,
	uint32 clientReg)
{
	uint32 status = gpu.Read(statusReg);
	uint32 page = gpu.Read(addressReg);
	if (status == 0) {
		Log("hang: VM_CONTEXT%d_PROTECTION_FAULT_STATUS 0: no fault (ADDR 0x%"
			B_PRIx32 ")", context, page);
		return;
	}

	// The client is four ASCII characters naming the block, e.g. "CB0 ".
	uint32 client = gpu.Read(clientReg);
	char block[5];
	for (int i = 0; i < 4; i++) {
		char c = (client >> (24 - 8 * i)) & 0xff;
		block[i] = isprint((unsigned char)c) ? c : '.';
	}
	block[4] = '\0';

	Log("hang: VM_CONTEXT%d_PROTECTION_FAULT_STATUS 0x%08" B_PRIx32
		": PROTECTIONS 0x%" B_PRIx32 " MEMORY_CLIENT_ID %" B_PRIu32 " (\"%s\")"
		" %s VMID %" B_PRIu32 "%s, ADDR page 0x%" B_PRIx32 " (MC address 0x%"
		B_PRIx64 ")", context, status,
		get_field(status, VM_CONTEXT0_PROTECTION_FAULT_STATUS__PROTECTIONS_MASK),
		get_field(status,
			VM_CONTEXT0_PROTECTION_FAULT_STATUS__MEMORY_CLIENT_ID_MASK),
		block,
		(status & VM_CONTEXT0_PROTECTION_FAULT_STATUS__MEMORY_CLIENT_RW_MASK)
			!= 0 ? "write" : "read",
		get_field(status, VM_CONTEXT0_PROTECTION_FAULT_STATUS__VMID_MASK),
		(status & VM_CONTEXT0_PROTECTION_FAULT_STATUS__ATOMIC_MASK) != 0
			? " ATOMIC" : "",
		page, (uint64)page << 12);
}


//	#pragma mark - shader waves


/*!	wave_read_ind(): selects a wave slot and one of its registers through
	the SQ_IND_INDEX index register, and reads it from SQ_IND_DATA.
	FORCE_READ reads it whether or not the wave is halted.
*/
static uint32
read_wave(Gpu& gpu, uint32 simd, uint32 wave, uint32 index)
{
	gpu.Write(mmSQ_IND_INDEX, (wave << SQ_IND_INDEX__WAVE_ID__SHIFT)
		| (simd << SQ_IND_INDEX__SIMD_ID__SHIFT)
		| (index << SQ_IND_INDEX__INDEX__SHIFT)
		| SQ_IND_INDEX__FORCE_READ_MASK);
	return gpu.Read(mmSQ_IND_DATA);
}


static void
log_wave(Gpu& gpu, uint32 cu, uint32 simd, uint32 wave, uint32 status)
{
	uint64 pc = ((uint64)read_wave(gpu, simd, wave, ixSQ_WAVE_PC_HI) << 32)
		| read_wave(gpu, simd, wave, ixSQ_WAVE_PC_LO);
	uint32 exec[2] = {
		read_wave(gpu, simd, wave, ixSQ_WAVE_EXEC_LO),
		read_wave(gpu, simd, wave, ixSQ_WAVE_EXEC_HI)
	};
	uint32 inst[2] = {
		read_wave(gpu, simd, wave, ixSQ_WAVE_INST_DW0),
		read_wave(gpu, simd, wave, ixSQ_WAVE_INST_DW1)
	};
	uint32 hwId = read_wave(gpu, simd, wave, ixSQ_WAVE_HW_ID);
	uint32 trapsts = read_wave(gpu, simd, wave, ixSQ_WAVE_TRAPSTS);
	uint32 ibSts = read_wave(gpu, simd, wave, ixSQ_WAVE_IB_STS);
	uint32 gprAlloc = read_wave(gpu, simd, wave, ixSQ_WAVE_GPR_ALLOC);
	uint32 ldsAlloc = read_wave(gpu, simd, wave, ixSQ_WAVE_LDS_ALLOC);
	uint32 mode = read_wave(gpu, simd, wave, ixSQ_WAVE_MODE);
	uint32 m0 = read_wave(gpu, simd, wave, ixSQ_WAVE_M0);

	Log("hang: wave cu %" B_PRIu32 " simd %" B_PRIu32 " slot %" B_PRIu32
		": PC 0x%" B_PRIx64 " INST %08" B_PRIx32 " %08" B_PRIx32 " EXEC %08"
		B_PRIx32 "%08" B_PRIx32 " HW_ID 0x%08" B_PRIx32 " (ME %" B_PRIu32
		" PIPE %" B_PRIu32 " QUEUE %" B_PRIu32 " STATE %" B_PRIu32 " VMID %"
		B_PRIu32 " TG %" B_PRIu32 ")", cu, simd, wave, pc, inst[0], inst[1],
		exec[1], exec[0], hwId,
		get_field(hwId, SQ_WAVE_HW_ID__ME_ID_MASK),
		get_field(hwId, SQ_WAVE_HW_ID__PIPE_ID_MASK),
		get_field(hwId, SQ_WAVE_HW_ID__QUEUE_ID_MASK),
		get_field(hwId, SQ_WAVE_HW_ID__STATE_ID_MASK),
		get_field(hwId, SQ_WAVE_HW_ID__VM_ID_MASK),
		get_field(hwId, SQ_WAVE_HW_ID__TG_ID_MASK));
	log_decoded("  SQ_WAVE_STATUS", status, kSqWaveStatus);
	log_decoded("  SQ_WAVE_TRAPSTS", trapsts, kSqWaveTrapstsExcp);
	Log("hang:   SQ_WAVE_IB_STS 0x%08" B_PRIx32 " (VM_CNT %" B_PRIu32
		" EXP_CNT %" B_PRIu32 " LGKM_CNT %" B_PRIu32 " VALU_CNT %" B_PRIu32
		") GPR_ALLOC 0x%08" B_PRIx32 " LDS_ALLOC 0x%08" B_PRIx32 " MODE 0x%08"
		B_PRIx32 " M0 0x%08" B_PRIx32, ibSts,
		get_field(ibSts, SQ_WAVE_IB_STS__VM_CNT_MASK),
		get_field(ibSts, SQ_WAVE_IB_STS__EXP_CNT_MASK),
		get_field(ibSts, SQ_WAVE_IB_STS__LGKM_CNT_MASK),
		get_field(ibSts, SQ_WAVE_IB_STS__VALU_CNT_MASK),
		gprAlloc, ldsAlloc, mode, m0);
}


/*!	Walks every wave slot of the active CUs, as umr does through Linux's
	debugfs wave reads, and logs the slots that hold a wave (VALID) or a
	halted one. Also logs the per-CU texture blocks on the way.
	Writes GRBM_GFX_INDEX and SQ_IND_INDEX, and leaves GRBM_GFX_INDEX
	broadcasting to all, as gfx_v8_0_select_se_sh(0xffffffff, ...) does.
*/
static void
log_waves(Gpu& gpu)
{
	uint32 savedIndex = gpu.Read(mmGRBM_GFX_INDEX);

	// SE 0, SH 0, all instances: the shader array's configuration
	// (gfx_v8_0_get_cu_active_bitmap()).
	gpu.Write(mmGRBM_GFX_INDEX, GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK);
	uint32 inactive = get_field(gpu.Read(mmCC_GC_SHADER_ARRAY_CONFIG)
			| gpu.Read(mmGC_USER_SHADER_ARRAY_CONFIG),
		CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK);
	uint32 activeCus = ~inactive & ((1u << kMaxCuPerSh) - 1);
	Log("hang: waves: GRBM_GFX_INDEX was 0x%08" B_PRIx32 ", active CU mask 0x%"
		B_PRIx32, savedIndex, activeCus);

	int found = 0;
	for (uint32 cu = 0; cu < kMaxCuPerSh; cu++) {
		if ((activeCus & (1u << cu)) == 0)
			continue;

		// gfx_v8_0_select_se_sh(adev, 0, 0, cu): SE 0, SH 0, instance cu
		gpu.Write(mmGRBM_GFX_INDEX,
			(cu << GRBM_GFX_INDEX__INSTANCE_INDEX__SHIFT)
				& GRBM_GFX_INDEX__INSTANCE_INDEX_MASK);

		char name[32];
		snprintf(name, sizeof(name), "CU%" B_PRIu32 " TA_STATUS", cu);
		log_register(gpu, name, mmTA_STATUS, kTaStatus);
		snprintf(name, sizeof(name), "CU%" B_PRIu32 " TCP_STATUS", cu);
		log_register(gpu, name, mmTCP_STATUS, kTcpStatus);

		for (uint32 simd = 0; simd < kSimdPerCu; simd++) {
			for (uint32 wave = 0; wave < kWavesPerSimd; wave++) {
				uint32 status = read_wave(gpu, simd, wave, ixSQ_WAVE_STATUS);
				if (status == 0xffffffff || (status
						& (SQ_WAVE_STATUS__VALID_MASK
							| SQ_WAVE_STATUS__HALT_MASK)) == 0) {
					continue;
				}
				if (found++ < kMaxWavesLogged)
					log_wave(gpu, cu, simd, wave, status);
			}
		}
	}

	gpu.Write(mmGRBM_GFX_INDEX, GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK
		| GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK
		| GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK);

	if (found == 0)
		Log("hang: waves: no slot holds a valid or halted wave");
	else if (found > kMaxWavesLogged) {
		Log("hang: waves: %d live waves, the first %d logged", found,
			kMaxWavesLogged);
	} else
		Log("hang: waves: %d live waves", found);
}


//	#pragma mark -


void
DumpGfxHang(Gpu& gpu, const char* why)
{
	Log("hang: --- %s ---", why);

	// The whole chip: which blocks are busy.
	LOG_REGISTER(gpu, GRBM_STATUS, kGrbmStatus);
	LOG_REGISTER(gpu, GRBM_STATUS2, kGrbmStatus2);
	LOG_REGISTER(gpu, GRBM_STATUS_SE0, kGrbmStatusSe0);
	LOG_REGISTER(gpu, SRBM_STATUS, kSrbmStatus);
	LOG_REGISTER(gpu, SRBM_STATUS2, kSrbmStatus2);

	// The command processor: what it is busy with, what it waits for.
	LOG_REGISTER(gpu, CP_STAT, kCpStat);
	LOG_REGISTER(gpu, CP_BUSY_STAT, kCpBusyStat);
	LOG_REGISTER(gpu, CP_STALLED_STAT1, kCpStalledStat1);
	LOG_REGISTER(gpu, CP_STALLED_STAT2, kCpStalledStat2);
	LOG_REGISTER(gpu, CP_STALLED_STAT3, kCpStalledStat3);
	LOG_REGISTER(gpu, CP_CPF_STATUS, kCpCpfStatus);
	LOG_REGISTER(gpu, CP_CPF_BUSY_STAT, kCpCpfBusyStat);
	LOG_REGISTER(gpu, CP_CPF_STALLED_STAT1, kCpCpfStalledStat1);
	LOG_REGISTER(gpu, CP_CPC_STATUS, kCpCpcStatus);
	LOG_REGISTER(gpu, CP_ME_CNTL, kCpMeCntl);
	LOG_REGISTER(gpu, CP_INT_STATUS, kCpIntStatus);
	log_cp_pointers(gpu);

	// The 3D pipeline, front to back (shader engine 0, the only one).
	LOG_REGISTER(gpu, WD_CNTL_STATUS, kWdCntlStatus);
	LOG_REGISTER(gpu, IA_CNTL_STATUS, kIaCntlStatus);
	LOG_REGISTER(gpu, VGT_CNTL_STATUS, kVgtCntlStatus);
	LOG_REGISTER(gpu, SPI_DEBUG_BUSY, kSpiDebugBusy);
	LOG_REGISTER(gpu, PA_CL_CNTL_STATUS, kPaClCntlStatus);
	LOG_REGISTER(gpu, PA_SU_CNTL_STATUS, kPaSuCntlStatus);
	LOG_REGISTER(gpu, GDS_CNTL_STATUS, kGdsCntlStatus);

	// Memory: a VM protection fault stalls the client that took it.
	log_vm_fault(gpu, 0, mmVM_CONTEXT0_PROTECTION_FAULT_STATUS,
		mmVM_CONTEXT0_PROTECTION_FAULT_ADDR,
		mmVM_CONTEXT0_PROTECTION_FAULT_MCCLIENT);
	log_vm_fault(gpu, 1, mmVM_CONTEXT1_PROTECTION_FAULT_STATUS,
		mmVM_CONTEXT1_PROTECTION_FAULT_ADDR,
		mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT);

	// Last, as it is the only part that writes (index registers): should
	// a read through them hang the bus, everything above is already out.
	log_waves(gpu);

	Log("hang: --- end ---");
}


}	// namespace gfx8
