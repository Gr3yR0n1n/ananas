#include <ananas/device.h>
#include <ananas/console.h>
#include <ananas/error.h>
#include <ananas/mm.h>
#include <ananas/lib.h>
#include <ananas/init.h>
#include <ananas/thread.h>
#include <ananas/pcpu.h>
#include <ananas/tty.h>
#include <ananas/vfs.h>
#include <machine/vm.h>
#include <machine/param.h> /* for PAGE_SIZE */

#include "options.h"

#define SHELL_BIN "/bin/sh"
#define ROOT_DEVICE "slice0"
#define ROOT_FS_TYPE "fatfs"
#define DEVFS_MOUNTPOINT "/dev"
#undef RAMDISK

/* If set, display the entire init tree before launching it */
#undef VERBOSE_INIT

extern void* __initfuncs_begin;
extern void* __initfuncs_end;

static struct INIT_FUNC_DYNAMICS initfunc_dynamics;
static int initfunc_dynamics_amount = 0;

void
init_register_func(struct KERNEL_MODULE* kmod, struct INIT_FUNC* ifunc)
{
	struct INIT_DYNAMIC_FUNC* idfunc = kmalloc(sizeof(struct INIT_DYNAMIC_FUNC));
	idfunc->idf_kmod = kmod;
	idfunc->idf_ifunc = ifunc;
	/* No lock necessary as we're behind the module lock either way */
	DQUEUE_ADD_TAIL(&initfunc_dynamics, idfunc);
	initfunc_dynamics_amount++;
#ifdef VERBOSE_INIT	
	kprintf("init_register_func(): kmod=%p ifunc=%p\n", kmod, ifunc->if_func);
#endif
}

void
init_unregister_module(struct KERNEL_MODULE* kmod)
{
	/* No lock necessary as we're behind the module lock either way */
	if (!DQUEUE_EMPTY(&initfunc_dynamics))
		DQUEUE_FOREACH_SAFE(&initfunc_dynamics, idf, struct INIT_DYNAMIC_FUNC) {
			if (idf->idf_kmod != kmod)
				continue;
			DQUEUE_REMOVE(&initfunc_dynamics, idf);
			kfree(idf);
		}
}

static void
run_init(enum INIT_SUBSYSTEM first_subsystem, enum INIT_SUBSYSTEM last_subsystem)
{
	/*
	 * Create a shadow copy of the init function chain; this is done so that we
	 * can sort it.
	 */
	size_t init_static_func_len  = (addr_t)&__initfuncs_end - (addr_t)&__initfuncs_begin;
	struct INIT_FUNC** ifn_chain = kmalloc(init_static_func_len + initfunc_dynamics_amount * sizeof(struct INIT_FUNC*));
	memcpy(ifn_chain, (void*)&__initfuncs_begin, init_static_func_len);
	int n = init_static_func_len / sizeof(struct INIT_FUNC*);
	if (n > 0)
		DQUEUE_FOREACH(&initfunc_dynamics, idf, struct INIT_DYNAMIC_FUNC) {
			ifn_chain[n++] = idf->idf_ifunc;
		}
	
	/* Sort the init functions chain; we use a simple bubble sort to do so */
	int num_init_funcs = init_static_func_len / sizeof(struct INIT_FUNC*) + initfunc_dynamics_amount;
	for (int i = 0; i < num_init_funcs; i++)
		for (int j = num_init_funcs - 1; j > i; j--) {
			struct INIT_FUNC* a = ifn_chain[j];
			struct INIT_FUNC* b = ifn_chain[j - 1];
			if ((a->if_subsystem > b->if_subsystem) ||
			    (a->if_subsystem >= b->if_subsystem &&
			     a->if_order >= b->if_order))
				continue;
			struct INIT_FUNC swap;
			swap = *a;
			*a = *b;
			*b = swap;
		}

#ifdef VERBOSE_INIT
	kprintf("Init tree\n");
	struct INIT_FUNC** c = (struct INIT_FUNC**)ifn_chain;
	for (int n = 0; n < num_init_funcs; n++, c++) {
		kprintf("initfunc %u -> %p (subsys %x, order %x)", n,
		 (*c)->if_func, (*c)->if_subsystem, (*c)->if_order);
		if ((*c)->if_subsystem < first_subsystem)
			kprintf(" [skip]");
		else if ((*c)->if_subsystem > last_subsystem)
			kprintf(" [ignore]");
		kprintf("\n");
	}
#endif
	
	/* Execute all init functions in order except the final one */
	struct INIT_FUNC** ifn = (struct INIT_FUNC**)ifn_chain;
	for (int i = 0; i < num_init_funcs - 1; i++, ifn++) {
		if ((*ifn)->if_subsystem < first_subsystem)
			continue;
		if ((*ifn)->if_subsystem > last_subsystem)
			break;
		(*ifn)->if_func();
	}

	/* Throw away the init function chain; it served its purpose */
	struct INIT_FUNC* ifunc = *ifn;
	kfree(ifn_chain);

	/* Only throw away the dynamic chain if this is our final init run */
	if (last_subsystem == SUBSYSTEM_LAST && !DQUEUE_EMPTY(&initfunc_dynamics))
		DQUEUE_FOREACH_SAFE(&initfunc_dynamics, idf, struct INIT_DYNAMIC_FUNC) {
			kfree(idf);
		}

	/* Call the final init function; it shouldn't return */
	ifunc->if_func();
}

static errorcode_t
hello_world()
{
	/* Show a startup banner */
	kprintf("Ananas/%s - %s %s\n", ARCHITECTURE, __DATE__, __TIME__);
	unsigned int total_pages, avail_pages;
	page_get_stats(&total_pages, &avail_pages);
	kprintf("Memory: %uKB available / %uKB total\n", avail_pages * (PAGE_SIZE / 1024), total_pages * (PAGE_SIZE / 1024));
#if defined(__i386__) || defined(__amd64__)
	extern int md_cpu_clock_mhz;
	kprintf("CPU: %u MHz\n", md_cpu_clock_mhz);
#endif

	return ANANAS_ERROR_OK;
}

INIT_FUNCTION(hello_world, SUBSYSTEM_CONSOLE, ORDER_LAST);

#if 0
static errorcode_t
mount_filesystems()
{
	errorcode_t err;

	kprintf("- Mounting / from %s...", ROOT_DEVICE);
	err = vfs_mount(ROOT_DEVICE, "/", ROOT_FS_TYPE, NULL);
	if (err == ANANAS_ERROR_NONE) {
		kprintf(" ok\n");
	} else {
		kprintf(" failed, error %i\n", err);
	}

#ifdef DEVFS
	kprintf("- Mounting devfs on %s...", DEVFS_MOUNTPOINT);
	err = vfs_mount(NULL, DEVFS_MOUNTPOINT, "devfs", NULL);
	if (err == ANANAS_ERROR_NONE) {
		kprintf(" ok\n");
	} else {
		kprintf(" failed, error %i\n", err);
	}
#endif

#ifdef RAMDISK
	kprintf("- Mounting romdisk on /rom...");
	err = vfs_mount(RAMDISK, "/rom", "cramfs", NULL);
	if (err == ANANAS_ERROR_NONE) {
		kprintf(" ok\n");
	} else {
		kprintf(" failed, error %i\n", err);
	}
#endif
	return ANANAS_ERROR_OK;
}

INIT_FUNCTION(mount_filesystems, SUBSYSTEM_VFS, ORDER_LAST);

#ifdef SHELL_BIN
static errorcode_t
launch_shell()
{
	thread_t* t;
	errorcode_t err = thread_alloc(NULL, &t, THREAD_ALLOC_DEFAULT);
	if (err != ANANAS_ERROR_NONE) {
		kprintf(" couldn't create process, %i\n", err);
		return err;
	}

	struct VFS_FILE f;
	kprintf("- Lauching %s...", SHELL_BIN);
	err = vfs_open(SHELL_BIN, NULL, &f);
	if (err == ANANAS_ERROR_NONE) {
		err = vfs_summon(&f, t);
		if (err == ANANAS_ERROR_NONE) {
			kprintf(" ok\n");
			thread_set_args(t, "sh\0-l\0", PAGE_SIZE);
			thread_set_environment(t, "OS=Ananas\0USER=root\0\0", PAGE_SIZE);
			thread_resume(t);
		} else {
			kprintf(" fail - error %i\n", err);
		}
		vfs_close(&f);
	} else {
		kprintf(" fail - error %i\n", err);
	}
	return err;
}

INIT_FUNCTION(launch_shell, SUBSYSTEM_SCHEDULER, ORDER_MIDDLE);
#endif /* SHELL_BIN  */
#endif

static void
init_thread_func(void* done)
{
	/*
	 * We need to run the init tree in two steps: up to the module part, and everything
	 * after it; this is done so that modules can dynamically register functions as
	 * well.
	 */
	run_init(SUBSYSTEM_MODULE, SUBSYSTEM_MODULE + 1);
	run_init(SUBSYSTEM_MODULE + 1, SUBSYSTEM_LAST);

	/* All done! */
	*(volatile int*)done = 1;

	/* XXX We can't yet exit kernel threads, so just suspend for now */
	thread_suspend(PCPU_GET(curthread));
	schedule();

	/* NOTREACHED */
}

void
mi_startup()
{
	DQUEUE_INIT(&initfunc_dynamics);

	/*
	 * Create a thread to handle the init functions; this will allow us to
	 * sleep whenever we want, as there is always the idle thread to pick up the
	 * cycles for us (note that this code is run as the init thread and such it
	 * must never sleep)
	 */
	volatile int done = 0;
	thread_t init_thread;
	kthread_init(&init_thread, init_thread_func, (void*)&done);
	thread_set_args(&init_thread, "[init]\0\0", 8);
	thread_resume(&init_thread);

	/* Activate the scheduler - it is time */
	scheduler_launch();

	/*
	 *  Okay, for time being this will be the idle thread - we must not sleep, as
	 * we are the idle thread
	 */
	while(!done) {
		md_cpu_relax();
	}

#if 0 // NOTYET, can't destroy kernel threads
	/* Guess we are all done; kill the init thread */
	thread_free(&init_thread);
	thread_destroy(&init_thread);
#endif

	/* And now, we become the idle thread */
	idle_thread();

	/* NOTREACHED */
}

#define TEST_THREADS 0
#include "init-debug.c"

/* vim:set ts=2 sw=2: */
