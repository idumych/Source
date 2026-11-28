/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef GFX8_GPU_H
#define GFX8_GPU_H


// The shared interface of gfx8test, a userland bring-up test for the 3D
// engine of AMD GFX8 APUs (Stoney Ridge, 1002:98e4), on top of radeon_hd.
// radeon_hd keeps driving the display; this tool borrows its register and
// frame buffer areas.


#include <OS.h>
#include <SupportDefs.h>


struct radeon_shared_info;


namespace gfx8 {


// Memory the GPU can reach: a piece of the frame buffer BAR (the APU's
// "VRAM", a carve-out of system RAM), above what the display uses.
struct GpuMemory {
	void*		cpu;		// where this process sees it (write-combined)
	uint64		gpu;		// its address in the GPU's memory controller (MC)
	size_t		size;
};


// A firmware image read from disk, with its amdgpu common header parsed.
struct Firmware {
	const char*	name;		// e.g. "stoney_pfp.bin"
	uint8*		data;		// the whole file (malloc'ed)
	size_t		size;
	// from struct common_firmware_header
	uint32		ucodeVersion;
	uint32		ucodeOffset;	// ucode_array_offset_bytes
	uint32		ucodeSize;		// ucode_size_bytes
	uint32		headerMajor;
	uint32		headerMinor;
	// for gfx firmware (struct gfx_firmware_header_v1_0): jump table
	uint32		jtOffset;	// in dwords, 0 if none
	uint32		jtSize;		// in dwords
};


class Gpu {
public:
								Gpu();
								~Gpu();

	// Opens the first radeon_hd device and clones its areas.
			status_t			Init();

	// Registers, by dword offset as in AMD's register headers (mmFOO).
			uint32				Read(uint32 reg) const;
			void				Write(uint32 reg, uint32 value);
			void				Modify(uint32 reg, uint32 mask, uint32 value);
				// reg = (reg & ~mask) | (value & mask)

	// The SMU's indirect registers (through mmMP0PUB_IND_INDEX/DATA, the
	// "SMC" index/data pair Linux uses for VI APUs).
			uint32				ReadSmc(uint32 reg);
			void				WriteSmc(uint32 reg, uint32 value);

	// Polls until (Read(reg) & mask) == value, or the timeout (in
	// microseconds) passes. Returns B_OK or B_TIMED_OUT.
			status_t			Wait(uint32 reg, uint32 mask, uint32 value,
									bigtime_t timeout = 1000000);

	// Takes `size` bytes (rounded up to 4 KB, aligned to `alignment`) from
	// the unused tail of the frame buffer. Never freed: a test runs once.
			status_t			Allocate(size_t size, GpuMemory& memory,
									size_t alignment = 4096);

	// The visible frame buffer, for drawing tests.
			void*				FrameBuffer() const { return fFrameBuffer; }
			uint64				FrameBufferGpu() const { return fVramStart; }
			uint32				BytesPerRow() const;
			uint32				Width() const;
			uint32				Height() const;
			uint32				BitsPerPixel() const;

	// Reads firmware/amdgpu/<name> from the system's data directories
	// (non-packaged first) and parses its common header.
			status_t			LoadFirmware(const char* name, Firmware& fw);
	static	void				FreeFirmware(Firmware& fw);

	// MC address of the start of "VRAM", from mmMC_VM_FB_LOCATION.
			uint64				VramStart() const { return fVramStart; }
			uint32				PciId() const;

private:
			int					fDevice;
			area_id				fSharedArea;
			area_id				fRegsArea;
			area_id				fFrameBufferArea;
			struct radeon_shared_info* fShared;
			volatile uint32*	fRegs;
			uint8*				fFrameBuffer;
			size_t				fFrameBufferSize;
			uint64				fVramStart;
			size_t				fNextFree;		// offset into the frame buffer
};


// Prints a step's result to stdout and the syslog (/dev/dprintf), e.g.
// Log("smu: TOC loaded, %u entries", count). Lines start with "gfx8: ".
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));


}	// namespace gfx8


#endif	// GFX8_GPU_H
