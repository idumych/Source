/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef GFX8_STEPS_H
#define GFX8_STEPS_H


// The bring-up steps, each in its own file. Every step logs what it did and
// returns B_OK, or an error after logging the registers that explain it.


#include "Gpu.h"


namespace gfx8 {


// Probe.cpp: reads and logs the state of the GPU; changes nothing.
status_t Probe(Gpu& gpu);

// Smu8.cpp: loads the CP (ce, pfp, me, mec), RLC and SDMA firmware through
// the SMU, as Linux's smu8_smumgr does for Stoney (AMDGPU_FW_LOAD_SMU):
// builds the table of contents in GPU memory and runs its jobs.
status_t LoadFirmwareThroughSmu(Gpu& gpu);

// GfxRing.cpp: the 3D engine.
struct Ring {
	GpuMemory	buffer;			// the ring itself (power of two dwords)
	GpuMemory	writeBack;		// rptr write-back and fence dwords
	uint32		wptr;			// in dwords
	uint32		fenceSequence;
};

// Golden registers for Stoney, then RLC start and CP gfx ring 0.
status_t StartGfx(Gpu& gpu, Ring& ring);

// Writes 0xDEADBEEF to a scratch register through the ring and reads it
// back (as Linux's gfx_v8_0_ring_test_ring).
status_t TestRing(Gpu& gpu, Ring& ring);

// WRITE_DATA to memory, then an end-of-pipe fence; waits for both.
status_t TestFence(Gpu& gpu, Ring& ring);

// Fills a rectangle of the visible frame buffer with CP DMA.
status_t TestFill(Gpu& gpu, Ring& ring, uint32 x, uint32 y, uint32 width,
	uint32 height, uint32 color);


}	// namespace gfx8


#endif	// GFX8_STEPS_H
