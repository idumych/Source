/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "Gpu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ByteOrder.h>
#include <FindDirectory.h>
#include <OS.h>

#include <radeon_hd.h>

#include "FirmwareHeaders.h"

#include "gmc_8_1_d.h"
#include "smu_8_0_d.h"


namespace gfx8 {


static const size_t kMegabyte = 1024 * 1024;
static const size_t kSafetyMargin = 16 * kMegabyte;


static inline uint64
round_up(uint64 value, uint64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


void
Log(const char* format, ...)
{
	static int sDprintf = -2;
	if (sDprintf == -2)
		sDprintf = open("/dev/dprintf", O_WRONLY);

	char line[512];
	int length = snprintf(line, sizeof(line), "gfx8: ");

	va_list args;
	va_start(args, format);
	vsnprintf(line + length, sizeof(line) - length - 1, format, args);
	va_end(args);

	length = strlen(line);
	if (length == 0 || line[length - 1] != '\n') {
		line[length++] = '\n';
		line[length] = '\0';
	}

	fputs(line, stdout);
	fflush(stdout);
		// so that the last line before a hang is out
	if (sDprintf >= 0)
		write(sDprintf, line, length);
}


//	#pragma mark - Gpu


Gpu::Gpu()
	:
	fDevice(-1),
	fSharedArea(-1),
	fRegsArea(-1),
	fFrameBufferArea(-1),
	fShared(NULL),
	fRegs(NULL),
	fFrameBuffer(NULL),
	fFrameBufferSize(0),
	fVramStart(0),
	fNextFree(0)
{
}


Gpu::~Gpu()
{
	if (fFrameBufferArea >= 0)
		delete_area(fFrameBufferArea);
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
	if (fSharedArea >= 0)
		delete_area(fSharedArea);
	if (fDevice >= 0)
		close(fDevice);
}


status_t
Gpu::Init()
{
	// The first radeon_hd device (by name), e.g. radeon_hd_000100.
	DIR* dir = opendir("/dev/graphics");
	if (dir == NULL) {
		Log("init: no /dev/graphics: %s", strerror(errno));
		return B_ENTRY_NOT_FOUND;
	}

	char name[B_FILE_NAME_LENGTH] = "";
	while (struct dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "radeon_hd_", 10) != 0)
			continue;
		if (name[0] == '\0' || strcmp(entry->d_name, name) < 0)
			strlcpy(name, entry->d_name, sizeof(name));
	}
	closedir(dir);

	if (name[0] == '\0') {
		Log("init: no radeon_hd device in /dev/graphics");
		return B_ENTRY_NOT_FOUND;
	}

	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "/dev/graphics/%s", name);
	fDevice = open(path, O_RDWR);
	if (fDevice < 0) {
		Log("init: cannot open %s: %s", path, strerror(errno));
		return errno;
	}

	radeon_get_private_data data;
	data.magic = RADEON_PRIVATE_DATA_MAGIC;
	if (ioctl(fDevice, RADEON_GET_PRIVATE_DATA, &data,
			sizeof(radeon_get_private_data)) != 0) {
		Log("init: RADEON_GET_PRIVATE_DATA failed: %s", strerror(errno));
		return B_ERROR;
	}

	fSharedArea = clone_area("gfx8 radeon_hd shared info", (void**)&fShared,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (fSharedArea < 0) {
		Log("init: cannot clone the shared info: %s", strerror(fSharedArea));
		return fSharedArea;
	}

	void* regs;
	fRegsArea = clone_area("gfx8 radeon_hd regs", &regs, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, fShared->registers_area);
	if (fRegsArea < 0) {
		Log("init: cannot clone the registers: %s", strerror(fRegsArea));
		return fRegsArea;
	}
	fRegs = (volatile uint32*)regs;

	fFrameBufferSize = (size_t)fShared->frame_buffer_size * 1024;
	void* frameBuffer;
	fFrameBufferArea = clone_area("gfx8 radeon_hd frame buffer", &frameBuffer,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, fShared->frame_buffer_area);
	if (fFrameBufferArea >= 0) {
		fFrameBuffer = (uint8*)frameBuffer;
	} else {
		// radeon_hd maps the frame buffer with user protection but without
		// B_CLONEABLE_AREA; app_server uses the kernel's mapping directly,
		// so do the same.
		Log("init: cannot clone the frame buffer (%s), using radeon_hd's "
			"mapping at %p", strerror(fFrameBufferArea),
			fShared->frame_buffer);
		fFrameBuffer = fShared->frame_buffer;
		if (fFrameBuffer == NULL)
			return fFrameBufferArea;
	}

	Log("init: %s, %s (pci 1002:%04" B_PRIx32 " rev %#" B_PRIx32
		"), DCE %u.%u", path, fShared->chipsetName, fShared->pciID,
		fShared->pciRev, fShared->dceMajor, fShared->dceMinor);

	const display_mode& mode = fShared->current_mode;
	Log("init: display %ux%u, %" B_PRIu32 " bits per pixel, %" B_PRIu32
		" bytes per row, %" B_PRIu32 " kHz", mode.virtual_width,
		mode.virtual_height, fShared->bits_per_pixel, fShared->bytes_per_row,
		mode.timing.pixel_clock);

	// Where "VRAM" (the carve-out) is in the GPU's MC address space, as
	// gmc_v8_0_vram_gtt_location(): FB_BASE is in 16 MB units.
	uint32 fbLocation = Read(mmMC_VM_FB_LOCATION);
	uint32 fbOffset = Read(mmMC_VM_FB_OFFSET);
	fVramStart = (uint64)(fbLocation & 0xffff) << 24;
	uint64 vramTop = ((uint64)(fbLocation >> 16) << 24) | 0xffffff;
	Log("init: MC_VM_FB_LOCATION %#010" B_PRIx32 ": VRAM at MC %#" B_PRIx64
		"-%#" B_PRIx64 " (%" B_PRIu64 " MB)", fbLocation, fVramStart,
		vramTop, (vramTop + 1 - fVramStart) / kMegabyte);
	Log("init: MC_VM_FB_OFFSET %#010" B_PRIx32 " (system address %#"
		B_PRIx64 " if in 4 MB units)", fbOffset, (uint64)fbOffset << 22);
	Log("init: frame buffer BAR at %#" B_PRIxADDR ", %" B_PRIuSIZE
		" MB mapped at %p; assuming BAR offset 0 = MC %#" B_PRIx64,
		fShared->frame_buffer_phys, fFrameBufferSize / kMegabyte,
		fFrameBuffer, fVramStart);
	if (vramTop < fVramStart
		|| vramTop + 1 - fVramStart < (uint64)fFrameBufferSize) {
		Log("init: warning: the BAR is larger than the MC's VRAM range");
	}

	// radeon_hd programs the scanout at its own idea of the VRAM start,
	// which on DCE 10/11 APUs is the firmware's FB_BASE (gpu.cpp,
	// radeon_gpu_mc_init()). Check that it matches ours.
	static const uint32 kCrtcOffsets[] = {
		EVERGREEN_CRTC0_REGISTER_OFFSET, EVERGREEN_CRTC1_REGISTER_OFFSET
	};
	for (uint32 crtc = 0; crtc < B_COUNT_OF(kCrtcOffsets); crtc++) {
		uint32 offset = kCrtcOffsets[crtc];
		uint32 enable = Read((EVERGREEN_GRPH_ENABLE + offset) / 4);
		uint64 scanout
			= Read((EVERGREEN_GRPH_PRIMARY_SURFACE_ADDRESS + offset) / 4)
			| ((uint64)Read(
				(EVERGREEN_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + offset) / 4)
				<< 32);
		Log("init: crtc %" B_PRIu32 ": grph %s, scanout at MC %#" B_PRIx64,
			crtc, (enable & 1) != 0 ? "on" : "off", scanout);
		if ((enable & 1) != 0 && scanout != fVramStart) {
			Log("init: warning: crtc %" B_PRIu32 " does not scan out from "
				"BAR offset 0 (%+" B_PRId64 ")", crtc,
				(int64)(scanout - fVramStart));
		}
	}

	uint64 screenSize = (uint64)fShared->bytes_per_row * Height();
	fNextFree = round_up(screenSize, kMegabyte) + kSafetyMargin;
	if (fNextFree >= fFrameBufferSize) {
		Log("init: no room for GPU memory above the screen (%" B_PRIu64
			" bytes)", screenSize);
		return B_NO_MEMORY;
	}
	Log("init: GPU memory from BAR offset %#" B_PRIxSIZE " (MC %#" B_PRIx64
		"), %" B_PRIuSIZE " MB free", fNextFree, fVramStart + fNextFree,
		(fFrameBufferSize - fNextFree) / kMegabyte);

	return B_OK;
}


uint32
Gpu::Read(uint32 reg) const
{
	return fRegs[reg];
}


void
Gpu::Write(uint32 reg, uint32 value)
{
	fRegs[reg] = value;
}


void
Gpu::Modify(uint32 reg, uint32 mask, uint32 value)
{
	fRegs[reg] = (fRegs[reg] & ~mask) | (value & mask);
}


uint32
Gpu::ReadSmc(uint32 reg)
{
	// as Linux's cz_smc_rreg() (vi.c)
	Write(mmMP0PUB_IND_INDEX, reg);
	return Read(mmMP0PUB_IND_DATA);
}


void
Gpu::WriteSmc(uint32 reg, uint32 value)
{
	Write(mmMP0PUB_IND_INDEX, reg);
	Write(mmMP0PUB_IND_DATA, value);
}


status_t
Gpu::Wait(uint32 reg, uint32 mask, uint32 value, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (true) {
		if ((Read(reg) & mask) == value)
			return B_OK;
		if (system_time() > end)
			break;
		snooze(10);
	}

	// one last look, in case we were descheduled past the end
	return (Read(reg) & mask) == value ? B_OK : B_TIMED_OUT;
}


status_t
Gpu::Allocate(size_t size, GpuMemory& memory, size_t alignment)
{
	if (alignment < 4096)
		alignment = 4096;
	uint64 rounded = round_up(size, 4096);
	uint64 offset = round_up(fNextFree, alignment);
	if (size == 0 || offset + rounded > fFrameBufferSize) {
		Log("allocate: %" B_PRIuSIZE " bytes do not fit (next free %#"
			B_PRIxSIZE ", frame buffer %#" B_PRIxSIZE ")", size, fNextFree,
			fFrameBufferSize);
		return B_NO_MEMORY;
	}

	memory.cpu = fFrameBuffer + offset;
	memory.gpu = fVramStart + offset;
	memory.size = rounded;
	memset(memory.cpu, 0, rounded);
	fNextFree = offset + rounded;
	return B_OK;
}


uint32
Gpu::BytesPerRow() const
{
	return fShared->bytes_per_row;
}


uint32
Gpu::Width() const
{
	return fShared->current_mode.virtual_width;
}


uint32
Gpu::Height() const
{
	return fShared->current_mode.virtual_height;
}


uint32
Gpu::BitsPerPixel() const
{
	return fShared->bits_per_pixel;
}


uint32
Gpu::PciId() const
{
	return fShared->pciID;
}


static status_t
read_file(const char* path, uint8*& data, size_t& size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno;

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return B_BAD_DATA;
	}

	size = st.st_size;
	data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != (ssize_t)size) {
		free(data);
		data = NULL;
		return bytesRead < 0 ? errno : B_IO_ERROR;
	}
	return B_OK;
}


static bool
has_jump_table(const char* name)
{
	// The CP firmware has a gfx_firmware_header_v1_0; RLC, SDMA and the
	// rest have other headers.
	static const char* kSuffixes[] = {
		"_ce.bin", "_pfp.bin", "_me.bin", "_mec.bin", "_mec2.bin"
	};
	size_t length = strlen(name);
	for (size_t i = 0; i < B_COUNT_OF(kSuffixes); i++) {
		size_t suffixLength = strlen(kSuffixes[i]);
		if (length >= suffixLength
			&& strcmp(name + length - suffixLength, kSuffixes[i]) == 0) {
			return true;
		}
	}
	return false;
}


status_t
Gpu::LoadFirmware(const char* name, Firmware& fw)
{
	memset(&fw, 0, sizeof(fw));
	fw.name = name;

	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY,
		B_USER_NONPACKAGED_DATA_DIRECTORY
	};

	char path[B_PATH_NAME_LENGTH];
	status_t status = B_ENTRY_NOT_FOUND;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
		char directory[B_PATH_NAME_LENGTH];
		if (find_directory(kDirectories[i], -1, false, directory,
				sizeof(directory)) != B_OK) {
			continue;
		}
		if (snprintf(path, sizeof(path), "%s/firmware/amdgpu/%s", directory,
				name) >= (int)sizeof(path)) {
			continue;
		}
		status = read_file(path, fw.data, fw.size);
		if (status == B_OK)
			break;
	}
	if (status != B_OK) {
		Log("firmware: %s not found in firmware/amdgpu of the data "
			"directories: %s", name, strerror(status));
		return status;
	}

	if (fw.size < sizeof(common_firmware_header)) {
		Log("firmware: %s: too small (%" B_PRIuSIZE " bytes)", path, fw.size);
		FreeFirmware(fw);
		return B_BAD_DATA;
	}

	const common_firmware_header* header
		= (const common_firmware_header*)fw.data;
	uint32 headerSize = B_LENDIAN_TO_HOST_INT32(header->header_size_bytes);
	fw.ucodeVersion = B_LENDIAN_TO_HOST_INT32(header->ucode_version);
	fw.ucodeOffset = B_LENDIAN_TO_HOST_INT32(header->ucode_array_offset_bytes);
	fw.ucodeSize = B_LENDIAN_TO_HOST_INT32(header->ucode_size_bytes);
	fw.headerMajor = B_LENDIAN_TO_HOST_INT16(header->header_version_major);
	fw.headerMinor = B_LENDIAN_TO_HOST_INT16(header->header_version_minor);

	if (headerSize > fw.size || fw.ucodeOffset > fw.size
		|| fw.ucodeSize > fw.size - fw.ucodeOffset) {
		Log("firmware: %s: bad header (header %" B_PRIu32 ", ucode %"
			B_PRIu32 " bytes at %" B_PRIu32 ", file %" B_PRIuSIZE ")", path,
			headerSize, fw.ucodeSize, fw.ucodeOffset, fw.size);
		FreeFirmware(fw);
		return B_BAD_DATA;
	}

	if (has_jump_table(name) && fw.headerMajor == 1
		&& headerSize >= sizeof(gfx_firmware_header_v1_0)) {
		const gfx_firmware_header_v1_0* gfxHeader
			= (const gfx_firmware_header_v1_0*)fw.data;
		fw.jtOffset = B_LENDIAN_TO_HOST_INT32(gfxHeader->jt_offset);
		fw.jtSize = B_LENDIAN_TO_HOST_INT32(gfxHeader->jt_size);
		if (((uint64)fw.jtOffset + fw.jtSize) * 4 > fw.ucodeSize) {
			Log("firmware: %s: jump table (%" B_PRIu32 " dwords at %" B_PRIu32
				") is outside the ucode", path, fw.jtSize, fw.jtOffset);
			FreeFirmware(fw);
			return B_BAD_DATA;
		}
	}

	Log("firmware: %s: %" B_PRIuSIZE " bytes, header v%" B_PRIu32 ".%"
		B_PRIu32 ", ucode version %#" B_PRIx32 ", %" B_PRIu32 " bytes at %"
		B_PRIu32 ", jump table %" B_PRIu32 " dwords at %" B_PRIu32, path,
		fw.size, fw.headerMajor, fw.headerMinor, fw.ucodeVersion,
		fw.ucodeSize, fw.ucodeOffset, fw.jtSize, fw.jtOffset);
	return B_OK;
}


/*static*/ void
Gpu::FreeFirmware(Firmware& fw)
{
	free(fw.data);
	fw.data = NULL;
	fw.size = 0;
}


}	// namespace gfx8
