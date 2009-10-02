#include "syscall.h"

register_t
syscall(struct SYSCALL_ARGS* a)
{
	switch(a->number) {
		case 0xfffe0001: /* XXX temp print */
			kprintf("%c", a->arg1);
			break;
		default:
			kprintf("syscall, no=%x,a1=%u,a2=%u,a3=%u,a4=%u,a5=%u\n",
			 a->number, a->arg1, a->arg2, a->arg3, a->arg4, a->arg5);
			break;
	}

	return 0xf00db00b;
}

/* vim:set ts=2 sw=2: */