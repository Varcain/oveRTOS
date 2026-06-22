/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Source for the /bin/sigwait fixture (tests/ontarget/loader_sigwait_image.h),
 * which proves ASYNCHRONOUS signal delivery: it installs a SIGINT handler and
 * blocks in read(0). When the console sees ^C (in canonical/ISIG mode) it
 * generates SIGINT for this foreground reader; the read returns EINTR after the
 * handler runs, and the program exits — i.e. Ctrl-C interrupts a running command.
 *
 * Build + embed (Buildroot uClibc-ng uClinux/bFLT toolchain, from the oveRTOS root):
 *   TC=$BUILDROOT/output/host/bin/arm-buildroot-uclinux-uclibcgnueabi
 *   "$TC-gcc" -Os -static tests/sim/zephyr-linux/sigwait.c -o /tmp/sigwait.bflt
 *   python3 -c "d=bytearray(open('/tmp/sigwait.bflt','rb').read());\
 *               d[0x28:0x2c]=b'\0\0\0\0';open('/tmp/w.bflt','wb').write(d)"  # zero build-date
 *   cmake -DIN=/tmp/w.bflt -DOUT=tests/ontarget/loader_sigwait_image.h \
 *         -DSYM=ove_test_sigwait_bflt -P tests/cmake/embed_bin.cmake
 */
#include <signal.h>
#include <unistd.h>

static volatile int stop;

static void onint(int sig)
{
	(void)sig;
	write(1, "[SIGINT]\n", 9);
	stop = 1;
}

int main(void)
{
	char c;

	signal(SIGINT, onint);
	write(1, "waiting\n", 8);
	while (!stop) {
		long r = read(0, &c, 1);
		if (r < 0)
			continue; /* EINTR: a signal arrived; re-check stop */
		if (r == 0)
			break; /* EOF */
	}
	write(1, "done\n", 5);
	return 0;
}
