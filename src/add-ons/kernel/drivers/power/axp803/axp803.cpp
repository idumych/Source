/*
 * Copyright 2026, Ian Dumych. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*	The X-Powers AXP803 PMIC of the PINE64 PinePhone (Allwinner A64), on the
	SoC's RSB ("reduced serial bus") controller.

	Its power key: a long press (the PMIC's own long press event, 1.5 s by
	default) is published as /dev/power/button/power in acpi_button's form:
	a read gives 1 once after a press, and select() reports one;
	power_daemon then shuts the system down. A short press is only logged;
	it is meant for blanking the screen once there is a way to.

	Its battery: /dev/power/battery/axp803 answers acpi_battery's ioctls,
	which PowerStatus (Deskbar) reads. The charge is the PMIC's fuel gauge
	percentage, reported against the PinePhone battery's nominal capacity;
	current and voltage come from its ADCs, the state follows Linux'
	axp20x_battery.c.

	Its USB power input: the PMIC detects the charger type (USB BC 1.2);
	for a dedicated charger or a charging downstream port, which may give
	1.5 A, the input current limit is raised to that if it is lower, as
	Linux' axp20x_usb_power.c lets user space do. Otherwise the phone,
	which takes more than the 500 mA of a standard port with the panel on,
	discharges while plugged in.

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

#include <device/power_managment.h>


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
static const uint8 kRegisterPowerStatus = 0x00;
static const uint8 kPowerStatusCharging = 1u << 2;
static const uint8 kRegisterPowerMode = 0x01;
static const uint8 kPowerModeBatteryPresent = 1u << 5;
static const uint8 kRegisterBatteryVoltage = 0x78;		// 12 bits, 1.1 mV
static const uint8 kRegisterChargeCurrent = 0x7a;		// 12 bits, 1 mA
static const uint8 kRegisterDischargeCurrent = 0x7c;	// 12 bits, 1 mA
static const uint8 kRegisterAdcEnable = 0x82;
static const uint8 kAdcEnableBattery = 0xe1;
	// battery voltage and current, and the TS pin (bit 0); Linux'
	// AXP22X_ADC_EN1_MASK, which its AXP813 ADC driver writes
static const uint8 kPowerStatusVbusPresent = 1u << 5;
static const uint8 kRegisterChargerDetection = 0x2f;	// bits 7:5
static const uint8 kRegisterChargeColdLimit = 0x38;	// Linux' V_LTF_CHRG
static const uint8 kRegisterChargeHotLimit = 0x39;	// Linux' V_HTF_CHRG
	// TS pin voltages, 12.8 mV steps; the PMIC drives 80 uA into the
	// battery's NTC thermistor (TS control 0xf2) and stops charging outside
	// them. Its defaults (0xa5, 0x1f) suit a 10 kOhm NTC, but the PinePhone's
	// battery reads 249 mV at room temperature, about 3.1 kOhm, and the PMIC
	// took it for too hot to charge. For such an NTC (B about 3435), 0 C is
	// about 8.9 kOhm, 710 mV, and 45 C about 1.5 kOhm, 120 mV.
static const uint8 kChargeColdLimit = 0x37;		// 704 mV
static const uint8 kChargeHotLimit = 0x09;		// 115 mV
static const uint8 kRegisterBcGlobal = 0x2c;
static const uint8 kBcEnable = 1u << 0;
	// USB BC 1.2 charger detection (Linux' AXP813_BC_EN); without it the
	// PinePhone reported every charger type as unknown
static const uint8 kRegisterInputCurrentLimit = 0x35;	// bits 7:4
static const uint32 kInputCurrentLimits[] = {	// mA
	100, 500, 900, 1500, 2000, 2500, 3000, 3500
};
static const uint8 kInputCurrentLimit1500 = 3;
static const char* kChargerTypes[] = {
	"unknown", "standard port", "charging port", "dedicated charger",
	"?", "?", "?", "?"
};
static const uint8 kRegisterFuelGauge = 0xb9;
static const uint8 kFuelGaugeValid = 1u << 7;

// the PinePhone's battery
static const uint32 kNominalCapacity = 3000;	// mAh
static const uint32 kNominalVoltage = 3800;		// mV
static const uint32 kEmptyVoltage = 3000;		// mV, a Li-ion cell at 0 %
static const uint32 kFullVoltage = 4200;		// mV, and at 100 %

static const char* sDeviceNames[] = {
	"power/button/power",
	"power/battery/axp803",
	NULL
};

static area_id sRsbArea = -1;
static volatile uint8* sRsb;
static uint32 sHardwareAddress = 0x3a3;

static mutex sLock = MUTEX_INITIALIZER("axp803");
static mutex sRsbLock = MUTEX_INITIALIZER("axp803 rsb");
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
	MutexLocker locker(sRsbLock);
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
	MutexLocker locker(sRsbLock);
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


struct battery_raw {
	uint8	gauge;
	uint32	voltage;	// mV
	uint32	charge;		// mA
	uint32	discharge;	// mA
	bool	present;
};

static status_t read_battery(acpi_battery_info* info, battery_raw* raw);
static void check_battery(uint32& lastPercent, uint32& lastState);
static status_t battery_info(acpi_battery_info* info);


// #pragma mark - USB power input


/*!	Raises the input current limit to 1.5 A for chargers that may give that,
	and logs what changes.
*/
static void
check_usb_power(uint8& lastPower, uint8& lastType)
{
	uint8 power, detection, limit;
	if (pmic_read(kRegisterPowerStatus, &power) != B_OK
		|| pmic_read(kRegisterChargerDetection, &detection) != B_OK
		|| pmic_read(kRegisterInputCurrentLimit, &limit) != B_OK) {
		return;
	}
	uint8 type = (power & kPowerStatusVbusPresent) != 0 ? detection >> 5 : 0;
	uint8 limitIndex = limit >> 4;

	if ((type == 2 || type == 3) && limitIndex < kInputCurrentLimit1500) {
		limitIndex = kInputCurrentLimit1500;
		pmic_write(kRegisterInputCurrentLimit,
			(limit & 0x0f) | (limitIndex << 4));
		TRACE("input current limit raised to %" B_PRIu32 " mA\n",
			kInputCurrentLimits[limitIndex]);
	}

	if ((power & kPowerStatusVbusPresent) != (lastPower & kPowerStatusVbusPresent)
		|| type != lastType) {
		acpi_battery_info info;
		battery_info(&info);
		TRACE("USB power %s (%s), input limit %" B_PRIu32 " mA, status %#x; "
			"battery %" B_PRIu32 " mA, %s\n",
			(power & kPowerStatusVbusPresent) != 0 ? "present" : "absent",
			kChargerTypes[type & 7], kInputCurrentLimits[limitIndex & 7],
			power, info.current_rate,
			(info.state & BATTERY_CHARGING) != 0 ? "charging"
				: (info.state & BATTERY_DISCHARGING) != 0 ? "discharging"
				: "not charging");
	}
	lastPower = power;
	lastType = type;
}


// #pragma mark - power key


static status_t
poll_thread(void*)
{
	uint8 lastPower = 0xff, lastType = 0xff;
	uint32 lastPercent = UINT32_MAX, lastState = UINT32_MAX;
	int32 round = 0;
	while (!sQuit) {
		// the charger type is known a while after the cable is plugged in
		if (round % 10 == 0)
			check_usb_power(lastPower, lastType);
		// a few seconds after the ADCs were switched on, then every 10 s
		if (round % 100 == 50)
			check_battery(lastPercent, lastState);
		round++;

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


// #pragma mark - battery


/*!	A 12 bit ADC value: the high register, then the low four bits. */
static status_t
read_adc(uint8 reg, uint32* value)
{
	uint8 high, low;
	status_t status = pmic_read(reg, &high);
	if (status == B_OK)
		status = pmic_read(reg + 1, &low);
	if (status == B_OK)
		*value = (uint32)high << 4 | (low & 0x0f);
	return status;
}


/*!	The charge from the voltage alone: linear between an empty and a full
	cell, which reads high in the middle of the curve and while charging. */
static uint32
percent_from_voltage(uint32 voltage)
{
	if (voltage <= kEmptyVoltage)
		return 0;
	if (voltage >= kFullVoltage)
		return 100;
	return (voltage - kEmptyVoltage) * 100 / (kFullVoltage - kEmptyVoltage);
}


static status_t
read_battery(acpi_battery_info* info, battery_raw* raw)
{
	memset(info, 0, sizeof(*info));
	if (raw != NULL)
		memset(raw, 0, sizeof(*raw));

	uint8 power, mode, gauge;
	status_t status = pmic_read(kRegisterPowerStatus, &power);
	if (status == B_OK)
		status = pmic_read(kRegisterPowerMode, &mode);
	if (status == B_OK)
		status = pmic_read(kRegisterFuelGauge, &gauge);
	if (status != B_OK)
		return status;
	if (raw != NULL)
		raw->gauge = gauge;
	if ((mode & kPowerModeBatteryPresent) == 0)
		return B_OK;

	uint32 voltage = 0, charge = 0, discharge = 0;
	read_adc(kRegisterBatteryVoltage, &voltage);
	read_adc(kRegisterChargeCurrent, &charge);
	read_adc(kRegisterDischargeCurrent, &discharge);

	voltage = voltage * 11 / 10;
	if (raw != NULL) {
		raw->voltage = voltage;
		raw->charge = charge;
		raw->discharge = discharge;
		raw->present = true;
	}

	// The fuel gauge can claim a full battery that cannot run the phone; no
	// more than the voltage allows, and only the voltage while the gauge has
	// no value
	uint32 percent = percent_from_voltage(voltage);
	if ((gauge & kFuelGaugeValid) != 0 && (gauge & 0x7f) < percent)
		percent = gauge & 0x7f;
	bool charging = (power & kPowerStatusCharging) != 0;

	if (charging)
		info->state = BATTERY_CHARGING;
	else if (discharge > 0)
		info->state = BATTERY_DISCHARGING;
	else
		info->state = BATTERY_NOT_CHARGING;
	if (!charging && percent <= 5)
		info->state |= BATTERY_CRITICAL_STATE;

	info->capacity = percent * kNominalCapacity / 100;
	info->current_rate = charging ? charge : discharge;
	info->voltage = voltage;
	return B_OK;
}


static status_t
battery_info(acpi_battery_info* info)
{
	return read_battery(info, NULL);
}


static void
check_battery(uint32& lastPercent, uint32& lastState)
{
	acpi_battery_info info;
	battery_raw raw;
	if (read_battery(&info, &raw) != B_OK)
		return;
	uint32 percent = info.capacity * 100 / kNominalCapacity;
	if (percent == lastPercent && info.state == lastState)
		return;
	lastPercent = percent;
	lastState = info.state;
	if (!raw.present) {
		TRACE("no battery (fuel gauge %#x)\n", raw.gauge);
		return;
	}
	TRACE("battery %" B_PRIu32 "%%, %" B_PRIu32 " mV, charge %" B_PRIu32
		" mA, discharge %" B_PRIu32 " mA, fuel gauge %#x, state %#" B_PRIx32
		"\n", percent, raw.voltage, raw.charge, raw.discharge, raw.gauge,
		info.state);

	// the charger's own view: power mode (charging, battery present,
	// activation mode for a deeply discharged cell), VBUS path, charger
	// control 1 and 2 (enable, target voltage and current, timers)
	uint8 mode = 0, path = 0, control1 = 0, control2 = 0;
	pmic_read(kRegisterPowerMode, &mode);
	pmic_read(0x30, &path);
	pmic_read(0x33, &control1);
	pmic_read(0x34, &control2);
	TRACE("charger: power mode %#x, VBUS path %#x, control %#x %#x\n", mode,
		path, control1, control2);

	// why charging stops (register names after Linux' axp20x.h, its AXP803
	// interrupt table and AXP813 ADC driver): the latched interrupt status
	// (0x49 bit 3 charging started, bit 2 done, bit 5 activation mode; 0x4a
	// bit 7 and 5 too hot and too cold to charge), the TS pin's voltage
	// (0x58, 0.8 mV steps) against the charge thresholds (0x38 cold, 0x39
	// hot), the ADC enable and the TS pin control (bias current, when on)
	uint8 irq[5] = {};
	for (int i = 0; i < 5; i++)
		pmic_read(0x48 + i, &irq[i]);
	uint32 ts = 0;
	read_adc(0x58, &ts);
	uint8 coldLimit = 0, hotLimit = 0, adcEnable = 0, tsControl = 0;
	pmic_read(kRegisterChargeColdLimit, &coldLimit);
	pmic_read(kRegisterChargeHotLimit, &hotLimit);
	pmic_read(kRegisterAdcEnable, &adcEnable);
	pmic_read(0x84, &tsControl);
	TRACE("charger: irq status %#x %#x %#x %#x %#x, TS %" B_PRIu32 " mV, "
		"thresholds cold %#x hot %#x, ADC enable %#x, TS control %#x\n",
		irq[0], irq[1], irq[2], irq[3], irq[4], ts * 8 / 10, coldLimit,
		hotLimit, adcEnable, tsControl);
}


static void
extended_battery_info(acpi_extended_battery_info* info)
{
	memset(info, 0, sizeof(*info));
	info->power_unit = ACPI_BATTERY_UNIT_MA;
	info->design_capacity = kNominalCapacity;
	info->last_full_charge = kNominalCapacity;
	info->technology = 1;	// rechargeable
	info->design_voltage = kNominalVoltage;
	info->design_capacity_warning = kNominalCapacity / 10;
	info->design_capacity_low = kNominalCapacity / 20;
	info->capacity_granularity_1 = kNominalCapacity / 100;
	info->capacity_granularity_2 = kNominalCapacity / 100;
	strlcpy(info->model_number, "PinePhone", sizeof(info->model_number));
	strlcpy(info->type, "Li-ion", sizeof(info->type));
	strlcpy(info->oem_info, "PINE64", sizeof(info->oem_info));
	info->revision = ACPI_BATTERY_REVISION_BIF;
}


static status_t
battery_open(const char* name, uint32 flags, void** _cookie)
{
	*_cookie = NULL;
	return B_OK;
}


static status_t
battery_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case IDENTIFY_DEVICE:
		{
			if (length < sizeof(uint32))
				return B_BAD_VALUE;
			uint32 magic = kMagicACPIBatteryID;
			return user_memcpy(buffer, &magic, sizeof(magic));
		}

		case GET_BATTERY_INFO:
		{
			if (length < sizeof(acpi_battery_info))
				return B_BAD_VALUE;
			acpi_battery_info info;
			status_t status = battery_info(&info);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &info, sizeof(info));
		}

		case GET_EXTENDED_BATTERY_INFO:
		{
			if (length < sizeof(acpi_extended_battery_info))
				return B_BAD_VALUE;
			acpi_extended_battery_info info;
			extended_battery_info(&info);
			return user_memcpy(buffer, &info, sizeof(info));
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
battery_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
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


static device_hooks sButtonHooks = {
	axp803_open,
	axp803_close,
	axp803_free,
	axp803_control,
	axp803_read,
	axp803_write,
	axp803_select,
	axp803_deselect
};

static device_hooks sBatteryHooks = {
	battery_open,
	axp803_close,
	axp803_free,
	battery_control,
	battery_read,
	axp803_write,
	NULL,
	NULL
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

	// battery voltage and current measured
	uint8 adc = 0;
	pmic_read(kRegisterAdcEnable, &adc);
	if ((adc & kAdcEnableBattery) != kAdcEnableBattery)
		pmic_write(kRegisterAdcEnable, adc | kAdcEnableBattery);

	// the battery temperature window for charging, for its NTC
	pmic_write(kRegisterChargeColdLimit, kChargeColdLimit);
	pmic_write(kRegisterChargeHotLimit, kChargeHotLimit);

	// the charger type, for a higher input current limit on a charger
	uint8 bc = 0;
	pmic_read(kRegisterBcGlobal, &bc);
	if ((bc & kBcEnable) == 0)
		pmic_write(kRegisterBcGlobal, bc | kBcEnable);

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
	if (strcmp(name, "power/battery/axp803") == 0)
		return &sBatteryHooks;
	return &sButtonHooks;
}
