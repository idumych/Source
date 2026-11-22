/*
 * Copyright 2026, Ian Dumych. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*	Keys on the low resolution ADC ("LRADC", "KEYADC") of Allwinner SoCs, as
	the PinePhone's volume keys on the A64. The keys pull channel 0 down to a
	voltage of their own through a resistor ladder; the controller signals
	when a key goes down and comes back up. As in Linux' sun4i-lradc-keys.c,
	the key is the button node (of the controller's device tree node) whose
	voltage is closest to what was measured when it went down. A sample taken
	while the voltage is still falling from its idle level can read well above
	every button, so a key is only decided once two consecutive samples fall
	within the tolerance of the same button; until then every sample
	interrupts.

	The keys are published as a keyboard (input/keyboard/lradc/0) that
	reports HID consumer page key codes, which the Shortcuts preferences bind
	to setvolume by default. */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <bus/FDT.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <keyboard_mouse_driver.h>


#define INFO(x...)	dprintf("sunxi_lradc_keys: " x)
#define ERROR(x...)	dprintf("sunxi_lradc_keys: " x)


#define DRIVER_MODULE_NAME	"drivers/input/sunxi_lradc_keys/driver_v1"
#define DEVICE_MODULE_NAME	"drivers/input/sunxi_lradc_keys/device_v1"
#define DEVICE_PATH			"input/keyboard/lradc/0"


// registers
#define LRADC_CTRL			0x00
#define LRADC_INTC			0x04
#define LRADC_INTS			0x08
#define LRADC_DATA0			0x0c

#define CTRL_FIRST_CONVERT_DELAY(x)	((uint32)(x) << 24)
#define CTRL_LEVEL_A_B_COUNT(x)		((uint32)(x) << 8)
#define CTRL_HOLD_ENABLE			(1u << 6)
#define CTRL_SAMPLE_RATE_250HZ		(0u << 2)
#define CTRL_ENABLE					(1u << 0)

#define INT_CHANNEL0_DATA			(1u << 0)
#define INT_CHANNEL0_KEY_UP			(1u << 4)
#define INT_CHANNEL0_KEY_DOWN		(1u << 1)

#define DATA_MASK					0x3f

// Linux input key codes of the button nodes, and what they are reported as
#define LINUX_KEY_MUTE				113
#define LINUX_KEY_VOLUMEDOWN		114
#define LINUX_KEY_VOLUMEUP			115
#define HID_CONSUMER(usage)			((0x0cu << 16) | (usage))

static const size_t kMaxButtons = 8;
static const size_t kQueueSize = 16;
static const uint32 kNoButton = UINT32_MAX;
static const uint32 kMaxTolerance = 100000;	// µV


static device_manager_info* sDeviceManager;


struct lradc_button {
	uint32				voltage;	// µV
	uint32				keycode;
};

struct lradc_device {
	device_node*		node;
	area_id				area;
	volatile uint8*		registers;
	uint32				interrupt;
	bool				interruptInstalled;
	uint32				reference;	// µV at a reading of 63

	lradc_button		buttons[kMaxButtons];
	uint32				buttonCount;
	uint32				tolerance;	// µV
	uint32				pressedKey;
	bool				sampling;	// went down, the key is not decided yet
	uint32				candidate;	// button of the last sample
	uint32				candidateVoltage;

	spinlock			queueLock;
	raw_key_info		queue[kQueueSize];
	uint32				queueStart;
	uint32				queueCount;
	ConditionVariable	condition;
	int32				openCount;
};


static inline uint32
read32(lradc_device* device, uint32 reg)
{
	return *(volatile uint32*)(device->registers + reg);
}


static inline void
write32(lradc_device* device, uint32 reg, uint32 value)
{
	*(volatile uint32*)(device->registers + reg) = value;
}


static uint32
keycode_for(uint32 linuxCode)
{
	switch (linuxCode) {
		case LINUX_KEY_MUTE:
			return HID_CONSUMER(0xe2);
		case LINUX_KEY_VOLUMEDOWN:
			return HID_CONSUMER(0xea);
		case LINUX_KEY_VOLUMEUP:
			return HID_CONSUMER(0xe9);
		default:
			return 0;
	}
}


/*!	Called with interrupts off. */
static void
queue_key(lradc_device* device, uint32 keycode, bool down)
{
	SpinLocker locker(device->queueLock);
	if (device->queueCount == kQueueSize) {
		device->queueStart = (device->queueStart + 1) % kQueueSize;
		device->queueCount--;
	}
	raw_key_info& info = device->queue[(device->queueStart
		+ device->queueCount) % kQueueSize];
	info.timestamp = system_time();
	info.keycode = keycode;
	info.is_keydown = down;
	device->queueCount++;
	locker.Unlock();

	device->condition.NotifyAll();
}


static inline uint32
read_voltage(lradc_device* device)
{
	return (read32(device, LRADC_DATA0) & DATA_MASK)
		* (uint64)device->reference / DATA_MASK;
}


/*!	The button whose voltage the reading is within the tolerance of, or
	kNoButton. */
static uint32
button_for(lradc_device* device, uint32 voltage)
{
	uint32 closest = UINT32_MAX;
	uint32 button = kNoButton;
	for (uint32 i = 0; i < device->buttonCount; i++) {
		uint32 difference = abs((int32)(device->buttons[i].voltage
			- voltage));
		if (difference < closest) {
			closest = difference;
			button = i;
		}
	}
	return closest <= device->tolerance ? button : kNoButton;
}


/*!	Called with interrupts off. */
static void
press(lradc_device* device, uint32 button, uint32 voltage)
{
	device->pressedKey = device->buttons[button].keycode;
	queue_key(device, device->pressedKey, true);
	INFO("key %#" B_PRIx32 " at %" B_PRIu32 " mV\n", device->pressedKey,
		voltage / 1000);
}


static void
set_sampling(lradc_device* device, bool sampling)
{
	device->sampling = sampling;
	device->candidate = kNoButton;
	write32(device, LRADC_INTC, INT_CHANNEL0_KEY_UP | INT_CHANNEL0_KEY_DOWN
		| (sampling ? INT_CHANNEL0_DATA : 0));
}


static int32
lradc_interrupt(void* data)
{
	lradc_device* device = (lradc_device*)data;
	uint32 status = read32(device, LRADC_INTS);
	if ((status & (INT_CHANNEL0_KEY_DOWN | INT_CHANNEL0_KEY_UP
			| INT_CHANNEL0_DATA)) == 0) {
		return B_UNHANDLED_INTERRUPT;
	}
	write32(device, LRADC_INTS, status);

	// A release first: it can share an interrupt with the next press
	if ((status & INT_CHANNEL0_KEY_UP) != 0) {
		if (device->sampling) {
			// released before two samples agreed
			if (device->candidate != kNoButton)
				press(device, device->candidate, device->candidateVoltage);
			else
				INFO("short press not matched to a key\n");
			set_sampling(device, false);
		}
		if (device->pressedKey != 0) {
			queue_key(device, device->pressedKey, false);
			device->pressedKey = 0;
		}
	}

	if ((status & INT_CHANNEL0_KEY_DOWN) != 0 && device->pressedKey == 0
		&& !device->sampling) {
		set_sampling(device, true);
		device->candidateVoltage = read_voltage(device);
		device->candidate = button_for(device, device->candidateVoltage);
	} else if ((status & INT_CHANNEL0_DATA) != 0 && device->sampling) {
		uint32 voltage = read_voltage(device);
		uint32 button = button_for(device, voltage);
		if (button != kNoButton && button == device->candidate) {
			press(device, button, voltage);
			set_sampling(device, false);
		} else {
			device->candidate = button;
			device->candidateVoltage = voltage;
		}
	}

	return B_INVOKE_SCHEDULER;
}


// #pragma mark - device


static status_t
lradc_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static status_t
lradc_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	lradc_device* device = (lradc_device*)deviceCookie;
	atomic_add(&device->openCount, 1);
	*_cookie = device;
	return B_OK;
}


static status_t
lradc_close(void* cookie)
{
	lradc_device* device = (lradc_device*)cookie;
	atomic_add(&device->openCount, -1);
	device->condition.NotifyAll();
	return B_OK;
}


static status_t
lradc_free(void* cookie)
{
	return B_OK;
}


static status_t
lradc_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
lradc_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
lradc_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	lradc_device* device = (lradc_device*)cookie;

	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			static const char kName[] = "Allwinner LRADC keys";
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_strlcpy((char*)buffer, kName, length) >= 0
				? B_OK : B_BAD_ADDRESS;
		}

		case KB_READ:
		{
			if (length < sizeof(raw_key_info))
				return B_BUFFER_OVERFLOW;

			raw_key_info info;
			while (true) {
				ConditionVariableEntry entry;
				device->condition.Add(&entry);

				cpu_status state = disable_interrupts();
				acquire_spinlock(&device->queueLock);
				bool available = device->queueCount > 0;
				if (available) {
					info = device->queue[device->queueStart];
					device->queueStart = (device->queueStart + 1) % kQueueSize;
					device->queueCount--;
				}
				release_spinlock(&device->queueLock);
				restore_interrupts(state);

				if (available)
					break;
				status_t status = entry.Wait(B_CAN_INTERRUPT);
				if (status == B_INTERRUPTED)
					return status;
			}

			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(buffer, &info, sizeof(info)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case KB_GET_KEYBOARD_ID:
		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_GET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
		case KB_GET_KEY_REPEAT_DELAY:
			// no LEDs, no repeat: the volume keys step once per press
			return B_NOT_SUPPORTED;
	}

	return B_DEV_INVALID_IOCTL;
}


// #pragma mark - driver


static const char* kCompatible[] = {
	"allwinner,sun50i-a64-lradc",
	"allwinner,sun8i-a83t-r-lradc",
};


static float
lradc_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	device_attr* attr = NULL;
	bool supported = false;
	while (!supported
		&& sDeviceManager->get_next_attr(parent, &attr) == B_OK) {
		if (strcmp(attr->name, "fdt/compatible") != 0
			|| attr->type != B_STRING_TYPE) {
			continue;
		}
		for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
			if (strcmp(attr->value.string, kCompatible[i]) == 0)
				supported = true;
		}
	}
	if (!supported)
		return 0.0f;

	fdt_device_module_info* fdt;
	fdt_device* device;
	if (sDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK) {
		return 0.0f;
	}
	const char* status = (const char*)fdt->get_prop(device, "status", NULL);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}
	return 0.8f;
}


static status_t
lradc_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Allwinner LRADC keys" } },
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED } },
		{}
	};
	return sDeviceManager->register_node(parent, DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static uint32
get_cell(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint32 defaultValue)
{
	int length;
	const uint32* property = (const uint32*)fdt->get_prop(device, name,
		&length);
	if (property == NULL || length != 4)
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(*property);
}


/*!	The button nodes are children of the controller's node; each is a device
	node of the FDT bus of its own.
*/
static void
read_buttons(lradc_device* device, device_node* controller)
{
	device_node* child = NULL;
	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "fdt" } },
		{}
	};
	while (device->buttonCount < kMaxButtons
		&& sDeviceManager->get_next_child_node(controller, attrs, &child)
			== B_OK) {
		fdt_device_module_info* fdt;
		fdt_device* fdtDevice;
		if (sDeviceManager->get_driver(child, (driver_module_info**)&fdt,
				(void**)&fdtDevice) != B_OK) {
			continue;
		}
		uint32 voltage = get_cell(fdt, fdtDevice, "voltage", UINT32_MAX);
		uint32 linuxCode = get_cell(fdt, fdtDevice, "linux,code", 0);
		uint32 channel = get_cell(fdt, fdtDevice, "channel", 0);
		uint32 keycode = keycode_for(linuxCode);
		const char* label = (const char*)fdt->get_prop(fdtDevice, "label",
			NULL);
		if (voltage == UINT32_MAX || channel != 0 || keycode == 0) {
			INFO("button \"%s\" (code %" B_PRIu32 ") ignored\n",
				label != NULL ? label : "?", linuxCode);
			continue;
		}
		device->buttons[device->buttonCount].voltage = voltage;
		device->buttons[device->buttonCount].keycode = keycode;
		device->buttonCount++;
		INFO("\"%s\" at %" B_PRIu32 " mV\n", label != NULL ? label : "?",
			voltage / 1000);
	}
	if (child != NULL)
		sDeviceManager->put_node(child);

	// Half the smallest gap between two buttons, so that a reading belongs to
	// one button at most
	for (uint32 i = 0; i < device->buttonCount; i++) {
		for (uint32 j = i + 1; j < device->buttonCount; j++) {
			uint32 gap = abs((int32)(device->buttons[i].voltage
				- device->buttons[j].voltage));
			if (gap / 2 < device->tolerance)
				device->tolerance = gap / 2;
		}
	}
}


static status_t
lradc_init_driver(device_node* node, void** _cookie)
{
	lradc_device* device = new(std::nothrow) lradc_device;
	if (device == NULL)
		return B_NO_MEMORY;
	device->node = node;
	device->area = -1;
	device->registers = NULL;
	device->interruptInstalled = false;
	device->buttonCount = 0;
	device->tolerance = kMaxTolerance;
	device->pressedKey = 0;
	device->sampling = false;
	device->candidate = kNoButton;
	device->candidateVoltage = 0;
	B_INITIALIZE_SPINLOCK(&device->queueLock);
	device->queueStart = device->queueCount = 0;
	device->condition.Init(device, "lradc keys");
	device->openCount = 0;
	// ALDO3 (3.0 V on the PinePhone) through the A83T/A64 divider of 3/4
	device->reference = 3000000 * 3 / 4;

	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* fdtDevice;
	uint64 base = 0, size = 0, interrupt = 0;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&fdtDevice);
	if (status == B_OK && (!fdt->get_reg(fdtDevice, 0, &base, &size)
			|| !fdt->get_interrupt(fdtDevice, 0, NULL, &interrupt))) {
		status = B_BAD_DATA;
	}
	if (status == B_OK)
		read_buttons(device, parent);
	sDeviceManager->put_node(parent);

	if (status != B_OK || device->buttonCount == 0) {
		ERROR("no registers, interrupt or buttons in the device tree\n");
		delete device;
		return status != B_OK ? status : B_DEVICE_NOT_FOUND;
	}
	device->interrupt = interrupt;

	device->area = map_physical_memory("lradc registers", base,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&device->registers);
	if (device->area < 0) {
		status = device->area;
		delete device;
		return status;
	}

	write32(device, LRADC_INTC, 0);
	write32(device, LRADC_INTS, 0xffffffff);
	status = install_io_interrupt_handler(device->interrupt, lradc_interrupt,
		device, 0);
	if (status != B_OK) {
		ERROR("cannot install the handler for interrupt %" B_PRIu32 "\n",
			device->interrupt);
		delete_area(device->area);
		delete device;
		return status;
	}
	device->interruptInstalled = true;

	// Linux' settings: continuous sampling at 250 Hz, a key counts once two
	// samples agree
	write32(device, LRADC_CTRL, CTRL_FIRST_CONVERT_DELAY(2)
		| CTRL_LEVEL_A_B_COUNT(1) | CTRL_HOLD_ENABLE
		| CTRL_SAMPLE_RATE_250HZ | CTRL_ENABLE);
	write32(device, LRADC_INTC, INT_CHANNEL0_KEY_UP | INT_CHANNEL0_KEY_DOWN);

	INFO("%" B_PRIu32 " keys at %#" B_PRIx64 ", interrupt %" B_PRIu32
		", tolerance %" B_PRIu32 " mV\n", device->buttonCount, base,
		device->interrupt, device->tolerance / 1000);
	*_cookie = device;
	return B_OK;
}


static void
lradc_uninit_driver(void* cookie)
{
	lradc_device* device = (lradc_device*)cookie;
	write32(device, LRADC_INTC, 0);
	write32(device, LRADC_CTRL, 0);
	if (device->interruptInstalled) {
		remove_io_interrupt_handler(device->interrupt, lradc_interrupt,
			device);
	}
	delete_area(device->area);
	delete device;
}


static status_t
lradc_register_child_devices(void* cookie)
{
	lradc_device* device = (lradc_device*)cookie;
	return sDeviceManager->publish_device(device->node, DEVICE_PATH,
		DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static device_module_info sLradcDevice = {
	{
		DEVICE_MODULE_NAME,
		0,
		NULL
	},
	lradc_init_device,
	NULL,	// uninit
	NULL,	// removed
	lradc_open,
	lradc_close,
	lradc_free,
	lradc_read,
	lradc_write,
	NULL,	// io
	lradc_control,
	NULL,	// select
	NULL	// deselect
};


static driver_module_info sLradcDriver = {
	{
		DRIVER_MODULE_NAME,
		0,
		NULL
	},
	lradc_supports_device,
	lradc_register_device,
	lradc_init_driver,
	lradc_uninit_driver,
	lradc_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sLradcDriver,
	(module_info*)&sLradcDevice,
	NULL
};
