/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Source for the /bin/sigtest fixture (tests/ontarget/loader_sigtest_image.h),
 * a tiny program that proves real signal delivery: it installs a SIGINT handler,
 * raise()s SIGINT (-> gettid + tkill), the engine delivers the signal so the
 * handler runs, then rt_sigreturn resumes execution after raise().
 *
 * Build + embed (Buildroot uClibc-ng uClinux/bFLT toolchain, from the oveRTOS root):
 *   TC=$BUILDROOT/output/host/bin/arm-buildroot-uclinux-uclibcgnueabi
 *   "$TC-gcc" -Os -static tests/sim/zephyr-linux/sigtest.c -o /tmp/sigtest.bflt
 *   python3 -c "d=bytearray(open('/tmp/sigtest.bflt','rb').read());\
 *               d[0x28:0x2c]=b'\0\0\0\0';open('/tmp/s.bflt','wb').write(d)"  # zero build-date
 *   cmake -DIN=/tmp/s.bflt -DOUT=tests/ontarget/loader_sigtest_image.h \
 *         -DSYM=ove_test_sigtest_bflt -P tests/cmake/embed_bin.cmake
 */
#include <signal.h>
#include <unistd.h>

static volatile int caught;

static void handler(int sig)
{
	(void)sig;
	write(1, "caught SIGINT\n", 14);
	caught = 1;
}

int main(void)
{
	signal(SIGINT, handler);
	write(1, "before raise\n", 13);
	raise(SIGINT);
	write(1, "after raise\n", 12);
	return caught ? 0 : 3;
}
