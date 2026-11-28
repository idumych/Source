/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


// gfx8test: brings up the 3D engine of an AMD GFX8 APU (Stoney Ridge) step
// by step, next to radeon_hd, and stops at the first step that fails.


#include <stdio.h>
#include <string.h>

#include "Gpu.h"
#include "Steps.h"


using namespace gfx8;


static const uint32 kFillWidth = 200;
static const uint32 kFillHeight = 120;
static const uint32 kFireOrange = 0xffff6a13;	// B_RGB32


static void
print_usage(const char* name)
{
	fprintf(stderr, "Usage: %s [--probe]\n"
		"  --probe  only open the device and log its state (read-only)\n"
		"  without options, run all the bring-up steps\n", name);
}


/*!	Converts an ARGB 8888 color to a dword of pixels in the frame buffer's
	format. Returns false for formats a dword fill cannot do.
*/
static bool
frame_buffer_color(uint32 bitsPerPixel, uint32 argb, uint32& color)
{
	uint32 red = (argb >> 16) & 0xff;
	uint32 green = (argb >> 8) & 0xff;
	uint32 blue = argb & 0xff;

	switch (bitsPerPixel) {
		case 32:
			color = argb;
			return true;
		case 16:
			color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
			color |= color << 16;
			return true;
		case 15:
			color = ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3);
			color |= color << 16;
			return true;
		default:
			return false;
	}
}


static status_t
fill(Gpu& gpu, Ring& ring, uint32 bitsPerPixel)
{
	uint32 color;
	if (!frame_buffer_color(bitsPerPixel, kFireOrange, color)) {
		Log("fill: %" B_PRIu32 " bits per pixel is not supported",
			bitsPerPixel);
		return B_NOT_SUPPORTED;
	}
	if (gpu.Width() < kFillWidth || gpu.Height() < kFillHeight)
		return B_BAD_VALUE;

	// even, so that 16 bit pixels start on a dword
	uint32 x = (gpu.Width() - kFillWidth) / 2 & ~1u;
	uint32 y = (gpu.Height() - kFillHeight) / 2;
	Log("fill: %" B_PRIu32 "x%" B_PRIu32 " at %" B_PRIu32 ",%" B_PRIu32
		", color %#010" B_PRIx32, kFillWidth, kFillHeight, x, y, color);
	return TestFill(gpu, ring, x, y, kFillWidth, kFillHeight, color);
}


int
main(int argc, char** argv)
{
	bool probeOnly = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--probe") == 0)
			probeOnly = true;
		else {
			print_usage(argv[0]);
			return 1;
		}
	}

	Gpu gpu;
	Ring ring;
	memset(&ring, 0, sizeof(ring));

	enum {
		kInit, kProbe, kSmu, kStartGfx, kTestRing, kTestFence, kTestFill,
		kStepCount
	};
	static const char* kNames[kStepCount] = {
		"init", "probe", "load firmware through smu", "start gfx",
		"test ring", "test fence", "test fill"
	};
	int stepCount = probeOnly ? kProbe + 1 : kStepCount;

	status_t status = B_OK;
	for (int step = 0; step < stepCount; step++) {
		Log("step %d %s: starting", step + 1, kNames[step]);
		switch (step) {
			case kInit:
				status = gpu.Init();
				break;
			case kProbe:
				status = Probe(gpu);
				break;
			case kSmu:
				status = LoadFirmwareThroughSmu(gpu);
				break;
			case kStartGfx:
				status = StartGfx(gpu, ring);
				break;
			case kTestRing:
				status = TestRing(gpu, ring);
				break;
			case kTestFence:
				status = TestFence(gpu, ring);
				break;
			case kTestFill:
				status = fill(gpu, ring, gpu.BitsPerPixel());
				break;
		}

		if (status == B_OK)
			Log("step %d %s: ok", step + 1, kNames[step]);
		else {
			Log("step %d %s: failed (%s)", step + 1, kNames[step],
				strerror(status));
			return 1;
		}
	}

	return 0;
}
