#include <ananas/error.h>
#include <ananas/bootinfo.h>
#include "kernel/kdb.h"
#include "kernel/lib.h"
#include "kernel/reboot.h"

KDB_COMMAND(bootinfo, NULL, "Display boot info")
{
	if (bootinfo == NULL) {
		kprintf("no bootinfo supplied by loader\n");
		return;
	}
	kprintf("modules location   : %x\n",
	 bootinfo->bi_modules);
	kprintf("memorymap location: %x - %x\n",
	 bootinfo->bi_memory_map_addr,
	 bootinfo->bi_memory_map_addr + bootinfo->bi_memory_map_size);
}

KDB_COMMAND(reboot, NULL, "Force a reboot")
{
	md_reboot();
}

/* vim:set ts=2 sw=2: */
