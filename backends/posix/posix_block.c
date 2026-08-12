/* POSIX reference block backend: optional regular-file image. */
#include "ove/block_backend.h"

#ifdef CONFIG_OVE_BLOCK

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *image_path(void)
{
	return getenv("OVE_BLOCK_IMAGE");
}

static int g_media_present = -1;
static uint32_t g_media_generation = 1u;
static dev_t g_media_device;
static ino_t g_media_inode;

static void note_media(int present, const struct stat *st)
{
	int changed = 0;
	if (present && g_media_present > 0 && st &&
	    (st->st_dev != g_media_device || st->st_ino != g_media_inode))
		changed = 1;
	int old = __atomic_exchange_n(&g_media_present, present, __ATOMIC_ACQ_REL);
	if ((old >= 0 && old != present) || changed)
		(void)__atomic_add_fetch(&g_media_generation, 1u, __ATOMIC_ACQ_REL);
	if (present && st) {
		g_media_device = st->st_dev;
		g_media_inode = st->st_ino;
	}
}

int ove_block_backend_get_info(struct ove_block_info *out)
{
	const char *path = image_path();
	if (!out || !path) {
		if (!path)
			note_media(0, NULL);
		return path ? OVE_ERR_INVALID_PARAM : OVE_ERR_NOT_REGISTERED;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		note_media(0, NULL);
		return OVE_ERR_NOT_REGISTERED;
	}
	note_media(1, &st);
	memset(out, 0, sizeof(*out));
	out->block_count = (uint64_t)st.st_size / 512u;
	out->logical_block_size = 512u;
	out->erase_block_size = 512u;
	out->flags = OVE_BLOCK_F_MEDIA_PRESENT;
	out->generation = __atomic_load_n(&g_media_generation, __ATOMIC_ACQUIRE);
	return OVE_OK;
}

static int transfer(uint64_t first, uint32_t count, void *buffer, int write)
{
	const char *path = image_path();
	if (!path || (!buffer && count) || first > UINT64_MAX / 512u ||
	    first * 512u > (uint64_t)INT64_MAX)
		return path ? OVE_ERR_INVALID_PARAM : OVE_ERR_NOT_REGISTERED;
#if SIZE_MAX < UINT64_MAX
	if ((uint64_t)count * 512u > SIZE_MAX)
		return OVE_ERR_INVALID_PARAM;
#endif
	int fd = open(path, write ? O_RDWR : O_RDONLY);
	if (fd < 0)
		return OVE_ERR_IO;
	size_t length = (size_t)count * 512u;
	off_t offset = (off_t)(first * 512u);
	ssize_t done = write ? pwrite(fd, buffer, length, offset)
			     : pread(fd, buffer, length, offset);
	int saved = errno;
	close(fd);
	errno = saved;
	return done == (ssize_t)length ? OVE_OK : OVE_ERR_IO;
}

int ove_block_backend_read(uint64_t first_block, uint32_t block_count, void *buffer)
{
	return transfer(first_block, block_count, buffer, 0);
}
int ove_block_backend_write(uint64_t first_block, uint32_t block_count, const void *buffer)
{
	return transfer(first_block, block_count, (void *)buffer, 1);
}
int ove_block_backend_sync(void)
{
	const char *path = image_path();
	if (!path)
		return OVE_ERR_NOT_REGISTERED;
	int fd = open(path, O_RDWR);
	if (fd < 0)
		return OVE_ERR_IO;
	int rc = fsync(fd);
	close(fd);
	return rc == 0 ? OVE_OK : OVE_ERR_IO;
}

#endif /* CONFIG_OVE_BLOCK */
