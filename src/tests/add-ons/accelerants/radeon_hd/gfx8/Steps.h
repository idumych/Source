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

// Copies a rectangle of the visible frame buffer to another place on it with
// CP DMA, then compares the two through the CPU. They must not overlap.
status_t TestCopy(Gpu& gpu, Ring& ring, uint32 srcX, uint32 srcY,
	uint32 dstX, uint32 dstY, uint32 width, uint32 height);

// Draw.cpp: draws an upright filled triangle in the square (x, y, size) of
// the visible frame buffer with the 3D pipeline (a vertex and a pixel
// shader, DRAW_INDEX_AUTO), then checks a pixel inside it (must be `color`,
// a B_RGB32 value) and one outside it (must be unchanged) through the CPU.
status_t TestTriangle(Gpu& gpu, Ring& ring, uint32 x, uint32 y, uint32 size,
	uint32 color);


}	// namespace gfx8


#endif	// GFX8_STEPS_H
