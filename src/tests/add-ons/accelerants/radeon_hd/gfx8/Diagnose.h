/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef GFX8_DIAGNOSE_H
#define GFX8_DIAGNOSE_H


#include "Gpu.h"


namespace gfx8 {


// Logs, decoded bit by bit, the registers that tell where the 3D engine is
// stuck: GRBM/SRBM status, the CP's busy and stalled bits, the ring and IB
// pointers, the 3D pipeline blocks, VM faults, and the state of every live
// shader wave. Lines start with "gfx8: hang: ".
//
// It only reads, except for two index registers it needs for the wave
// reads, last of all: GRBM_GFX_INDEX (left at broadcast to all, as Linux and
// StartGfx leave it) and SQ_IND_INDEX. Reading the CP header dumps advances
// their read position, as in GfxRing's own state dump.
void DumpGfxHang(Gpu& gpu, const char* why);


}	// namespace gfx8


#endif	// GFX8_DIAGNOSE_H
