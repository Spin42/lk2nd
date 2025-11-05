// SPDX-License-Identifier: BSD-3-Clause
/* U-Boot environment support for lk2nd A/B partitioning (RAUC-compatible) */

#include <debug.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <lib/bio.h>
#include <crc32.h>

#include "ubootenv.h"

#define UBOOT_ENV_FLAG_ACTIVE 1

/* Calculate CRC32 for U-Boot environment */
static uint32_t uboot_env_crc(const uint8_t *data, size_t len)
{
	return crc32(0, data, len);
}

/* Parse RAUC boot variables from environment and cache them */
static void uboot_env_parse_rauc_vars(struct uboot_env *env)
{
	const char *boot_order, *boot_a_left, *boot_b_left;

	/* Parse BOOT_ORDER (default: "A B") */
	boot_order = uboot_env_get(env, "BOOT_ORDER");
	if (boot_order) {
		strlcpy(env->boot_order, boot_order, sizeof(env->boot_order));
	} else {
		strlcpy(env->boot_order, "A B", sizeof(env->boot_order));
		uboot_env_set(env, "BOOT_ORDER", "A B");
	}

	/* Parse BOOT_A_LEFT */
	boot_a_left = uboot_env_get(env, "BOOT_A_LEFT");
	env->boot_a_left = boot_a_left ? atoi(boot_a_left) : UBOOT_ENV_MAX_BOOT_ATTEMPTS;
	if (!boot_a_left) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", UBOOT_ENV_MAX_BOOT_ATTEMPTS);
		uboot_env_set(env, "BOOT_A_LEFT", buf);
	}

	/* Parse BOOT_B_LEFT */
	boot_b_left = uboot_env_get(env, "BOOT_B_LEFT");
	env->boot_b_left = boot_b_left ? atoi(boot_b_left) : UBOOT_ENV_MAX_BOOT_ATTEMPTS;
	if (!boot_b_left) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", UBOOT_ENV_MAX_BOOT_ATTEMPTS);
		uboot_env_set(env, "BOOT_B_LEFT", buf);
	}

	dprintf(INFO, "RAUC boot config: BOOT_ORDER='%s' BOOT_A_LEFT=%d BOOT_B_LEFT=%d\n",
		env->boot_order, env->boot_a_left, env->boot_b_left);
}

int uboot_env_init(struct uboot_env *env, const char *partition, uint64_t offset, size_t size)
{
	bdev_t *bdev;
	uint8_t *buffer;
	ssize_t ret;

	if (!env || !partition || size == 0)
		return -1;

	memset(env, 0, sizeof(*env));

	bdev = bio_open(partition);
	if (!bdev) {
		dprintf(CRITICAL, "ubootenv: Failed to open partition %s\n", partition);
		return -1;
	}

	/* Allocate buffer for environment (header + data) */
	buffer = malloc(size);
	if (!buffer) {
		bio_close(bdev);
		return -1;
	}

	/* Read environment from partition */
	ret = bio_read(bdev, buffer, offset, size);
	bio_close(bdev);

	if (ret != (ssize_t)size) {
		dprintf(CRITICAL, "ubootenv: Failed to read environment: %ld\n", ret);
		free(buffer);
		return -1;
	}

	/* Parse header: CRC32 (4 bytes) + flags (1 byte) + data */
	env->crc = *(uint32_t *)buffer;
	env->flags = buffer[4];
	env->size = size;
	env->data_size = size - 5;  /* Subtract header size */

	/* Allocate and copy data */
	env->data = malloc(env->data_size);
	if (!env->data) {
		free(buffer);
		return -1;
	}
	memcpy(env->data, buffer + 5, env->data_size);
	free(buffer);

	/* Verify CRC */
	uint32_t calculated_crc = uboot_env_crc((uint8_t *)env->data, env->data_size);
	if (calculated_crc != env->crc) {
		dprintf(INFO, "ubootenv: CRC mismatch (calculated: 0x%x, stored: 0x%x), initializing clean env\n",
			calculated_crc, env->crc);
		/* Initialize with empty environment */
		memset(env->data, 0, env->data_size);
		env->dirty = true;
	}

	/* Parse RAUC A/B boot variables */
	uboot_env_parse_rauc_vars(env);

	dprintf(INFO, "ubootenv: Initialized from %s at offset 0x%llx\n", partition, offset);

	return 0;
}

const char *uboot_env_get(struct uboot_env *env, const char *key)
{
	char *ptr = env->data;
	size_t key_len = strlen(key);

	if (!env || !key || !env->data)
		return NULL;

	/* Scan through environment data */
	while (ptr < env->data + env->data_size && *ptr != '\0') {
		/* Check if this entry matches our key */
		if (strncmp(ptr, key, key_len) == 0 && ptr[key_len] == '=') {
			return ptr + key_len + 1;  /* Return pointer to value */
		}
		/* Move to next entry */
		ptr += strlen(ptr) + 1;
	}

	return NULL;
}

int uboot_env_set(struct uboot_env *env, const char *key, const char *value)
{
	char *ptr = env->data;
	char *insert_pos = NULL;
	size_t key_len = strlen(key);
	size_t value_len = strlen(value);
	size_t entry_len = key_len + 1 + value_len + 1;  /* key=value\0 */
	char *existing = NULL;
	size_t existing_len = 0;

	if (!env || !key || !value)
		return -1;

	/* Find if key already exists */
	while (ptr < env->data + env->data_size && *ptr != '\0') {
		if (strncmp(ptr, key, key_len) == 0 && ptr[key_len] == '=') {
			existing = ptr;
			existing_len = strlen(ptr) + 1;
			break;
		}
		ptr += strlen(ptr) + 1;
	}

	if (existing) {
		/* Update existing entry */
		if (entry_len <= existing_len) {
			/* New value fits in place - safe to use snprintf */
			int ret = snprintf(existing, existing_len, "%s=%s", key, value);
			if (ret < 0 || (size_t)ret >= existing_len) {
				dprintf(CRITICAL, "ubootenv: Failed to format %s=%s\n", key, value);
				return -1;
			}
		} else {
			/* Need to move data - remove old and append new */
			size_t remaining = env->data_size - (existing - env->data) - existing_len;
			memmove(existing, existing + existing_len, remaining);

			/* Find new end position */
			ptr = env->data;
			while (ptr < env->data + env->data_size && *ptr != '\0') {
				ptr += strlen(ptr) + 1;
			}
			insert_pos = ptr;
		}
	} else {
		/* Find end of environment */
		ptr = env->data;
		while (ptr < env->data + env->data_size && *ptr != '\0') {
			ptr += strlen(ptr) + 1;
		}
		insert_pos = ptr;
	}

	/* Append new entry if needed */
	if (insert_pos) {
		size_t available = env->data_size - (insert_pos - env->data);
		if (entry_len + 1 > available) {  /* +1 for final \0 */
			dprintf(CRITICAL, "ubootenv: Not enough space for %s=%s\n", key, value);
			return -1;
		}
		snprintf(insert_pos, available, "%s=%s", key, value);
		insert_pos[entry_len] = '\0';  /* Double null terminator */
	}

	env->dirty = true;
	return 0;
}

int uboot_env_save(struct uboot_env *env, const char *partition, uint64_t offset)
{
	bdev_t *bdev;
	uint8_t *buffer;
	ssize_t ret;

	if (!env || !partition || !env->dirty)
		return 0;

	/* Recalculate CRC */
	env->crc = uboot_env_crc((uint8_t *)env->data, env->data_size);

	/* Prepare buffer with header */
	buffer = malloc(env->size);
	if (!buffer)
		return -1;

	*(uint32_t *)buffer = env->crc;
	buffer[4] = UBOOT_ENV_FLAG_ACTIVE;
	memcpy(buffer + 5, env->data, env->data_size);

	/* Write to partition */
	bdev = bio_open(partition);
	if (!bdev) {
		free(buffer);
		return -1;
	}

	ret = bio_write(bdev, buffer, offset, env->size);
	bio_close(bdev);
	free(buffer);

	if (ret != (ssize_t)env->size) {
		dprintf(CRITICAL, "ubootenv: Failed to write environment: %ld\n", ret);
		return -1;
	}

	env->dirty = false;
	dprintf(INFO, "ubootenv: Saved to %s at offset 0x%llx\n", partition, offset);
	return 0;
}

void uboot_env_free(struct uboot_env *env)
{
	if (env && env->data) {
		free(env->data);
		env->data = NULL;
	}
}

/* RAUC-style A/B partition management functions */

/*
 * Get current boot slot based on BOOT_ORDER and remaining attempts
 * Returns the first slot in BOOT_ORDER that has attempts remaining
 */
char uboot_env_get_boot_slot(struct uboot_env *env)
{
	char *slot, *saveptr;
	char order_copy[sizeof(env->boot_order)];

	if (!env)
		return 'A';  /* Safe default */

	/* Make a copy since strtok_r modifies the string */
	strlcpy(order_copy, env->boot_order, sizeof(order_copy));

	/* Iterate through BOOT_ORDER to find first slot with attempts remaining */
	for (slot = strtok_r(order_copy, " ", &saveptr); slot;
	     slot = strtok_r(NULL, " ", &saveptr)) {

		if (slot[0] == 'A' && env->boot_a_left > 0) {
			return 'A';
		} else if (slot[0] == 'B' && env->boot_b_left > 0) {
			return 'B';
		}
	}

	/* All slots exhausted - return first slot in order anyway as last resort */
	dprintf(CRITICAL, "ubootenv: All boot slots exhausted!\n");
	return env->boot_order[0];
}

/*
 * Decrement boot counter for the given slot (called before boot attempt)
 * This implements the RAUC pre-boot logic
 */
int uboot_env_decrement_boot_left(struct uboot_env *env, char slot)
{
	char var_name[16];
	char value_str[16];
	int *counter;

	if (!env)
		return -1;

	if (slot == 'A') {
		counter = &env->boot_a_left;
		strlcpy(var_name, "BOOT_A_LEFT", sizeof(var_name));
	} else if (slot == 'B') {
		counter = &env->boot_b_left;
		strlcpy(var_name, "BOOT_B_LEFT", sizeof(var_name));
	} else {
		dprintf(CRITICAL, "ubootenv: Invalid slot '%c'\n", slot);
		return -1;
	}

	if (*counter > 0) {
		(*counter)--;
		snprintf(value_str, sizeof(value_str), "%d", *counter);
		uboot_env_set(env, var_name, value_str);

		dprintf(INFO, "ubootenv: Slot %c attempts remaining: %d\n", slot, *counter);
		return 0;
	}

	dprintf(CRITICAL, "ubootenv: Slot %c has no attempts left\n", slot);
	return -1;
}

/*
 * Get next slot from BOOT_ORDER after current_slot
 * Returns '\0' if no more slots available
 */
char uboot_env_get_next_slot(struct uboot_env *env, char current_slot)
{
	char *slot, *saveptr;
	char order_copy[sizeof(env->boot_order)];
	bool found_current = false;

	if (!env)
		return '\0';

	strlcpy(order_copy, env->boot_order, sizeof(order_copy));

	/* Find current slot in order, then return the next one */
	for (slot = strtok_r(order_copy, " ", &saveptr); slot;
	     slot = strtok_r(NULL, " ", &saveptr)) {

		if (found_current) {
			/* Return next slot if it has attempts remaining */
			if (slot[0] == 'A' && env->boot_a_left > 0)
				return 'A';
			else if (slot[0] == 'B' && env->boot_b_left > 0)
				return 'B';
		}

		if (slot[0] == current_slot)
			found_current = true;
	}

	return '\0';  /* No more slots */
}
