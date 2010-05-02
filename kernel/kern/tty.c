/*
 * Implementation of our TTY device; this multiplexes input/output devices to a
 * single device, and handles the TTY magic.
 */
#include <sys/types.h>
#include <sys/device.h>
#include <sys/pcpu.h>
#include <sys/limits.h>
#include <sys/mm.h>
#include <sys/lib.h>
#include <termios.h>

/* Newline char - cannot be modified using c_cc */
#define NL '\n'
#define CR 0xd

struct TTY_PRIVDATA {
	device_t				input_dev;
	device_t				output_dev;

	struct termios	termios;
	char						input_queue[MAX_INPUT];
	unsigned int		in_writepos;
	unsigned int		in_readpos;

	thread_t				in_sleeper;
};

static struct DRIVER drv_tty;

device_t
tty_alloc(device_t input_dev, device_t output_dev)
{
	device_t dev = device_alloc(NULL, &drv_tty);
	if (dev == NULL)
		return NULL;

	struct TTY_PRIVDATA* priv = kmalloc(sizeof(struct TTY_PRIVDATA));
	memset(priv, 0, sizeof(struct TTY_PRIVDATA));
	priv->input_dev = input_dev;
	priv->output_dev = output_dev;
	dev->privdata = priv;

	/* Use sensible defaults for the termios structure */
	for (int i = 0; i < NCCS; i++)
		priv->termios.c_cc[i] = _POSIX_VDISABLE;
#define CTRL(x) ((x) - 'A' + 1)
	priv->termios.c_cc[VEOF] = CTRL('D');
	priv->termios.c_cc[VKILL] = CTRL('U');
	priv->termios.c_cc[VERASE] = CTRL('H');
#undef CTRL
	priv->termios.c_iflag = ICRNL | ICANON;
	priv->termios.c_oflag = OPOST | ONLCR;
	priv->termios.c_lflag = ECHO | ECHOE;
	priv->termios.c_cflag = CREAD;
	return dev;
}

device_t
tty_get_inputdev(device_t dev)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;
	return priv->input_dev;
}

device_t
tty_get_outputdev(device_t dev)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;
	return priv->output_dev;
}

static ssize_t
tty_write(device_t dev, const char* data, size_t len)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;
	return priv->output_dev->driver->drv_write(priv->output_dev, data, len, 0);
}

static ssize_t
tty_read(device_t dev, char* data, size_t len)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;

	/*
	 * We must read from a tty. XXX We assume blocking does not apply.
	 */
	while (1) {
		if ((priv->termios.c_iflag & ICANON) == 0) {
			/* Canonical input is off - handle requests immediately */
			panic("XXX implement me: icanon off!");
		}

		if (priv->in_readpos == priv->in_writepos) {
			/* Buffer is empty - wait a bit for it to be refilled */
			priv->in_sleeper = PCPU_CURTHREAD();
			thread_suspend(priv->in_sleeper);
			reschedule();
			continue;
		}

		/*
		 * A line is delimited by a newline NL, end-of-file char EOF or end-of-line
		 * EOL char. We will have to scan our input buffer for any of these.
		 */
		int in_len;
		if (priv->in_readpos < priv->in_writepos) {
			in_len = priv->in_writepos - priv->in_readpos;
		} else /* if (priv->in_readpos > priv->in_writepos) */ {
			in_len = (MAX_INPUT - priv->in_writepos) + priv->in_readpos;
		}

		/* See if we can find a delimiter here */
#define CHAR_AT(i) (priv->input_queue[(priv->in_readpos + i) % MAX_INPUT])
		unsigned int n = 0;
		while (n < in_len) {
			if (CHAR_AT(n) == NL)
				break;
			if (priv->termios.c_cc[VEOF] != _POSIX_VDISABLE && CHAR_AT(n) == priv->termios.c_cc[VEOF])
				break;
			if (priv->termios.c_cc[VEOL] != _POSIX_VDISABLE && CHAR_AT(n) == priv->termios.c_cc[VEOL])
				break;
			n++;
		}
#undef CHAR_AT
		if (n == in_len) {
			/* Line is not complete - try again later */
			priv->in_sleeper = PCPU_CURTHREAD();
			thread_suspend(priv->in_sleeper);
			reschedule();
			continue;
		}

		/* A full line is available - copy the data over */
		ssize_t num_read = 0;
		if (len > in_len)
			len = in_len;
		while (len > 0) {
			data[num_read] = priv->input_queue[priv->in_readpos];
			priv->in_readpos = (priv->in_readpos + 1) % MAX_INPUT;
			num_read++; len--;
		}
		return num_read;
	}

	/* NOTREACHED */
}

static void
tty_putchar(device_t dev, unsigned char ch)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;
	if (priv->termios.c_oflag & OPOST) {
		if ((priv->termios.c_oflag & ONLCR) && ch == NL)
			device_write(priv->output_dev, "\r", 1, 0);
	}
	device_write(priv->output_dev, &ch, 1, 0);
}

static void
tty_handle_echo(device_t dev, unsigned char byte)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;
	if ((priv->termios.c_iflag & ICANON) && (priv->termios.c_oflag & ECHOE) && byte == priv->termios.c_cc[VERASE]) {
		/* Need to echo erase char */
		char erase_seq[3] = { 8, ' ', 8 };
		device_write(priv->output_dev, erase_seq, sizeof(erase_seq), 0);
		return;
	}
	if ((priv->termios.c_lflag & (ICANON | ECHONL)) && byte == NL) {
		tty_putchar(dev, byte);
		return;
	}
	if (priv->termios.c_lflag & ECHO)
		tty_putchar(dev, byte);
}

void
tty_signal_data(device_t dev)
{
	struct TTY_PRIVDATA* priv = (struct TTY_PRIVDATA*)dev->privdata;
	unsigned char byte;

	/*
	 * This will be called once data is available from our input device; we need
	 * to queue it up.
	 */
	while (device_read(priv->input_dev, &byte, 1, 0)) {
		/* If we are out of buffer space, just eat the charachter XXX possibly unnecessary for VERASE */
		if ((priv->in_writepos + 1) % MAX_INPUT == priv->in_readpos)
			continue;

		/* Handle CR/NL transformations */
		if ((priv->termios.c_iflag & INLCR) && byte == NL)
			byte = CR;
		else if ((priv->termios.c_iflag & IGNCR) && byte == CR)
			continue;
		else if ((priv->termios.c_iflag & ICRNL) && byte == CR)
			byte = NL;

		/* Handle backspace */
		if ((priv->termios.c_iflag & ICANON) && byte == priv->termios.c_cc[VERASE]) {
			if (priv->in_readpos != priv->in_writepos) {
				/* Still a charachter available which wasn't read. Nuke it */
				if (priv->in_writepos > 0)
					priv->in_writepos--;
				else
					priv->in_writepos = MAX_INPUT - 1;
			}
		} else {
			/* Store the charachter! */
			priv->input_queue[priv->in_writepos] = byte;
			priv->in_writepos = (priv->in_writepos + 1) % MAX_INPUT;
		}

		/* Handle writing the charachter, if needed */
		tty_handle_echo(dev, byte);
	}

	/* If we have a sleeper, awaken him XXX dies if thread dies! */
	if (priv->in_sleeper != NULL) {
		thread_resume(priv->in_sleeper);
		priv->in_sleeper = NULL;
	}
}

static struct DRIVER drv_tty = {
	.name					= "tty",
	.drv_probe		= NULL,
	.drv_read			= tty_read,
	.drv_write		= tty_write
};

/* vim:set ts=2 sw=2: */
