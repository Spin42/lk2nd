/* SPDX-License-Identifier: BSD-3-Clause */
/* U-Boot environment support for lk2nd A/B partitioning (RAUC-compatible) */

#ifndef LK2ND_BOOT_UBOOTENV_H
#define LK2ND_BOOT_UBOOTENV_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/*
 * RAUC U-Boot Boot Logic
 * ----------------------
 * RAUC uses the following U-Boot environment variables for A/B boot logic:
 *
 * BOOT_ORDER: Space-separated list of boot slots to try in order (e.g., "A B")
 * BOOT_A_LEFT: Number of remaining boot attempts for slot A
 * BOOT_B_LEFT: Number of remaining boot attempts for slot B
 *
 * Boot flow:
 * 1. Read BOOT_ORDER to get list of slots
 * 2. For each slot in order:
 *    - Check BOOT_<slot>_LEFT counter
 *    - If counter > 0: decrement it, try to boot this slot
 *    - If counter == 0: skip to next slot
 * 3. If boot succeeds, userspace calls rauc status mark-good to reset counter
 * 4. If all slots exhausted, system may enter recovery
 */

#define UBOOT_ENV_DEFAULT_SIZE 0x20000  /* 128KB - typical U-Boot env size */
#define UBOOT_ENV_MAX_BOOT_ATTEMPTS 3   /* Default boot attempts per slot */

/* U-Boot environment context */
struct uboot_env {
	uint32_t crc;
	uint8_t flags;
	char *data;
	size_t size;
	size_t data_size;
	bool dirty;
	bool has_flags;        /* true: format is [CRC][flags][data], false: [CRC][data] */

	/* Parsed RAUC-style boot state (cached for performance) */
	char boot_order[32];     /* BOOT_ORDER value (e.g., "A B") */
	int boot_a_left;         /* BOOT_A_LEFT counter */
	int boot_b_left;         /* BOOT_B_LEFT counter */
};

/* Initialize U-Boot environment from partition at given offset */
int uboot_env_init(struct uboot_env *env, const char *partition, uint64_t offset, size_t size);

/* Get environment variable value (returns pointer into env->data) */
const char *uboot_env_get(struct uboot_env *env, const char *key);

/* Set environment variable and mark environment as dirty */
int uboot_env_set(struct uboot_env *env, const char *key, const char *value);

/* Save environment back to partition (only if dirty) */
int uboot_env_save(struct uboot_env *env, const char *partition, uint64_t offset);

/* Free environment resources */
void uboot_env_free(struct uboot_env *env);

/* RAUC-style A/B boot management */

/* Get current boot slot based on BOOT_ORDER and remaining attempts */
char uboot_env_get_boot_slot(struct uboot_env *env);

/* Decrement boot counter for current slot (called before boot attempt) */
int uboot_env_decrement_boot_left(struct uboot_env *env, char slot);

/* Get next slot from BOOT_ORDER, or '\0' if no more slots */
char uboot_env_get_next_slot(struct uboot_env *env, char current_slot);

#endif /* LK2ND_BOOT_UBOOTENV_H */
