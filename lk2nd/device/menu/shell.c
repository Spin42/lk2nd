// SPDX-License-Identifier: BSD-3-Clause
/*
 * lk2nd serial shell - a small U-Boot-style command prompt.
 *
 * Entered when the boot countdown is interrupted over serial. Command
 * names follow U-Boot where an equivalent exists (printenv, setenv,
 * saveenv, boot, ls, part, md, ums, reset, poweroff, version).
 */

#include <compiler.h>
#include <debug.h>
#include <display_menu.h>
#include <kernel/thread.h>
#include <platform.h>
#include <printf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdbool.h>
#include <lib/bio.h>
#include <lib/fs.h>

#include <lk2nd/boot.h>
#include <lk2nd/device/menu.h>
#include <lk2nd/hw/bdev.h>
#include <lk2nd/ramoops.h>
#include <lk2nd/version.h>

#include "../device.h"
#include "../../boot/ab.h"

// Defined in app/aboot/aboot.c
extern void cmd_continue(const char *arg, void *data, unsigned sz);
extern unsigned boot_into_recovery;
extern int dgetc(char *c, bool wait);

// Defined in lk2nd/boot/util.c
extern void lk2nd_print_file_tree(char *root, char *prefix);

// Defined in platform/msm_shared/debug.c
extern volatile int debug_uart_suppress;

#ifdef LK2ND_UMS
extern int ums_enter_mode(const char *partition_name);
#endif

#define xstr(s) str(s)
#define str(s) #s

#define SHELL_LINE_LEN  128
#define SHELL_MAX_ARGS  8

struct shell_cmd {
	const char *name;
	const char *usage;
	int (*fn)(int argc, char **argv);
};

static const struct shell_cmd shell_cmds[];

/* Parse a hex ("0x...") or decimal number */
static unsigned long shell_parse_num(const char *s)
{
	unsigned long val = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		while (*s) {
			char c = *s;
			if (c >= '0' && c <= '9')
				val = (val << 4) | (c - '0');
			else if (c >= 'a' && c <= 'f')
				val = (val << 4) | (c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				val = (val << 4) | (c - 'A' + 10);
			else
				break;
			s++;
		}
	} else {
		while (*s >= '0' && *s <= '9') {
			val = val * 10 + (*s - '0');
			s++;
		}
	}

	return val;
}

static char shell_getc(void)
{
	char c;

	while (dgetc(&c, false) != 0)
		thread_sleep(10);
	return c;
}

/* Non-blocking getc with a short timeout, for escape sequences */
static int shell_getc_timeout(char *c)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (dgetc(c, false) == 0)
			return 0;
		thread_sleep(10);
	}
	return -1;
}

static void shell_readline(char *buf, size_t len)
{
	size_t pos = 0;
	char c;

	for (;;) {
		c = shell_getc();

		switch (c) {
		case '\r':
		case '\n':
			printf("\n");
			buf[pos] = '\0';
			return;

		case 0x08: /* backspace */
		case 0x7f: /* DEL */
			if (pos > 0) {
				pos--;
				printf("\b \b");
			}
			break;

		case 0x1b: /* swallow ESC [ x sequences (arrow keys etc.) */
			if (shell_getc_timeout(&c) == 0 && c == '[')
				shell_getc_timeout(&c);
			break;

		default:
			if (c >= 0x20 && pos < len - 1) {
				buf[pos++] = c;
				printf("%c", c);
			}
			break;
		}
	}
}

static int shell_tokenize(char *line, char **argv)
{
	int argc = 0;
	char *p = line;

	while (*p && argc < SHELL_MAX_ARGS) {
		while (*p == ' ')
			*p++ = '\0';
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && *p != ' ')
			p++;
	}

	return argc;
}

/* --- commands --- */

static int cmd_help(int argc, char **argv)
{
	const struct shell_cmd *cmd;

	for (cmd = shell_cmds; cmd->name; cmd++)
		printf("  %-10s %s\n", cmd->name, cmd->usage);
	return 0;
}

static int cmd_version(int argc, char **argv)
{
	printf("lk2nd %s (%s)\n", LK2ND_VERSION, xstr(BOARD));
	if (lk2nd_dev.model)
		printf("device: %s\n", lk2nd_dev.model);
	return 0;
}

static int cmd_printenv(int argc, char **argv)
{
	const char *val;

	if (lk2nd_boot_ab_ensure_init() < 0) {
		printf("environment not available\n");
		return -1;
	}

	if (argc < 2) {
		lk2nd_boot_ab_env_print();
		return 0;
	}

	val = lk2nd_boot_ab_env_get(argv[1]);
	if (!val) {
		printf("## Error: \"%s\" not defined\n", argv[1]);
		return -1;
	}
	printf("%s=%s\n", argv[1], val);
	return 0;
}

static int cmd_setenv(int argc, char **argv)
{
	char value[SHELL_LINE_LEN];
	size_t pos = 0;
	int i;

	if (argc < 2) {
		printf("usage: setenv <name> [value]\n");
		return -1;
	}

	if (lk2nd_boot_ab_ensure_init() < 0) {
		printf("environment not available\n");
		return -1;
	}

	/* Join the remaining arguments, like U-Boot does */
	value[0] = '\0';
	for (i = 2; i < argc; i++) {
		if (pos && pos < sizeof(value) - 1)
			value[pos++] = ' ';
		pos += strlcpy(value + pos, argv[i], sizeof(value) - pos);
	}

	if (lk2nd_boot_ab_env_set(argv[1], value) < 0) {
		printf("## Error: failed to set \"%s\"\n", argv[1]);
		return -1;
	}
	return 0;
}

static int cmd_saveenv(int argc, char **argv)
{
	if (lk2nd_boot_ab_ensure_init() < 0) {
		printf("environment not available\n");
		return -1;
	}

	printf("Saving Environment...\n");
	if (lk2nd_boot_ab_env_save() < 0) {
		printf("## Error: environment save failed\n");
		return -1;
	}
	return 0;
}

static int cmd_slot(int argc, char **argv)
{
	const char *val;

	if (lk2nd_boot_ab_ensure_init() < 0) {
		printf("A/B not available\n");
		return -1;
	}

	printf("current slot: %c (offset 0x%llx)\n",
	       lk2nd_boot_ab_get_slot(), lk2nd_boot_ab_get_offset());
	val = lk2nd_boot_ab_env_get("BOOT_ORDER");
	printf("BOOT_ORDER:  %s\n", val ? val : "(unset)");
	val = lk2nd_boot_ab_env_get("BOOT_A_LEFT");
	printf("BOOT_A_LEFT: %s\n", val ? val : "(unset)");
	val = lk2nd_boot_ab_env_get("BOOT_B_LEFT");
	printf("BOOT_B_LEFT: %s\n", val ? val : "(unset)");
	return 0;
}

static int cmd_boot(int argc, char **argv)
{
	lk2nd_boot();

	/* Only returns if nothing bootable was found */
	printf("no bootable file system found\n");
	return -1;
}

static int cmd_pstore(int argc, char **argv)
{
	if (argc < 2)
		lk2nd_ramoops_print_summary();
	else if (!strcmp(argv[1], "console"))
		lk2nd_ramoops_print_console();
	else if (!strcmp(argv[1], "dump"))
		lk2nd_ramoops_print_dumps();
	else if (!strcmp(argv[1], "zap"))
		lk2nd_ramoops_zap();
	else {
		printf("usage: pstore [console|dump|zap]\n");
		return -1;
	}
	return 0;
}

static int cmd_part(int argc, char **argv)
{
	lk2nd_bdev_init();
	lk2nd_bdev_dump_devices();
	return 0;
}

static int cmd_ls(int argc, char **argv)
{
	char mountpoint[64];
	int ret;

	if (argc < 2) {
		printf("usage: ls <device>  (see 'part' for devices)\n");
		return -1;
	}

	lk2nd_bdev_init();

	snprintf(mountpoint, sizeof(mountpoint), "/%s", argv[1]);
	ret = fs_mount(mountpoint, "ext2", argv[1]);
	if (ret < 0)
		ret = fs_mount(mountpoint, "fat", argv[1]);
	if (ret < 0) {
		printf("failed to mount '%s' (ext2/fat): %d\n", argv[1], ret);
		return -1;
	}

	lk2nd_print_file_tree(mountpoint, " ");
	fs_unmount(mountpoint);
	return 0;
}

static int cmd_md(int argc, char **argv)
{
	unsigned long addr;
	unsigned int count = 64, i;
	const uint32_t *p;

	if (argc < 2) {
		printf("usage: md <hex-addr> [word-count]\n");
		return -1;
	}

	addr = shell_parse_num(argv[1]) & ~3UL;
	if (argc > 2)
		count = shell_parse_num(argv[2]);

	p = (const uint32_t *)addr;
	for (i = 0; i < count; i++) {
		if (i % 4 == 0)
			printf("%08lx:", addr + i * 4);
		printf(" %08x", p[i]);
		if (i % 4 == 3 || i == count - 1)
			printf("\n");
	}
	return 0;
}

#ifdef LK2ND_UMS
static int cmd_ums(int argc, char **argv)
{
	const char *part = (argc > 1) ? argv[1] : xstr(LK2ND_UMS_PARTITION);
	int ret;

	lk2nd_bdev_init();

	printf("entering USB mass storage mode (device '%s')\n", part);
	ret = ums_enter_mode(part);
	if (ret < 0)
		printf("UMS failed: %d\n", ret);
	return ret;
}
#endif

static int cmd_reset(int argc, char **argv)
{
	printf("resetting...\n");
	reboot_device(0);
	return 0;
}

static int cmd_reboot(int argc, char **argv)
{
	if (argc < 2) {
		reboot_device(0);
	} else if (!strcmp(argv[1], "bootloader")) {
		reboot_device(FASTBOOT_MODE);
	} else if (!strcmp(argv[1], "recovery")) {
		boot_into_recovery = 1;
		cmd_continue(NULL, NULL, 0);
	} else if (!strcmp(argv[1], "edl")) {
		reboot_device(EMERGENCY_DLOAD);
	} else {
		printf("usage: reboot [bootloader|recovery|edl]\n");
		return -1;
	}
	return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
	printf("powering off...\n");
	shutdown_device();
	return 0;
}

static const struct shell_cmd shell_cmds[] = {
	{ "help",     "list commands",                              cmd_help },
	{ "version",  "print lk2nd version",                        cmd_version },
	{ "printenv", "[name] - print U-Boot environment",          cmd_printenv },
	{ "setenv",   "<name> [value] - set environment variable",  cmd_setenv },
	{ "saveenv",  "write environment to storage",               cmd_saveenv },
	{ "slot",     "show A/B boot state",                        cmd_slot },
	{ "boot",     "scan file systems and boot",                 cmd_boot },
	{ "part",     "list block devices",                         cmd_part },
	{ "pstore",   "[console|dump|zap] - ramoops crash logs",    cmd_pstore },
	{ "ls",       "<device> - list files on a device",          cmd_ls },
	{ "md",       "<hex-addr> [count] - display memory",        cmd_md },
#ifdef LK2ND_UMS
	{ "ums",      "[device] - USB mass storage mode",           cmd_ums },
#endif
	{ "reset",    "reboot the device",                          cmd_reset },
	{ "reboot",   "[bootloader|recovery|edl] - reboot",         cmd_reboot },
	{ "poweroff", "shut the device down",                       cmd_poweroff },
	{ NULL, NULL, NULL },
};

/**
 * lk2nd_shell() - Interactive serial command prompt.
 */
void lk2nd_shell(void)
{
	char line[SHELL_LINE_LEN];
	char *argv[SHELL_MAX_ARGS];
	const struct shell_cmd *cmd;
	int argc;

	debug_uart_suppress = 0;

	printf("\nlk2nd %s - type 'help' for commands\n", LK2ND_VERSION);

	for (;;) {
		printf("lk2nd> ");
		shell_readline(line, sizeof(line));

		argc = shell_tokenize(line, argv);
		if (argc == 0)
			continue;

		for (cmd = shell_cmds; cmd->name; cmd++)
			if (!strcmp(cmd->name, argv[0]))
				break;

		if (cmd->name)
			cmd->fn(argc, argv);
		else
			printf("Unknown command '%s' - try 'help'\n", argv[0]);
	}
}
