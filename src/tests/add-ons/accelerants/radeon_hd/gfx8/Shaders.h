/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef GFX8_SHADERS_H
#define GFX8_SHADERS_H


// The two shaders of the first triangle, as GCN3 (gfx810) machine code. The
// source and the commands that regenerate the arrays are in Shaders.cpp.


#include <SupportDefs.h>


namespace gfx8 {


struct ShaderBinary {
	const uint32*	code;		// GCN3 machine code, little-endian dwords
	uint32			dwords;
	uint32			rsrc1;		// SPI_SHADER_PGM_RSRC1_{VS,PS} value
	uint32			rsrc2;		// SPI_SHADER_PGM_RSRC2_{VS,PS} value
	uint32			userSgprs;	// how many user-data SGPRs it reads
};

// Vertex shader, no vertex buffers: v0 = vertex index (0, 1, 2), user SGPRs
// s0..s5 = x0, y0, x1, y1, x2, y2 (floats). Exports POS0 = (x, y, 0, 1) and
// PARAM0 = (0, 0, 0, 1).
extern const ShaderBinary kTriangleVs;

// Pixel shader: user SGPRs s0..s3 = r, g, b, a (floats). Exports MRT0 as
// four 32-bit floats (SPI_SHADER_COL_FORMAT 32_ABGR). Needs PERSP_CENTER
// enabled in SPI_PS_INPUT_ENA and SPI_PS_INPUT_ADDR; see Shaders.cpp.
extern const ShaderBinary kConstantPs;


}	// namespace gfx8


#endif	// GFX8_SHADERS_H
