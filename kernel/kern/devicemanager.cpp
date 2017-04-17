#include <ananas/console.h>
#include <ananas/device.h>
#include <ananas/driver.h>
#include <ananas/error.h>
#include <ananas/kdb.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include <ananas/kmem.h>
#include <ananas/tty.h>
#include <ananas/thread.h>
#include <ananas/vm.h>
#include "options.h"

static spinlock_t spl_devicequeue = SPINLOCK_DEFAULT_INIT;
static Ananas::DeviceList deviceList;

namespace Ananas {

namespace DriverManager {
namespace internal {
Ananas::DriverList& GetDriverList();
} // namespace internal
} // namespace DriverManager

namespace DeviceManager {

namespace {

void PrintAttachment(Device& device)
{
	KASSERT(device.d_Parent != NULL, "can't print device which doesn't have a parent bus");
	kprintf("%s%u on %s%u ", device.d_Name, device.d_Unit, device.d_Parent->d_Name, device.d_Parent->d_Unit);
	device.d_ResourceSet.Print();
	kprintf("\n");
}

} // unnamed namespace

namespace internal {

void Register(Device& device)
{
	spinlock_lock(&spl_devicequeue);
	LIST_APPEND(&deviceList, &device);
	spinlock_unlock(&spl_devicequeue);
}

void OnDeviceDestruction(Device& device)
{
	// This is just a safety precaution for now
	spinlock_lock(&spl_devicequeue);
	LIST_FOREACH(&deviceList, d, Device) {
		KASSERT(d != &device, "destroying device '%s' still in the device queue", device.d_Name);
	}
	spinlock_unlock(&spl_devicequeue);
}

void Unregister(Device& device)
{
	/* XXX clear waiters; should we signal them? */

	spinlock_lock(&spl_devicequeue);
	LIST_REMOVE(&deviceList, &device);
	spinlock_unlock(&spl_devicequeue);
}

Device* InstantiateDevice(Driver& driver, const CreateDeviceProperties& cdp)
{
	Device* device = driver.CreateDevice(cdp);
	if (device != nullptr) {
		strcpy(device->d_Name, driver.d_Name);
		device->d_Unit = driver.d_CurrentUnit++; // XXX should we lock this?
	}
	return device;
}

void DeinstantiateDevice(Device& device)
{
	delete &device;
}

} // namespace internal

errorcode_t
AttachSingle(Device& device)
{
	/*
	 * XXX This is a kludge: it prevents us from displaying attach information for drivers
	 * that will be initialize outside the probe tree (such as the console which will be
	 * initialized as soon as possible.
	 */
	if (device.d_Parent != nullptr)
		PrintAttachment(device);

	errorcode_t err = device.GetDeviceOperations().Attach();
	ANANAS_ERROR_RETURN(err);

	/* Hook the device up to the tree */
	internal::Register(device);

	/* Attempt to attach child devices, if any */
	AttachBus(device);

	return ananas_success();
}

errorcode_t
Detach(Device& device)
{
	// Ensure the device is out of the queue; this prevents anyone from using the
	// device
	internal::Unregister(device);

	// XXX I wonder how realistic this is - failing to detach (what can we do?)
	errorcode_t err = device.GetDeviceOperations().Detach();
	ANANAS_ERROR_RETURN(err);

	device.Printf("detached");

	// XXX We should destroy the device, but only if we know this is safe (we need refcounting here)
	internal::DeinstantiateDevice(device);
	return ananas_success();
}

/*
 * Attaches a single device, by handing it off to all possible drivers (first
 * one wins). Succeeds if a driver accepted the device, in which case the
 * device name and unit will be updated.
 */
Ananas::Device*
AttachChild(Device& bus, const Ananas::ResourceSet& resourceSet)
{
	auto& driverList = Ananas::DriverManager::internal::GetDriverList();
	if (LIST_EMPTY(&driverList))
		return nullptr;

	CreateDeviceProperties cdp(bus, resourceSet);
	LIST_FOREACH_SAFE(&driverList, d, Driver) {
		if (!d->MustProbeOnBus(bus))
			continue; /* bus cannot contain this device */

		/* Hook the device to this driver and try to attach it */
		Device* device = internal::InstantiateDevice(*d, cdp);
		if (device == nullptr)
			continue;
		if (ananas_is_success(AttachSingle(*device)))
			return device;

		// Attach failed - get rid of the device
		internal::DeinstantiateDevice(*device);
	}

	return nullptr;
}

/*
 * Attaches children on a given bus - assumes the bus is already set up. May
 * attach multiple devices or none at all.
 *
 * XXX contains hacks for the input/output console device stuff (skips
 * attaching them, as they are already at the point this is run)
 */
void
AttachBus(Device& bus)
{
	auto& driverList = Ananas::DriverManager::internal::GetDriverList();
	if (LIST_EMPTY(&driverList))
		return;

	/*
	 * Fetch TTY devices; we need them to report the device that is already
	 * attached at this point.
	 */
	Ananas::Device* input_dev = tty_get_inputdev(console_tty);
	Ananas::Device* output_dev = tty_get_outputdev(console_tty);
	LIST_FOREACH_SAFE(&driverList, d, Driver) {
		if (!d->MustProbeOnBus(bus))
			continue; /* bus cannot contain this device */

		/*
		 * If we found the driver for the in- or output driver, display it (they are
		 * already attached). XXX we will skip any extra units here
		 */
		if (input_dev != NULL && strcmp(input_dev->d_Name, d->d_Name) == 0) {
			input_dev->d_Parent = &bus;
			PrintAttachment(*input_dev);
			internal::Register(*input_dev);
			continue;
		}
		if (output_dev != NULL && strcmp(output_dev->d_Name, d->d_Name) == 0) {
			output_dev->d_Parent = &bus;
			PrintAttachment(*output_dev);
			internal::Register(*output_dev);
			continue;
		}

		Ananas::ResourceSet resourceSet; // TODO

		// See if the driver accepts our resource set
		Device* device = internal::InstantiateDevice(*d, CreateDeviceProperties(bus, resourceSet));
		if (device == nullptr)
			continue;

		if (ananas_is_failure(AttachSingle(*device))) {
			internal::DeinstantiateDevice(*device); // attach failed; discard the device
		}
	}
}

Device*
FindDevice(const char* name)
{
	char* ptr = (char*)name;
	while (*ptr != '\0' && (*ptr < '0' || *ptr > '9'))
		ptr++;
	int unit = (*ptr != '\0') ? strtoul(ptr, NULL, 10) : 0;

	LIST_FOREACH(&deviceList, device, Device) {
		if (!strncmp(device->d_Name, name, ptr - name) && device->d_Unit == unit)
			return device;
	}
	return nullptr;
}

Device*
CreateDevice(const char* driver, const Ananas::CreateDeviceProperties& cdp)
{
	auto& driverList = Ananas::DriverManager::internal::GetDriverList();
	LIST_FOREACH_SAFE(&driverList, d, Driver) {
		if (strcmp(d->d_Name, driver) == 0)
			return internal::InstantiateDevice(*d, cdp);
	}

	return nullptr;
}

} // namespace DeviceManager

} // namespace Ananas

#ifdef OPTION_KDB
static int
print_devices(Ananas::Device* parent, int indent)
{
	int count = 0;
	LIST_FOREACH(&deviceList, dev, Ananas::Device) {
		if (dev->d_Parent != parent)
			continue;
		for (int n = 0; n < indent; n++)
			kprintf(" ");
		kprintf("device %p: '%s' unit %u\n",
	 	 dev, dev->d_Name, dev->d_Unit);
		count += print_devices(dev, indent + 1) + 1;
	}
	return count;
}

KDB_COMMAND(devices, NULL, "Displays a list of all devices")
{
	int count = print_devices(NULL, 0);

	/* See if we have printed everything; if not, our structure is wrong and we should fix this */
	int n = 0;
	LIST_FOREACH(&deviceList, dev, Ananas::Device) {
		n++;
	}
	if (n != count)
		kprintf("Warning: %d extra device(s) unreachable by walking the device chain!\n", n - count);
}

KDB_COMMAND(devdump, "s:devname", "Displays debugging dump of a device")
{
	auto dev = Ananas::DeviceManager::FindDevice(arg[1].a_u.u_string);
	if (dev == nullptr) {
		kprintf("device not found\n");
		return;
	}

	dev->GetDeviceOperations().DebugDump();
}
#endif

/* vim:set ts=2 sw=2: */