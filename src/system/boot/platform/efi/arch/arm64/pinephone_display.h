/*
 * Copyright 2026, Ian Dumych. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef EFI_ARM64_PINEPHONE_DISPLAY_H
#define EFI_ARM64_PINEPHONE_DISPLAY_H


#include <SupportDefs.h>


// The PinePhone's firmware (Tow-Boot) leaves the panel off and offers no
// GOP. On a PinePhone this brings up the panel and fills in
// gKernelArgs.frame_buffer, which the kernel and app_server then use as on
// any EFI machine.
status_t pinephone_display_init();

// Whether pinephone_display_init() lit the panel.
bool pinephone_display_active();

// Makes what the loader drew visible to the display engine.
void pinephone_display_flush();

// Stops copying loader output (below) and clears the panel, for the splash.
void pinephone_display_clear();

// Copies loader output onto the panel until the boot splash goes up, so a
// loader that stops (at its boot menu, for example) can be seen without the
// serial cable.
void pinephone_display_mirror(const char* text, size_t length);

// The last loader step: shows on the LED that the kernel is being started.
void pinephone_display_starting_kernel();


#endif	// EFI_ARM64_PINEPHONE_DISPLAY_H
