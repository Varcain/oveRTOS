/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zephyr display driver for QEMU semihosting framebuffer.
 *
 * Implements the Zephyr display API by writing RGB565 pixels to
 * /dev/shm/ove-fb via ARM semihosting SVC calls. The host-side
 * display viewer (ove-dashboard-bridge.py) reads and renders them.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <string.h>
#include <stdint.h>

#define DT_DRV_COMPAT ove_shmem_display

/* ARM semihosting operations */
#define SH_SYS_OPEN 0x01
#define SH_SYS_WRITE 0x05
#define SH_SYS_SEEK 0x0A

static inline uint32_t sh_call(uint32_t op, void *arg)
{
	register uint32_t r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
	return r0;
}

static inline int sh_open(const char *path, int mode)
{
	uint32_t args[3];
	args[0] = (uint32_t)(uintptr_t)path;
	args[1] = (uint32_t)mode;
	args[2] = (uint32_t)__builtin_strlen(path);
	return (int)sh_call(SH_SYS_OPEN, args);
}

static inline int sh_write(int fd, const void *buf, size_t len)
{
	uint32_t args[3];
	args[0] = (uint32_t)fd;
	args[1] = (uint32_t)(uintptr_t)buf;
	args[2] = (uint32_t)len;
	return (int)sh_call(SH_SYS_WRITE, args);
}

static inline int sh_seek(int fd, uint32_t pos)
{
	uint32_t args[2];
	args[0] = (uint32_t)fd;
	args[1] = pos;
	return (int)sh_call(SH_SYS_SEEK, args);
}

/* Framebuffer header — matches ove-dashboard-bridge.py protocol */
#define FB_MAGIC 0x42465854 /* "TXFB" */
#define FB_FORMAT 0	    /* RGB565 */

struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

struct shmem_display_data {
	int sh_fd;
	uint16_t width;
	uint16_t height;
	uint16_t framebuffer[]; /* flexible array — sized at init */
};

/* Static framebuffer (480*272*2 = 261120 bytes) */
static uint16_t fb_pixels[480 * 272];
static struct shmem_display_data display_data;

static int shmem_display_init(const struct device *dev)
{
	struct shmem_display_data *data = dev->data;

	data->width = DT_INST_PROP(0, width);
	data->height = DT_INST_PROP(0, height);

	/* Open shmem file via semihosting. mode 7 = "r+b" */
	data->sh_fd = sh_open("/dev/shm/ove-fb", 7);
	/* sh_fd < 0 is fine — means no display viewer (headless) */

	memset(fb_pixels, 0, sizeof(fb_pixels));
	return 0;
}

static int shmem_display_write(const struct device *dev, const uint16_t x, const uint16_t y,
			       const struct display_buffer_descriptor *desc, const void *buf)
{
	struct shmem_display_data *data = dev->data;
	const uint8_t *src = buf;
	uint32_t w = desc->width;
	uint32_t h = desc->height;
	uint32_t pitch = desc->pitch;

	/* Copy pixels into local framebuffer */
	for (uint32_t row = 0; row < h; row++) {
		uint32_t fb_idx = (y + row) * data->width + x;
		memcpy(&fb_pixels[fb_idx], src + row * pitch * 2, w * 2);
	}

	/* Flush to shmem after each write */
	if (data->sh_fd >= 0) {
		struct fb_header hdr = {
			.magic = FB_MAGIC,
			.width = data->width,
			.height = data->height,
			.format = FB_FORMAT,
			.dirty = 1,
		};
		sh_seek(data->sh_fd, 0);
		sh_write(data->sh_fd, &hdr, sizeof(hdr));
		sh_write(data->sh_fd, fb_pixels, data->width * data->height * 2);
	}

	return 0;
}

static int shmem_display_blanking_off(const struct device *dev)
{
	return 0;
}

static int shmem_display_blanking_on(const struct device *dev)
{
	return 0;
}

static void shmem_display_get_capabilities(const struct device *dev,
					   struct display_capabilities *caps)
{
	struct shmem_display_data *data = dev->data;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = data->width;
	caps->y_resolution = data->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
}

static const struct display_driver_api shmem_display_api = {
	.write = shmem_display_write,
	.blanking_off = shmem_display_blanking_off,
	.blanking_on = shmem_display_blanking_on,
	.get_capabilities = shmem_display_get_capabilities,
};

DEVICE_DT_INST_DEFINE(0, shmem_display_init, NULL, &display_data, NULL, POST_KERNEL,
		      CONFIG_DISPLAY_INIT_PRIORITY, &shmem_display_api);
