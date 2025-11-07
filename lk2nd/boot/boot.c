// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2023 Nikita Travkin <nikita@trvn.ru> */

#include <debug.h>
#include <lib/bio.h>
#include <lib/fs.h>
#include <list.h>
#include <stdlib.h>
#include <target.h>

#include <lk2nd/boot.h>
#include <lk2nd/hw/bdev.h>

#include "ab.h"
#include "boot.h"

#ifndef LK2ND_BOOT_MIN_SIZE
#define LK2ND_BOOT_MIN_SIZE (16 * 1024 * 1024)
#endif

/**
 * lk2nd_scan_devices() - Scan filesystems and try to boot
 */
static void lk2nd_scan_devices(void)
{
	struct bdev_struct *bdevs = bio_get_bdevs();
	char mountpoint[128];
	bdev_t *bdev;
	int ret;
	const char *base_device = NULL;
	uint64_t target_offset = 0;
	char subdev_name[64];

	/* Early default A/B bootstrap (generic, not RAUC-specific)
	 * Only applies if A/B not already initialized by extlinux (env + offsets).
	 * Defaults chosen for Fairphone 2 userdata layout:
	 *   env partition: mmcblk0p20 (resolved internally)
	 *   env offset:    0x10000
	 *   env size:      0x20000
	 *   slot A offset: 0x00100000
	 *   slot B offset: 0x04100000
	 */
	if (!lk2nd_boot_ab_get_base_device()) {
		lk2nd_boot_ab_init("mmcblk0p20", 0x10000, 0x20000);
		lk2nd_boot_ab_set_offsets(0x00100000ULL, 0x04100000ULL);
	}

	dprintf(INFO, "boot: Trying to boot from the file system...\n");

	/* Check if A/B boot is configured: base device is the U-Boot env partition */
	base_device = lk2nd_boot_ab_get_base_device();
	target_offset = lk2nd_boot_ab_get_offset();

	if (base_device) {
		dprintf(INFO, "boot: A/B mode - base device '%s'", base_device);
		if (target_offset > 0)
			dprintf(INFO, " at offset 0x%llx\n", target_offset);
		else
			dprintf(INFO, " (no offset)\n");
	}

	/* If A/B is configured, prefer direct mount of the base device or its subdevice */
	if (base_device) {
		if (target_offset > 0) {
			/* Create a subdevice starting at the specified offset */
			bdev_t *parent_bdev = bio_open(base_device);
			if (parent_bdev) {
			size_t block_size = parent_bdev->block_size;
			bnum_t start_block = target_offset / block_size;
			/* Use remaining size from offset to end of partition */
			size_t subdev_len = parent_bdev->block_count - start_block;

				snprintf(subdev_name, sizeof(subdev_name), "ab-slot");

				ret = bio_publish_subdevice(base_device, subdev_name, start_block, subdev_len);
				bio_close(parent_bdev);

			if (ret == 0) {
				dprintf(INFO, "boot: Created subdevice '%s' at block %u (0x%llx bytes)\n",
					subdev_name, (unsigned)start_block, target_offset);

					/* Mount directly and try to boot */
					snprintf(mountpoint, sizeof(mountpoint), "/%s", subdev_name);
					ret = fs_mount(mountpoint, "ext2", subdev_name);
					if (ret >= 0) {
						if (DEBUGLEVEL >= SPEW) {
							dprintf(SPEW, "Scanning %s ...\n", subdev_name);
							dprintf(SPEW, "%s\n", mountpoint);
							lk2nd_print_file_tree(mountpoint, " ");
						}
						lk2nd_try_extlinux(mountpoint);
						return;
					} else {
						dprintf(CRITICAL, "boot: Failed to mount subdevice '%s'\n", subdev_name);
					}
			} else {
				dprintf(CRITICAL, "boot: Failed to create subdevice: %d\n", ret);
			}
			} else {
				dprintf(CRITICAL, "boot: Failed to open base device '%s'\n", base_device);
		}
		} else {
			/* No offset: mount the base device directly */
			snprintf(mountpoint, sizeof(mountpoint), "/%s", base_device);
			ret = fs_mount(mountpoint, "ext2", base_device);
			if (ret >= 0) {
				if (DEBUGLEVEL >= SPEW) {
					dprintf(SPEW, "Scanning %s ...\n", base_device);
					dprintf(SPEW, "%s\n", mountpoint);
					lk2nd_print_file_tree(mountpoint, " ");
				}
				lk2nd_try_extlinux(mountpoint);
				return;
			}
		}
	}

	/* Fallback: scan all devices as before (non-A/B mode) */
	list_for_every_entry(&bdevs->list, bdev, bdev_t, node) {

		/* Skip top level block devices. */
		if (!bdev->is_leaf)
			continue;

		/* In non-A/B mode we scan all partitions; otherwise we already tried direct mount */

		/*
		 * Skip partitions that are too small to have a boot fs on.
		 *
		 * 'boot' partition is explicitly allowed to have a small fs on it
		 * in case one installs next stage bootloader package (i.e. u-boot)
		 * there but still wants to make use of lk2nd's device database.
		 */
		if (bdev->size < LK2ND_BOOT_MIN_SIZE &&
		    !(bdev->label && !strncmp(bdev->label, "boot", strlen("boot"))))
			continue;

		snprintf(mountpoint, sizeof(mountpoint), "/%s", bdev->name);
		ret = fs_mount(mountpoint, "ext2", bdev->name);
		if (ret < 0)
			continue;

		if (DEBUGLEVEL >= SPEW) {
			dprintf(SPEW, "Scanning %s ...\n", bdev->name);
			dprintf(SPEW, "%s\n", mountpoint);
			lk2nd_print_file_tree(mountpoint, " ");
		}

		lk2nd_try_extlinux(mountpoint);

	}

	dprintf(INFO, "boot: Bootable file system not found. Reverting to android boot.\n");
}

/**
 * lk2nd_boot() - Try to boot the OS.
 *
 * This method is supposed to be called from aboot.
 * If appropriate OS is found, it will be booted, and this
 * method will never return.
 */
void lk2nd_boot(void)
{
	static bool init_done = false;

	if (!init_done) {
		lk2nd_bdev_init();
		init_done = true;
	}

	lk2nd_scan_devices();
}
