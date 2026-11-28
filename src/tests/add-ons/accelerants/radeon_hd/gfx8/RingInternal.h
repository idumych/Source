/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef GFX8_RING_INTERNAL_H
#define GFX8_RING_INTERNAL_H


// GfxRing.cpp's ring helpers, for the steps in other files that put packets
// on gfx ring 0.


#include "Steps.h"


namespace gfx8 {


static inline uint32
ring_mask(const Ring& ring)
{
	return ring.buffer.size / 4 - 1;
}


static inline void
ring_write(Ring& ring, uint32 value)
{
	((volatile uint32*)ring.buffer.cpu)[ring.wptr] = value;
	ring.wptr = (ring.wptr + 1) & ring_mask(ring);
}


// CPU writes through the BAR go through the HDP; make them visible to the
// GPU (vi_flush_hdp) or drop stale HDP read data (vi_invalidate_hdp).
void flush_hdp(Gpu& gpu);
void invalidate_hdp(Gpu& gpu);

// Logs the CP, RLC and VM state that explains a stuck ring.
void dump_state(Gpu& gpu, Ring& ring, const char* why);

// Waits until `count` dwords (plus the commit padding) fit in the ring.
status_t ring_alloc(Gpu& gpu, Ring& ring, uint32 count);

// Pads the ring with NOPs and hands the new wptr to the CP.
void ring_commit(Gpu& gpu, Ring& ring);

// Emits an end-of-pipe fence that flushes and invalidates the CB/DB and TC
// caches, commits, and waits for it.
status_t fence_and_wait(Gpu& gpu, Ring& ring, const char* what);


}	// namespace gfx8


#endif	// GFX8_RING_INTERNAL_H
