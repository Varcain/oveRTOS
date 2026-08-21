/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal generated-rootfs contract used by the host-side composition test.
 * The LXP parser is mocked there, so this is deliberately not a CPIO image.
 */

#ifndef OVE_TEST_LOADER_ROOTFS_IMAGE_H
#define OVE_TEST_LOADER_ROOTFS_IMAGE_H

static const unsigned char ove_test_rootfs_cpio[] = {0x30, 0x37, 0x30, 0x37};
static const unsigned long ove_test_rootfs_cpio_len = sizeof(ove_test_rootfs_cpio);

#endif /* OVE_TEST_LOADER_ROOTFS_IMAGE_H */
