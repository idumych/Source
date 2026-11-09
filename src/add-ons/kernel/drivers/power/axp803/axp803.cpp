/*
 * Copyright 2026, Ian Dumych. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*	The X-Powers AXP803 PMIC of the PINE64 PinePhone (Allwinner A64), on the
	SoC's RSB ("reduced serial bus") controller.

	For now its power key: a long press (the PMIC's own long press event,
	1.5 s by default) is published as /dev/power/button/power in
	acpi_button's form: a read gives 1 once after a press, and select()
	reports one; power_daemon then shuts the system down. A short press is
	only logged; it is meant for blanking the screen once there is a way to.

	The key events are bits of the PMIC's interrupt status (bank 5, Linux'
	axp20x.c), polled; the PMIC's interrupt line (through the R_INTC) is
	not needed for that. The boot firmware leaves the RSB set up and the
	PMIC at runtime address 0x2d, as Linux' sunxi-rsb.c would give it; that
	is set up again only if the PMIC does not answer. */


#include <stdio.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <fs/select_sync_pool.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <libfdt.h>


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;

#define TRACE(x...) dprintf("axp803: " x)


// RSB controller
static const uint32 kRsbControl = 0x00;
static const uint32 kRsbClock = 0x04;
static const uint32 kRsbStatus = 0x0c;
static const uint32 kRsbAddress = 0x10;
static const uint32 kRsbData = 0x1c;
static const uint32 kRsbPmicControl = 0x28;
static const uint32 kRsbCommand = 0x2c;
static const uint32 kRsbDeviceAddress = 0x30;
static const uint32 kRsbSoftReset = 1u << 0;
static const uint32 kRsbStart = 1u << 7;
static const uint32 kRsbTransferOver = 1u << 0;
static const uint32 kRsbCommandWrite8 = 0x4e;
static const uint32 kRsbCommandRead8 = 0x8b;
static const uint32 kRsbCommandSetRuntimeAddress = 0xe8;

// the A64's R_PRCM (RSB clock and reset) and R_PIO (RSB pins)
static const phys_addr_t kRPrcm = 0x01f01400;
static const phys_addr_t kRPio = 0x01f02c00;

// AXP803
static const uint32 kRuntimeAddress = 0x2d;
static const uint8 kRegisterChipId = 0x03;
static const uint8 kRegisterIrqEnable5 = 0x44;
static const uint8 kRegisterIrqStatus5 = 0x4c;
static const uint8 kIrqPekShort = 1u << 4;
static const uint8 kIrqPekLong = 1u << 3;

static const char* sDeviceNames[] = { "power/button/power", NULL };

static area_id sRsbArea = -1;
static volatile uint8* sRsb;
static uint32 sHardwareAddress = 0x3a3;

static mutex sLock = MUTEX_INITIALIZER("axp803");
static thread_id sThread = -1;
static volatile bool sQuit;
static uint8 sPressed;
static select_sync_pool* sSelectPool;


static inline uint32
rd(uint32 offset)
{
	return *(volatile uint32*)(sRsb + offset);
}


static inline void
wr(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRsb + offset) = value;
	memory_full_barrier();
}


// #pragma mark - RSB


static status_t
rsb_transfer()
{
	wr(kRsbStatus, rd(kRsbStatus));
		// write-1-to-clear what an earlier transfer left
	wr(kRsbControl, kRsbStart);
	for (int i = 0; i < 100; i++) {
		if ((rd(kRsbControl) & kRsbStart) == 0)
			return rd(kRsbStatus) == kRsbTransferOver ? B_OK : B_IO_ERROR;
		spin(50);
	}
	return B_TIMED_OUT;
}


static status_t
pmic_read(uint8 reg, uint8* value)
{
	wr(kRsbCommand, kRsbCommandRead8);
	wr(kRsbDeviceAddress, kRuntimeAddress << 16);
	wr(kRsbAddress, reg);
	status_t status = rsb_transfer();
	if (status == B_OK)
		*value = rd(kRsbData) & 0xff;
	return status;
}


static status_t
pmic_write(uint8 reg, uint8 value)
{
	wr(kRsbCommand, kRsbCommandWrite8);
	wr(kRsbDeviceAddress, kRuntimeAddress << 16);
	wr(kRsbAddress, reg);
	wr(kRsbData, value);
	return rsb_transfer();
}


static void
modify_page(phys_addr_t base, uint32 offset, uint32 mask, uint32 value)
{
	void* address;
	area_id area = map_physical_memory("axp803 rsb setup", base,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return;
	volatile uint32* reg = (volatile uint32*)((uint8*)address + offset);
	*reg = (*reg & ~mask) | value;
	delete_area(area);
}


/*!	What the boot firmware does: clock and pins of the controller, 3 MHz,
	the PMIC switched from I2C to RSB mode and given its runtime address.
*/
static status_t
rsb_init()
{
	modify_page(kRPrcm, 0x28, 1u << 3, 1u << 3);	// APB0 gate: RSB
	modify_page(kRPrcm, 0xb0, 1u << 3, 1u << 3);	// APB0 reset: RSB
	modify_page(kRPio, 0x00, 0x77, 0x22);			// PL0, PL1: S_RSB

	wr(kRsbControl, kRsbSoftReset);
	for (int i = 0; (rd(kRsbControl) & kRsbSoftReset) != 0; i++) {
		if (i > 100)
			return B_TIMED_OUT;
		spin(10);
	}
	wr(kRsbClock, (1u << 8) | 3);

	wr(kRsbPmicControl, (1u << 31) | (0x7c << 16) | (0x3e << 8));
	for (int i = 0; (rd(kRsbPmicControl) & (1u << 31)) != 0; i++) {
		if (i > 100)
			return B_TIMED_OUT;
		spin(10);
	}

	wr(kRsbCommand, kRsbCommandSetRuntimeAddress);
	wr(kRsbDeviceAddress, (kRuntimeAddress << 16) | sHardwareAddress);
	return rsb_transfer();
}


// #pragma mark - power key


static status_t
poll_thread(void*)
{
	while (!sQuit) {
		uint8 status;
		if (pmic_read(kRegisterIrqStatus5, &status) == B_OK
			&& (status & (kIrqPekShort | kIrqPekLong)) != 0) {
			pmic_write(kRegisterIrqStatus5,
				status & (kIrqPekShort | kIrqPekLong));
			if ((status & kIrqPekLong) != 0) {
				TRACE("power key held\n");
				MutexLocker locker(sLock);
				sPressed = 1;
				if (sSelectPool != NULL)
					notify_select_event_pool(sSelectPool, B_SELECT_READ);
			} else
				TRACE("power key pressed\n");
		}
		snooze(100000);
	}
	return B_OK;
}


// #pragma mark - device


static status_t
axp803_open(const char* name, uint32 flags, void** _cookie)
{
	*_cookie = NULL;
	return B_OK;
}


static status_t
axp803_close(void* cookie)
{
	return B_OK;
}


static status_t
axp803_free(void* cookie)
{
	return B_OK;
}


static status_t
axp803_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
axp803_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	if (*_length < 1)
		return B_IO_ERROR;

	MutexLocker locker(sLock);
	if (user_memcpy(buffer, &sPressed, 1) != B_OK)
		return B_BAD_ADDRESS;
	sPressed = 0;
	*_length = 1;
	return B_OK;
}


static status_t
axp803_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return B_NOT_ALLOWED;
}


static status_t
axp803_select(void* cookie, uint8 event, uint32 ref, selectsync* sync)
{
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);
	status_t status = add_select_sync_pool_entry(&sSelectPool, sync, event);
	if (status == B_OK && sPressed != 0)
		notify_select_event(sync, event);
	return status;
}


static status_t
axp803_deselect(void* cookie, uint8 event, selectsync* sync)
{
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);
	return remove_select_sync_pool_entry(&sSelectPool, sync, event);
}


static device_hooks sDeviceHooks = {
	axp803_open,
	axp803_close,
	axp803_free,
	axp803_control,
	axp803_read,
	axp803_write,
	axp803_select,
	axp803_deselect
};


// #pragma mark - driver


static int
pmic_node()
{
	if (gFDT == NULL)
		return -1;
	int node = fdt_node_offset_by_compatible(gFDT, -1, "x-powers,axp803");
	if (node < 0)
		return -1;
	const char* status = (const char*)fdt_getprop(gFDT, node, "status",
		NULL);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return -1;
	}
	return node;
}


status_t
init_hardware()
{
	return pmic_node() >= 0 ? B_OK : B_DEVICE_NOT_FOUND;
}


status_t
init_driver()
{
	int node = pmic_node();
	if (node < 0)
		return B_DEVICE_NOT_FOUND;

	// the RSB controller is the PMIC's parent node
	int bus = fdt_parent_offset(gFDT, node);
	int length;
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(gFDT, bus, "reg",
		&length);
	if (bus < 0 || reg == NULL || length < 8
		|| fdt_node_check_compatible(gFDT, bus, "allwinner,sun8i-a23-rsb")
			!= 0) {
		TRACE("no RSB controller for the PMIC\n");
		return B_DEVICE_NOT_FOUND;
	}
	phys_addr_t base = fdt32_to_cpu(reg[0]);
	const fdt32_t* address = (const fdt32_t*)fdt_getprop(gFDT, node, "reg",
		NULL);
	if (address != NULL)
		sHardwareAddress = fdt32_to_cpu(*address);

	sRsbArea = map_physical_memory("axp803 rsb", base, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sRsb);
	if (sRsbArea < 0)
		return sRsbArea;

	uint8 chipId;
	status_t status = pmic_read(kRegisterChipId, &chipId);
	if (status != B_OK) {
		TRACE("the PMIC does not answer (%s), setting up the RSB\n",
			strerror(status));
		status = rsb_init();
		if (status == B_OK)
			status = pmic_read(kRegisterChipId, &chipId);
	}
	if (status != B_OK) {
		TRACE("no PMIC on the RSB: %s\n", strerror(status));
		delete_area(sRsbArea);
		sRsbArea = -1;
		return status;
	}

	// let the key events through to the status register, and forget old ones
	uint8 enable = 0;
	pmic_read(kRegisterIrqEnable5, &enable);
	pmic_write(kRegisterIrqEnable5, enable | kIrqPekShort | kIrqPekLong);
	pmic_write(kRegisterIrqStatus5, kIrqPekShort | kIrqPekLong);

	sQuit = false;
	sThread = spawn_kernel_thread(poll_thread, "axp803 power key",
		B_LOW_PRIORITY, NULL);
	if (sThread < 0) {
		delete_area(sRsbArea);
		sRsbArea = -1;
		return sThread;
	}
	resume_thread(sThread);

	TRACE("chip ID %#x, power key on\n", chipId);
	return B_OK;
}


void
uninit_driver()
{
	sQuit = true;
	if (sThread >= 0)
		wait_for_thread(sThread, NULL);
	sThread = -1;
	if (sRsbArea >= 0)
		delete_area(sRsbArea);
	sRsbArea = -1;
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
