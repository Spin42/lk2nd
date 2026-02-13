// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2023 Nikita Travkin <nikita@trvn.ru> */

#include <compiler.h>
#include <config.h>
#include <debug.h>
#include <dev/fbcon.h>
#include <display_menu.h>
#include <kernel/thread.h>
#include <platform.h>
#include <platform/timer.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdbool.h>

#include <lk2nd/device/keys.h>
#include <lk2nd/device/menu.h>
#include <lk2nd/util/minmax.h>
#include <lk2nd/version.h>

#include "../device.h"

// Defined in app/aboot/aboot.c
extern void cmd_continue(const char *arg, void *data, unsigned sz);
extern int dgetc(char *c, bool wait);

/* Configuration */
#define FONT_WIDTH	(5+1)
#define FONT_HEIGHT	12
#define MIN_LINE	40

enum fbcon_colors {
	WHITE = FBCON_TITLE_MSG,
	SILVER = FBCON_SUBTITLE_MSG,
	YELLOW = FBCON_YELLOW_MSG,
	ORANGE = FBCON_ORANGE_MSG,
	RED = FBCON_RED_MSG,
	GREEN = FBCON_GREEN_MSG,
	WHITE_ON_BLUE = FBCON_SELECT_MSG_BG_COLOR,
};

static int scale_factor = 0;

/* Forward declaration for serial console menu (local to this file) */
static void display_serial_menu(void);

/*
 * Direct serial I/O helpers.
 *
 * These bypass the debug_uart_suppress flag so the menu can draw
 * while background dprintf output (fastboot thread, USB init, etc.)
 * is suppressed.  Defined in platform/msm_shared/debug.c.
 */
extern void _serial_putc(char c);
extern volatile int debug_uart_suppress;

static void serial_puts(const char *s)
{
	while (*s)
		_serial_putc(*s++);
}

static void serial_printf(const char *fmt, ...) __PRINTFLIKE(1, 2);
static void serial_printf(const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	serial_puts(buf);
}

/*
 * VT100 escape helpers for in-place terminal updates.
 *
 * These work on any terminal emulator (minicom, picocom, screen, PuTTY,
 * the Linux console, etc.).  They emit raw escape sequences directly to
 * the UART so background dprintf output cannot interleave.
 */

/* Save / restore cursor position */
#define VT_SAVE_CURSOR()      serial_puts("\033[s")
#define VT_RESTORE_CURSOR()   serial_puts("\033[u")

/* Move cursor to row,col (1-based) */
#define VT_GOTO(row, col)     serial_printf("\033[%d;%dH", (row), (col))

/* Clear from cursor to end of line */
#define VT_CLEAR_EOL()        serial_puts("\033[K")

/* Clear entire screen and home cursor */
#define VT_CLEAR_SCREEN()     serial_puts("\033[2J\033[H")

/* Hide / show cursor */
#define VT_HIDE_CURSOR()      serial_puts("\033[?25l")
#define VT_SHOW_CURSOR()      serial_puts("\033[?25h")

static void fbcon_puts(const char *str, unsigned type, int y, bool center)
{
	struct fbcon_config *fb = fbcon_display();
	int line_len = fb->width;
	int text_len = strlen(str) * FONT_WIDTH * scale_factor;
	int x = 0;

	if (center)
		x = (line_len - min(text_len, line_len)) / 2;

	while (*str != 0) {
		fbcon_putc_factor_xy(*str++, type, scale_factor, x, y);
		x += FONT_WIDTH * scale_factor;
		if (x >= line_len)
			return;
	}
}

static __PRINTFLIKE(4, 5) void fbcon_printf(unsigned type, int y, bool center, const char *fmt, ...)
{
	char buf[MIN_LINE * 3];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fbcon_puts(buf, type, y, center);
}

static const uint16_t published_keys[] = {
	KEY_VOLUMEUP,
	KEY_VOLUMEDOWN,
	KEY_POWER,
	KEY_HOME,
};

/**
 * lk2nd_boot_pressed_key() - Get the pressed key, if any.
 */
static uint16_t lk2nd_boot_pressed_key(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(published_keys); ++i)
		if (lk2nd_keys_pressed(published_keys[i]))
			return published_keys[i];

	return 0;
}

#define LONG_PRESS_DURATION 1000

static uint16_t wait_key(void)
{
	uint16_t keycode = 0;
	int press_start = 0;
	int press_duration;

	while (!(keycode = lk2nd_boot_pressed_key()))
		thread_sleep(1);

	press_start = current_time();

	while (lk2nd_keys_pressed(keycode)) {
		thread_sleep(1);

		press_duration = current_time() - press_start;
		if (lk2nd_dev.single_key && press_duration > LONG_PRESS_DURATION)
			return KEY_POWER;
	}

	if (lk2nd_dev.single_key)
		keycode = KEY_VOLUMEDOWN;

	/* A small debounce delay */
	thread_sleep(5);

	return keycode;
}

#define xstr(s) str(s)
#define str(s) #s

static void opt_continue(void)   { cmd_continue(NULL, NULL, 0); }
static void opt_reboot(void)     { reboot_device(0); }
static void opt_recovery(void)
{
	extern unsigned boot_into_recovery;

	boot_into_recovery = 1;
	cmd_continue(NULL, NULL, 0);
}
static void opt_bootloader(void) { reboot_device(FASTBOOT_MODE); }
static void opt_edl(void)        { reboot_device(EMERGENCY_DLOAD); }
static void opt_shutdown(void)   { shutdown_device(); }

#ifdef LK2ND_UMS
extern int ums_enter_mode(const char *partition_name);
static void opt_ums(void)
{
	/* Re-enable background output for UMS status messages */
	debug_uart_suppress = 0;

	dprintf(INFO, "Entering USB Mass Storage mode (partition='%s')\n",
		xstr(LK2ND_UMS_PARTITION));
	/* Note: xstr() stringifies the token, so LK2ND_UMS_PARTITION=userdata
	 * becomes the string literal "userdata". The makefile must NOT wrap
	 * the value in quotes or they will be embedded in the string. */

	/* Ensure block devices are initialized before UMS attempts a mount */
	extern void lk2nd_bdev_init(void);
	static bool bdev_init_done;
	if (!bdev_init_done) {
		lk2nd_bdev_init();
		bdev_init_done = true;
	}

	int ret = ums_enter_mode(xstr(LK2ND_UMS_PARTITION));
	if (ret == 0) {
		dprintf(INFO, "UMS mode ended, rebooting\n");
		reboot_device(0);
	} else {
		dprintf(CRITICAL, "UMS mode failed (ret=%d)\n", ret);
		thread_sleep(2000);
	}
}
#endif

static struct {
	const char *name;
	unsigned color;
	void (*action)(void);
} menu_options[] = {
	{ "Reboot",      GREEN,  opt_reboot },
	{ "Continue",    WHITE,  opt_continue },
	{ "Recovery",    ORANGE, opt_recovery },
	{ "Bootloader",  ORANGE, opt_bootloader },
#ifdef LK2ND_UMS
	{ "USB Storage", YELLOW, opt_ums },
#endif
	{ "EDL",         RED,    opt_edl },
	{ "Shutdown",    RED,    opt_shutdown },
};

#define NUM_OPTIONS  ARRAY_SIZE(menu_options)

#define fbcon_printf_ln(color, y, incr, x...) \
	do { \
		fbcon_printf(color, y, x); \
		y += incr; \
	} while(0)

#define fbcon_puts_ln(color, y, incr, center, str) \
	do { \
		fbcon_puts(str, color, y, center); \
		y += incr; \
	} while(0)

void display_fastboot_menu(void)
{
	struct fbcon_config *fb = fbcon_display();
	int y, y_menu, old_scale, incr;
	unsigned int sel = 0, i;
	bool armv8 = is_scm_armv8_support();

	/* Prefer serial console menu if explicitly enabled or if no framebuffer */
#ifdef LK2ND_SERIAL_MENU
	display_serial_menu();
	return;
#endif
	if (!fb) {
		display_serial_menu();
		return;
	}

	/*
	 * Make sure the specified line lenght fits on the screen.
	 */
	scale_factor = max(1U, min(fb->width, fb->height) / (FONT_WIDTH * MIN_LINE));
	old_scale = scale_factor;
	incr = FONT_HEIGHT * scale_factor;

	y = incr * 2;

	fbcon_clear();

	/*
	 * Draw the static part of the menu
	 */

	scale_factor += 1;
	incr = FONT_HEIGHT * scale_factor;
	fbcon_puts_ln(WHITE, y, incr, true, xstr(BOARD));

	scale_factor = old_scale;
		fbcon_puts_ln(RED, y, incr, true, "Unknown (FIXME!)");
	y += incr;
	fbcon_puts_ln(RED, y, incr, true, "Fastboot mode");
	y += incr;

	/* Skip lines for the menu */
	y_menu = y;
	y += incr * (ARRAY_SIZE(menu_options) + 1);

	if (lk2nd_dev.single_key) {
		fbcon_puts_ln(SILVER, y, incr, true, "Short press to navigate.");
		fbcon_puts_ln(SILVER, y, incr, true, "Long press to select.");
	} else {
		fbcon_printf_ln(SILVER, y, incr, true, "%s to navigate.",
				(lk2nd_dev.menu_keys.navigate ? lk2nd_dev.menu_keys.navigate : "Volume keys"));
		fbcon_printf_ln(SILVER, y, incr, true, "%s to select.",
				(lk2nd_dev.menu_keys.select ? lk2nd_dev.menu_keys.select : "Power key"));
	}

	/*
	 * Draw the device-specific information at the bottom of the screen
	 */

	scale_factor = max(1, scale_factor - 1);
	incr = FONT_HEIGHT * scale_factor;
	y = fb->height - 8 * incr;

	fbcon_puts_ln(WHITE, y, incr, true, "About this device");


	if (lk2nd_dev.panel.name)
		fbcon_printf_ln(SILVER, y, incr, false, " Panel:  %s", lk2nd_dev.panel.name);
	if (lk2nd_dev.battery)
		fbcon_printf_ln(SILVER, y, incr, false, " Battery:  %s", lk2nd_dev.battery);
#if WITH_LK2ND_DEVICE_2ND
	if (lk2nd_dev.bootloader)
		fbcon_printf_ln(SILVER, y, incr, false, " Bootloader:  %s", lk2nd_dev.bootloader);
#endif

	fbcon_printf_ln(armv8 ? GREEN : YELLOW, y, incr, false, " ARM64:  %s",
			armv8 ? "available" : "unavailable");

	/*
	 * Loop to render the menu elements
	 */

	scale_factor = old_scale;
	incr = FONT_HEIGHT * scale_factor;
	while (true) {
		y = y_menu;
		fbcon_clear_msg(y / FONT_HEIGHT, (y / FONT_HEIGHT + ARRAY_SIZE(menu_options) * scale_factor));
		for (i = 0; i < ARRAY_SIZE(menu_options); ++i) {
			fbcon_printf_ln(
				i == sel ? menu_options[i].color : SILVER,
				y, incr, true, "%c %s %c",
				i == sel ? '>' : ' ',
				menu_options[i].name,
				i == sel ? '<' : ' '
			);
		}

		fbcon_flush();

		switch (wait_key()) {
		case KEY_POWER:
			y = y_menu + incr * sel;
			fbcon_printf_ln(
				menu_options[sel].color,
				y, incr, true, ">> %s <<",
				menu_options[sel].name
			);
			menu_options[sel].action();
			break;
		case KEY_VOLUMEUP:
			if (sel == 0)
				sel = ARRAY_SIZE(menu_options) - 1;
			else
				sel--;
			break;
		case KEY_VOLUMEDOWN:
			sel++;
			if (sel >= ARRAY_SIZE(menu_options))
				sel = 0;
			break;
		}
	}
}

void display_default_image_on_screen(void);
void display_default_image_on_screen(void)
{
	struct fbcon_config *fb = fbcon_display();
	int y, incr;

	/*
	 * Make sure the specified line lenght fits on the screen.
	 */
	scale_factor = max(1U, min(fb->width, fb->height) / (FONT_WIDTH * MIN_LINE));
	incr = FONT_HEIGHT * scale_factor;
	y = fb->height - 3 * incr;

	fbcon_clear_msg(y / FONT_HEIGHT, y / FONT_HEIGHT + 3 * scale_factor);

	fbcon_puts_ln(WHITE, y, incr, true, xstr(BOARD));
	fbcon_puts_ln(SILVER, y, incr, true, LK2ND_VERSION);
	fbcon_flush();
}

/*
 * ====================================================================
 * Serial console menu
 *
 * Uses VT100 escape codes for in-place updates so that:
 *   - The header is drawn once and never reprinted
 *   - Only the changed menu lines are redrawn on navigation
 *   - Background dprintf output is suppressed while the menu is active
 *   - Arrow keys (ESC [ A / ESC [ B) work alongside u/d/number keys
 * ====================================================================
 */

/*
 * Row layout (1-based, matching VT100 addressing):
 *
 *   1  ---- header separator
 *   2  title
 *   3  ---- separator
 *   4  version
 *   5  device
 *   6  ARM64
 *   7  (optional: panel / battery / bootloader)
 *   .. ---- separator
 *   .. (blank)
 *   .. menu item 1          <- menu_start_row
 *   .. menu item 2
 *   ..   ...
 *   .. menu item N
 *   .. (blank)
 *   .. ---- separator
 *   .. help text
 *   .. ---- separator
 *
 * The actual row numbers are computed dynamically so the header can
 * include a variable number of device-info lines.
 */

/* First row where menu items start; set during header draw */
static int menu_start_row;

/**
 * serial_draw_header() - Draw the static header (once).
 *
 * Returns the next available row after the header.
 */
static int serial_draw_header(void)
{
	bool armv8 = is_scm_armv8_support();
	int row = 1;

	VT_CLEAR_SCREEN();
	VT_HIDE_CURSOR();

	VT_GOTO(row, 1);
	serial_puts("----------------------------------------------");
	row++;

	VT_GOTO(row, 1);
	serial_printf("  lk2nd Boot Menu  [%s]", xstr(BOARD));
	row++;

	VT_GOTO(row, 1);
	serial_puts("----------------------------------------------");
	row++;

	VT_GOTO(row, 1);
	serial_printf("  Version : %s", LK2ND_VERSION);
	row++;

	VT_GOTO(row, 1);
	if (lk2nd_dev.model)
		serial_printf("  Device  : %s", lk2nd_dev.model);
	else
		serial_puts("  Device  : Unknown");
	row++;

	VT_GOTO(row, 1);
	serial_printf("  ARM64   : %s", armv8 ? "yes" : "no");
	row++;

	if (lk2nd_dev.panel.name) {
		VT_GOTO(row, 1);
		serial_printf("  Panel   : %s", lk2nd_dev.panel.name);
		row++;
	}
	if (lk2nd_dev.battery) {
		VT_GOTO(row, 1);
		serial_printf("  Battery : %s", lk2nd_dev.battery);
		row++;
	}
#if WITH_LK2ND_DEVICE_2ND
	if (lk2nd_dev.bootloader) {
		VT_GOTO(row, 1);
		serial_printf("  Loader  : %s", lk2nd_dev.bootloader);
		row++;
	}
#endif

	VT_GOTO(row, 1);
	serial_puts("----------------------------------------------");
	row++;

	/* Blank line before menu */
	row++;

	return row;
}

/**
 * serial_draw_option() - Draw a single menu option at its row.
 */
static void serial_draw_option(unsigned int idx, bool selected)
{
	VT_GOTO(menu_start_row + (int)idx, 1);
	VT_CLEAR_EOL();

	if (selected)
		serial_printf("  > %d. %s", idx + 1, menu_options[idx].name);
	else
		serial_printf("    %d. %s", idx + 1, menu_options[idx].name);
}

/**
 * serial_draw_all_options() - Redraw every menu option.
 */
static void serial_draw_all_options(unsigned int sel)
{
	unsigned int i;

	for (i = 0; i < NUM_OPTIONS; i++)
		serial_draw_option(i, i == sel);
}

/**
 * serial_draw_footer() - Draw the help text below the menu.
 */
static void serial_draw_footer(void)
{
	int row = menu_start_row + (int)NUM_OPTIONS + 1;

	VT_GOTO(row, 1);
	serial_puts("----------------------------------------------");
	row++;
	VT_GOTO(row, 1);
	serial_puts("  Arrows/u/d: navigate   Enter: select");
	row++;
	VT_GOTO(row, 1);
	serial_puts("  1-9: jump to option    q: quit");
	row++;
	VT_GOTO(row, 1);
	serial_puts("----------------------------------------------");
}

/**
 * serial_draw_status() - Show a transient status message below the footer.
 */
static void serial_draw_status(const char *msg)
{
	int row = menu_start_row + (int)NUM_OPTIONS + 6;

	VT_GOTO(row, 1);
	VT_CLEAR_EOL();
	if (msg)
		serial_printf("  %s", msg);
}

/**
 * serial_getc_blocking() - Wait for a character from the serial port.
 *
 * Polls dgetc() with a short sleep between attempts so we don't
 * busy-wait.
 */
static char serial_getc_blocking(void)
{
	char c;

	while (dgetc(&c, false) != 0)
		thread_sleep(10);
	return c;
}

/**
 * display_serial_menu() - Interactive serial console boot menu.
 *
 * Suppresses background dprintf output while drawing and only
 * updates the lines that actually change.
 */
static void display_serial_menu(void)
{
	unsigned int sel = 0;
	unsigned int old_sel;

	/*
	 * Suppress background noise (fastboot thread, USB init messages,
	 * etc.) while the menu is being displayed.
	 */
	debug_uart_suppress = 1;

	/* Draw the complete screen once */
	menu_start_row = serial_draw_header();
	serial_draw_all_options(sel);
	serial_draw_footer();
	serial_draw_status(NULL);

	for (;;) {
		char c = serial_getc_blocking();

		old_sel = sel;

		switch (c) {
		case '\033': {
			/*
			 * VT100 arrow keys send ESC [ A (up) or ESC [ B (down).
			 * Read the next two bytes with a short timeout -- if they
			 * don't arrive quickly this was a bare ESC (ignore it).
			 */
			char seq1, seq2;
			int timeout;

			/* Wait up to ~50 ms for the '[' */
			for (timeout = 0; timeout < 5; timeout++) {
				if (dgetc(&seq1, false) == 0)
					break;
				thread_sleep(10);
			}
			if (timeout >= 5)
				break; /* bare ESC */
			if (seq1 != '[')
				break;

			/* Wait up to ~50 ms for the letter */
			for (timeout = 0; timeout < 5; timeout++) {
				if (dgetc(&seq2, false) == 0)
					break;
				thread_sleep(10);
			}
			if (timeout >= 5)
				break;

			if (seq2 == 'A') /* up arrow */
				goto nav_up;
			if (seq2 == 'B') /* down arrow */
				goto nav_down;
			break;
		}

		case 'u':
		case 'U':
		case 'k': /* vi-style */
nav_up:
			if (sel == 0)
				sel = NUM_OPTIONS - 1;
			else
				sel--;
			break;

		case 'd':
		case 'D':
		case 'j': /* vi-style */
nav_down:
			sel++;
			if (sel >= NUM_OPTIONS)
				sel = 0;
			break;

		case '\r':
		case '\n':
			serial_draw_status(NULL);
			serial_draw_option(sel, true);

			/* Unsuppress output before running the action */
			VT_SHOW_CURSOR();
			debug_uart_suppress = 0;

			dprintf(INFO, "Menu: executing '%s'\n",
				menu_options[sel].name);
			menu_options[sel].action();

			/*
			 * If the action returns (e.g. UMS exit), re-suppress
			 * and redraw.
			 */
			debug_uart_suppress = 1;
			menu_start_row = serial_draw_header();
			serial_draw_all_options(sel);
			serial_draw_footer();
			serial_draw_status("Returned from action");
			continue;

		case 'q':
		case 'Q':
			/* Restore terminal state and unsuppress */
			VT_SHOW_CURSOR();
			serial_draw_status(NULL);
			debug_uart_suppress = 0;
			dprintf(INFO, "Menu: exiting\n");
			return;

		case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9': {
			unsigned int choice = (unsigned int)(c - '1');
			if (choice < NUM_OPTIONS) {
				sel = choice;

				/* Redraw the changed lines */
				if (old_sel != sel) {
					serial_draw_option(old_sel, false);
					serial_draw_option(sel, true);
				}

				serial_draw_status(NULL);

				/* Unsuppress and run */
				VT_SHOW_CURSOR();
				debug_uart_suppress = 0;

				dprintf(INFO, "Menu: executing '%s'\n",
					menu_options[sel].name);
				menu_options[sel].action();

				debug_uart_suppress = 1;
				menu_start_row = serial_draw_header();
				serial_draw_all_options(sel);
				serial_draw_footer();
				serial_draw_status("Returned from action");
			} else {
				serial_draw_status("Invalid option");
			}
			continue;
		}

		default:
			/* Ignore unknown input silently */
			continue;
		}

		/* Only update the two lines that changed */
		if (old_sel != sel) {
			serial_draw_option(old_sel, false);
			serial_draw_option(sel, true);
			serial_draw_status(NULL);
		}
	}
}

/**
 * boot_menu_countdown_check() - Display boot countdown and wait for keypress
 *
 * Displays a countdown timer on serial console and waits for user input.
 * If any key is pressed during the countdown, returns 1 to indicate the user
 * wants to enter the fastboot menu. Otherwise returns 0 to continue normal boot.
 *
 * Uses \r to overwrite the countdown in-place.
 *
 * Return: 1 if key pressed (enter menu), 0 if timeout (normal boot)
 */
int boot_menu_countdown_check(void)
{
	int countdown = LK2ND_MENU_TIMEOUT;
	char c;
	bool triggered = false;

	dprintf(ALWAYS, "\n=== lk2nd Boot Menu ===\n");
	dprintf(ALWAYS, "Press any key within %d seconds to enter fastboot menu\n",
		countdown);

	/* Drain any buffered input first */
	while (dgetc(&c, false) == 0) { /* drain */ }

	while (countdown > 0 && !triggered) {
		dprintf(ALWAYS, "\rBooting in %2d ...  ", countdown);

		/* Wait 1 second while checking for keypress every 50ms */
		for (int i = 0; i < 20; i++) {
			if (dgetc(&c, false) == 0) {
				dprintf(ALWAYS, "\rKey pressed -- entering fastboot menu\n");
				triggered = true;
				break;
			}
			thread_sleep(50);
		}

		if (!triggered)
			countdown--;
	}

	if (triggered)
		return 1;

	dprintf(ALWAYS, "\rNo key pressed -- continuing normal boot   \n\n");
	return 0;
}
