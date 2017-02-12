#ifndef __ANANAS_RESOURCE_H__
#define __ANANAS_RESOURCE_H__

#include <ananas/types.h>

namespace Ananas {

struct Resource {
	enum Type {
		RT_Unused,
		/* Base resource types */
		RT_Memory,
		RT_IO,
		RT_IRQ,
		/* Generic child devices */
		RT_ChildNum,
		/* Pci-specific resource types */
		RT_PCI_VendorID,
		RT_PCI_DeviceID,
		RT_PCI_Bus,
		RT_PCI_Device,
		RT_PCI_Function,
		RT_PCI_ClassRev,
		/* Pnp id - used by acpi */
		RT_PNP_ID,
		/* USB-specific */
		RT_USB_Device,
	};
	typedef uintptr_t Base;
	typedef size_t Length;

	Resource() = default;
	Resource(const Type& type, Base base, Length length) : r_Type(type), r_Base(base), r_Length(length) { }

	Type r_Type = RT_Unused;
	Base r_Base = 0;
	Length r_Length = 0;
};

class ResourceSet {
public:

	/* Maximum number of resources a given resource set can have */
	static const size_t rs_MaxEntries = 16;

	void* AllocateResource(const Resource::Type& type, Resource::Length length);
	bool AddResource(const Resource& resource);
	Resource* GetResource(const Resource::Type& type, size_t index);
	void Print() const;

private:
	Resource rs_Resource[rs_MaxEntries];
};


} // namespace Ananas


#endif /* __ANANAS_RESOURCE_H__ */
