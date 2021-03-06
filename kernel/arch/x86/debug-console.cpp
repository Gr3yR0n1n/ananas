#include "kernel/x86/io.h"
#include "kernel/x86/sio.h"
#include "kernel/debug-console.h"

#define DEBUGCON_IO 0x3f8	/* COM1 */

static int debugcon_bochs_console = 0;

void
debugcon_init()
{
	/*
	 * Checks for the bochs 0xe9 'hack', which is actually a very handy feature;
	 * it allows us to dump debugger output to the bochs console, which we will
	 * do in addition to writing the serial port (since we cannot read from this
	 * port)
	 */
	outb(0x80, 0xff);
	if(inb(0xe9) == 0xe9) {
		debugcon_bochs_console++;
	}

	/* Wire up the serial port */
	outb(DEBUGCON_IO + SIO_REG_IER,  0);			/* Disables interrupts */
	outb(DEBUGCON_IO + SIO_REG_LCR,  0x80);		/* Enable DLAB */
	outb(DEBUGCON_IO + SIO_REG_DATA, 1);			/* Divisor low byte (115200 baud) */
	outb(DEBUGCON_IO + SIO_REG_IER,  0);			/* Divisor hi byte */
	outb(DEBUGCON_IO + SIO_REG_LCR,  3);			/* 8N1 */
	outb(DEBUGCON_IO + SIO_REG_FIFO, 0xc7);		/* Enable/clear FIFO (14 bytes) */
}

void
debugcon_putch(int ch)
{
	if (debugcon_bochs_console) {
		outb(0xe9, ch);
	} else {
		while((inb(DEBUGCON_IO + SIO_REG_LSR) & 0x20) == 0)
			/* wait for the transfer buffer to become empty */ ;
		outb(DEBUGCON_IO + SIO_REG_DATA, ch);
	}
}

int
debugcon_getch()
{
	if ((inb(DEBUGCON_IO + SIO_REG_LSR) & 1) == 0)
		return 0;
	return inb(DEBUGCON_IO + SIO_REG_DATA);
}

/* vim:set ts=2 sw=2: */
