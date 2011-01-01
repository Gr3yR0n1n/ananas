#include <ananas/types.h>
#include <machine/frame.h>
#include <machine/param.h>
#include <machine/thread.h>
#include <machine/vm.h>
#include <machine/macro.h>
#include <machine/smp.h>
#include <ananas/error.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include <ananas/pcpu.h>
#include <ananas/thread.h>
#include <ananas/vm.h>
#include "options.h"

extern struct TSS kernel_tss;
void clone_return();

static errorcode_t
md_thread_setup(thread_t t)
{
	memset(t->md_pagedir, 0, PAGE_SIZE);
	vm_map_kernel_addr(t->md_pagedir);

	/* Perform adequate mapping for the stack / code */
	vm_mapto_pagedir(t->md_pagedir, USERLAND_STACK_ADDR, (addr_t)t->md_stack,  THREAD_STACK_SIZE / PAGE_SIZE, 1);
	vm_map_pagedir(t->md_pagedir, (addr_t)t->md_kstack, KERNEL_STACK_SIZE / PAGE_SIZE, 0);

#ifdef SMP	
	/*
	 * Grr - for some odd reason, the GDT had to be subject to paging. This means
	 * we have to insert a suitable mapping for every CPU (this does not apply
 	 * to the BSP as it is not allocated)
	 */
	uint32_t i;
	for (i = 1; i < get_num_cpus(); i++) {
		struct IA32_CPU* cpu = get_cpu_struct(i);
		vm_map_pagedir(t->md_pagedir, (addr_t)cpu->gdt, 1 /* XXX */, 0);
	}
#endif

	/* Fill out the thread's registers - anything not here will be zero */ 
	t->md_ctx.esp  = (addr_t)USERLAND_STACK_ADDR + THREAD_STACK_SIZE;
	t->md_ctx.esp0 = (addr_t)t->md_kstack + KERNEL_STACK_SIZE;
	t->md_ctx.cs = GDT_SEL_USER_CODE + SEG_DPL_USER;
	t->md_ctx.ds = GDT_SEL_USER_DATA;
	t->md_ctx.es = GDT_SEL_USER_DATA;
	t->md_ctx.ss = GDT_SEL_USER_DATA + SEG_DPL_USER;
	t->md_ctx.cr3 = (addr_t)t->md_pagedir;
	t->md_ctx.eflags = EFLAGS_IF;

	/* initialize FPU state similar to what finit would do */
	t->md_fpu_ctx.cw = 0x37f;
	t->md_fpu_ctx.tw = 0xffff;

	t->next_mapping = 1048576;
	return ANANAS_ERROR_OK;
}

errorcode_t
md_thread_init(thread_t t)
{
	/* Create a pagedirectory and map the kernel pages in there */
	t->md_pagedir = kmalloc(PAGE_SIZE);

	/* Allocate stacks: one for the thread and one for the kernel */
	t->md_stack  = kmalloc(THREAD_STACK_SIZE);
	t->md_kstack = kmalloc(KERNEL_STACK_SIZE);

	return md_thread_setup(t);
}

void
md_thread_free(thread_t t)
{
	vm_free_pagedir(t->md_pagedir);
	kfree(t->md_pagedir);
	kfree(t->md_stack);
	kfree(t->md_kstack);
}

void
md_thread_switch(thread_t new, thread_t old)
{
	struct CONTEXT* ctx_new = (struct CONTEXT*)&new->md_ctx;

	/*
	 * Activate this context as the current CPU context. XXX lock
	 */
	__asm(
		"movw %%bx, %%fs\n"
		"movl	%%eax, %%fs:0\n"
	: : "a" (ctx_new), "b" (GDT_SEL_KERNEL_PCPU));

	/* Fetch kernel TSS */
	struct TSS* tss;
	__asm(
		"movl	%%fs:8, %0\n"
	: "=r" (tss));

	/* Activate the corresponding kernel stack in the TSS */
	tss->esp0 = ctx_new->esp0;

	/* Go! */
	md_restore_ctx(ctx_new);
}

void*
md_map_thread_memory(thread_t thread, void* ptr, size_t length, int write)
{
	KASSERT(length <= PAGE_SIZE, "no support for >PAGE_SIZE mappings yet!");

	addr_t addr = (addr_t)ptr & ~(PAGE_SIZE - 1);
	addr_t phys = vm_get_phys(thread->md_pagedir, addr, write);
	if (phys == 0)
		return NULL;

	addr_t virt = TEMP_USERLAND_ADDR + PCPU_GET(cpuid) * TEMP_USERLAND_SIZE;
	vm_mapto(virt, phys, 2 /* XXX */);
	return (void*)virt + ((addr_t)ptr % PAGE_SIZE);
}

void*
md_thread_map(thread_t thread, void* to, void* from, size_t length, int flags)
{
	int num_pages = length / PAGE_SIZE;
	if (length % PAGE_SIZE > 0)
		num_pages++;
	/* XXX cannot specify flags yet */
	vm_mapto_pagedir(thread->md_pagedir, (addr_t)to, (addr_t)from, num_pages, 1);
	return to;
}

errorcode_t
md_thread_unmap(thread_t thread, void* addr, size_t length)
{
	int num_pages = length / PAGE_SIZE;
	if (length % PAGE_SIZE > 0)
		num_pages++;
	vm_unmap_pagedir(thread->md_pagedir, (addr_t)addr, num_pages);
	return ANANAS_ERROR_OK;
}

void
md_thread_set_entrypoint(thread_t thread, addr_t entry)
{
	thread->md_ctx.eip = entry;
}

void
md_thread_set_argument(thread_t thread, addr_t arg)
{
	thread->md_ctx.esi = arg;
}

void
md_thread_setkthread(thread_t thread, kthread_func_t kfunc, void* arg)
{
	/*
	 * Kernel threads must share the environment with the kernel; so they have to
	 * run with supervisor privileges and use the kernel page directory.
	 */
	thread->md_ctx.cs = GDT_SEL_KERNEL_CODE;
	thread->md_ctx.ds = GDT_SEL_KERNEL_DATA;
	thread->md_ctx.es = GDT_SEL_KERNEL_DATA;
	thread->md_ctx.fs = GDT_SEL_KERNEL_PCPU;
	thread->md_ctx.eip = (addr_t)kfunc;
	thread->md_ctx.cr3 = (addr_t)pagedir - KERNBASE;

	/*
	 * Now, push 'arg' on the stack, as i386 passes arguments by the stack. Note that
	 * we must first place the value and then update esp0 because the interrupt code
	 * heavily utilized the stack, and the -= 4 protects our value from being
 	 * destroyed.
	 */
	*(uint32_t*)thread->md_ctx.esp0 = (uint32_t)arg;
	thread->md_ctx.esp0 -= 4;

	/*
	 * We do not differentiate between user/kernelstack because we cannot switch
	 * adequately between them (and they cannot do syscalls anyway)
	 */
	thread->md_ctx.esp = thread->md_ctx.esp0;
}
	
void
md_thread_clone(struct THREAD* t, struct THREAD* parent, register_t retval)
{
	/* Copy the entire context over */
	memcpy(&t->md_ctx, &parent->md_ctx, sizeof(t->md_ctx));

	/*
	 * A kernel stack is not subject to paging; this is unavoidable as we need it
	 * during a context switch, so it must reside in the kernel address space.
	 *
	 * This means we'll have to reinitialize a kernel stack - the clone_return
	 * code will perform the adequate magic.
	 */
	t->md_ctx.esp0 = (addr_t)t->md_kstack + KERNEL_STACK_SIZE;
	t->md_ctx.cr3  = (addr_t)t->md_pagedir;

	/*
	 * Copy stack content; we copy the kernel stack over
	 * because we can obtain the stackframe from it, which
	 * allows us to return to the intended caller.
	 */
	memcpy(t->md_stack,  parent->md_stack, THREAD_STACK_SIZE);
	memcpy(t->md_kstack, parent->md_kstack, KERNEL_STACK_SIZE);

	/* Handle return value */
	t->md_ctx.cs = GDT_SEL_KERNEL_CODE;
	t->md_ctx.eip = (addr_t)&clone_return;
	t->md_ctx.eax = retval;
}

/* vim:set ts=2 sw=2: */
