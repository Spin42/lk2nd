// SPDX-License-Identifier: BSD-3-Clause
/* A/B partition boot integration for lk2nd (offset-only) */

#include <debug.h>
#include <stdlib.h>
#include <string.h>
#include <lib/bio.h>
#include <list.h>

#include "boot.h"
#include "ubootenv.h"

/*
 * Generic A/B Boot Implementation (offset-only)
 *
 * Uses U-Boot environment variables stored at a fixed offset within a base
 * partition (e.g. userdata). Offsets for slots A/B are byte offsets within
 * the same base partition.
 *
 * Optional configuration via extlinux.conf (global directives):
 *   ab_env_part <partition>
 *   ab_env_offset <bytes>
 *   ab_env_size <bytes>
 *   ab_slot_offset_a <bytes>
 *   ab_slot_offset_b <bytes>
 *
 * Flow:
 * 1. Initialize: Read U-Boot env from the base device at configured offset
 * 2. Select slot: Determine current slot from BOOT_ORDER and remaining attempts
 * 3. Pre-boot: Decrement boot counter (BOOT_<slot>_LEFT) and save the env
 * 4. Boot: Publish a subdevice at the selected slot offset, mount it, and load extlinux
 * 5. Userspace: On success, reset counters in the U-Boot env (e.g. fw_setenv)
 *
 * Environment Variables:
 * - BOOT_ORDER: Space-separated slot list to try (e.g., "A B")
 * - BOOT_A_LEFT: Remaining boot attempts for slot A
 * - BOOT_B_LEFT: Remaining boot attempts for slot B
 */

/* Global A/B boot state */
static struct {
	struct uboot_env env;
	char partition[64];
	uint64_t offset;
	size_t size;
	bool initialized;
	char current_slot;  /* Cached current boot slot */
	uint64_t boot_offset_a;  /* Boot partition offset for slot A (0 = no offset) */
	uint64_t boot_offset_b;  /* Boot partition offset for slot B (0 = no offset) */
} ab_state = {0};

/*
 * Resolve a base device spec (from extlinux) to an actual bdev name:
 * - Try the name as-is
 * - If it's a Linux-style name like "mmcblk0pN", map to our wrapper device
 *   name "wrp0pN"
 * - Otherwise, try to find a device by GPT label match
 * Returns true and writes normalized name into out if found.
 */
static bool resolve_base_device(const char *spec, char *out, size_t out_len)
{
	bdev_t *b;

	if (!spec || !out || out_len == 0)
		return false;

	/* 1) Try as-is */
	b = bio_open(spec);
	if (b) {
		strlcpy(out, spec, out_len);
		bio_close(b);
		return true;
	}

	/* 2) Map Linux-style mmcblkXpN -> wrp0p(N-1) (wrapper is zero-based) */
	if (!strncmp(spec, "mmcblk", 6)) {
		const char *p = strrchr(spec, 'p');
		if (p && *(p+1)) {
			unsigned part = 0;
			const char *num = p + 1;
			/* parse decimal partition number */
			while (*num >= '0' && *num <= '9') {
				part = part * 10 + (*num - '0');
				num++;
			}
			if (part > 0) {
				char mapped[32];
				/* wrapper uses wrp0p<zero-based> */
				snprintf(mapped, sizeof(mapped), "wrp0p%u", part - 1);
				b = bio_open(mapped);
				if (b) {
					strlcpy(out, mapped, out_len);
					bio_close(b);
					return true;
				}
			}
		}
	}

	/* 3) Search by GPT label */
	{
		struct bdev_struct *bdevs = bio_get_bdevs();
		bdev_t *iter;
		list_for_every_entry(&bdevs->list, iter, bdev_t, node) {
			if (!iter->is_leaf)
				continue;
			if (iter->label && !strcmp(iter->label, spec)) {
				strlcpy(out, iter->name, out_len);
				return true;
			}
		}
	}

	return false;
}

/*
 * Initialize A/B boot from U-Boot environment
 * partition: Name of partition containing uboot.env (typically "userdata")
 * offset: Byte offset within partition where uboot.env starts
 * size: Size of uboot.env in bytes (typically 0x20000 / 128KB)
 */
void lk2nd_boot_ab_init(const char *partition, uint64_t offset, size_t size)
{
	int ret;
	char resolved[64];

	if (ab_state.initialized) {
		dprintf(INFO, "A/B boot already initialized\n");
		return;
	}

	if (!partition) {
		dprintf(CRITICAL, "A/B boot: partition name required\n");
		return;
	}

	if (size == 0)
		size = UBOOT_ENV_DEFAULT_SIZE;

	if (!resolve_base_device(partition, resolved, sizeof(resolved))) {
		dprintf(CRITICAL, "A/B boot: Failed to resolve base device '%s'\n", partition);
		return;
	}

	dprintf(INFO, "Initializing RAUC-style A/B boot from %s (resolved from '%s') at offset 0x%llx (size: 0x%zx)\n",
		resolved, partition, offset, size);

	ret = uboot_env_init(&ab_state.env, resolved, offset, size);
	if (ret < 0) {
		dprintf(CRITICAL, "A/B boot: Failed to initialize U-Boot environment: %d\n", ret);
		return;
	}

	strlcpy(ab_state.partition, resolved, sizeof(ab_state.partition));
	ab_state.offset = offset;
	ab_state.size = size;
	ab_state.initialized = true;

	/* Determine initial boot slot based on BOOT_ORDER and counters */
	ab_state.current_slot = uboot_env_get_boot_slot(&ab_state.env);

	dprintf(INFO, "RAUC A/B boot initialized - current slot: %c\n", ab_state.current_slot);
}

/*
 * Get current boot slot (A or B)
 * Returns the slot that should be booted based on BOOT_ORDER and remaining attempts
 * Returns 'A' if A/B boot is not initialized (standard boot mode)
 */
char lk2nd_boot_ab_get_slot(void)
{
	if (!ab_state.initialized) {
		/* A/B not configured - return 'A' which won't match any _A suffix
		 * allowing standard extlinux labels to work normally */
		return 'A';
	}

	return ab_state.current_slot;
}

/*
 * Called before attempting to boot - decrements boot counter
 * This implements RAUC's pre-boot logic where we decrement BOOT_<slot>_LEFT
 * Does nothing if A/B is not initialized (standard boot mode)
 */
void lk2nd_boot_ab_pre_boot(void)
{
	char next_slot;

	if (!ab_state.initialized) {
		/* A/B not configured - skip A/B boot logic entirely */
		return;
	}

	dprintf(INFO, "A/B pre-boot: Attempting to boot slot %c\n", ab_state.current_slot);

	/* Decrement boot counter for current slot */
	if (uboot_env_decrement_boot_left(&ab_state.env, ab_state.current_slot) < 0) {
		/* Current slot exhausted, try next slot in BOOT_ORDER */
		next_slot = uboot_env_get_next_slot(&ab_state.env, ab_state.current_slot);

		if (next_slot != '\0') {
			dprintf(CRITICAL, "Slot %c exhausted, switching to slot %c\n",
				ab_state.current_slot, next_slot);
			ab_state.current_slot = next_slot;
			uboot_env_decrement_boot_left(&ab_state.env, ab_state.current_slot);
		} else {
			dprintf(CRITICAL, "All boot slots exhausted! Attempting slot %c anyway\n",
				ab_state.current_slot);
		}
	}

	/* Save state before booting (critical for boot counting) */
	uboot_env_save(&ab_state.env, ab_state.partition, ab_state.offset);
}

/* Return the base device name used for boot (same as U-Boot env partition) */
const char *lk2nd_boot_ab_get_base_device(void)
{
	if (!ab_state.initialized)
		return NULL;
	return ab_state.partition;
}

/*
 * Set boot partition offsets for A/B slots
 * Use this when boot partitions are sub-partitions within a main partition
 * (e.g., boot-a at offset 0x100000 within userdata)
 */
void lk2nd_boot_ab_set_offsets(uint64_t offset_a, uint64_t offset_b)
{
	ab_state.boot_offset_a = offset_a;
	ab_state.boot_offset_b = offset_b;

	dprintf(INFO, "A/B boot offsets: A=0x%llx, B=0x%llx\n",
		ab_state.boot_offset_a, ab_state.boot_offset_b);
}

/* No partition names anymore: only offsets per slot are used */

/*
 * Get the boot partition offset for the current slot
 * Returns 0 if not configured or no offset needed
 */
uint64_t lk2nd_boot_ab_get_offset(void)
{
	if (!ab_state.initialized)
		return 0;

	if (ab_state.current_slot == 'A')
		return ab_state.boot_offset_a;
	else if (ab_state.current_slot == 'B')
		return ab_state.boot_offset_b;

	return 0;
}
