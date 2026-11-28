/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 *
 * The structures below are taken from Linux, which carries this notice:
 *
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Ported-from: Linux v6.12, drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h
 * Ported-from-license: MIT
 */
#ifndef GFX8_FIRMWARE_HEADERS_H
#define GFX8_FIRMWARE_HEADERS_H


// The headers at the start of the amdgpu firmware files (all little endian).


#include <SupportDefs.h>


namespace gfx8 {


struct common_firmware_header {
	uint32	size_bytes;					// header and image(s)
	uint32	header_size_bytes;			// just the header
	uint16	header_version_major;
	uint16	header_version_minor;
	uint16	ip_version_major;
	uint16	ip_version_minor;
	uint32	ucode_version;
	uint32	ucode_size_bytes;
	uint32	ucode_array_offset_bytes;	// from the start of the file
	uint32	crc32;						// of the payload
};

// version 1.0: ce, pfp, me, mec, mec2
struct gfx_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32	ucode_feature_version;
	uint32	jt_offset;					// jump table, in dwords
	uint32	jt_size;					// in dwords
};


}	// namespace gfx8


#endif	// GFX8_FIRMWARE_HEADERS_H
