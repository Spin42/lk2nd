// SPDX-License-Identifier: BSD-3-Clause
/*
 * USB serial console for the lk2nd shell.
 *
 * Exposes a vendor-specific USB interface with two bulk endpoints and
 * bridges it into the debug console: lk2nd_usbcon_putc()/getc() are
 * called from _dputc()/dgetc(), so everything the countdown and the
 * shell print (and read) also flows over USB.
 *
 * On the host, bind the generic USB serial driver once:
 *
 *   modprobe usbserial vendor=0x1d6b product=0x0104
 *
 * and open /dev/ttyUSB0 with minicom/picocom. Interrupting the boot
 * countdown from there drops into the shell, no UART wires needed.
 *
 * All I/O is non-blocking: output is buffered in a ring and flushed
 * whenever the host is reading; if no terminal is attached, output is
 * simply dropped and boot is never delayed.
 */

#include <debug.h>
#include <string.h>
#include <stdlib.h>
#include <platform.h>
#include <target.h>
#include <kernel/event.h>
#include <kernel/thread.h>
#include <dev/udc.h>
#include <arch/ops.h>
#include <arch/defines.h>
#include <stdbool.h>

#include <lk2nd/device/menu.h>

#define USBCON_TX_RING  1024
#define USBCON_TX_CHUNK 512
#define USBCON_RX_CHUNK 64

static struct udc_endpoint *usbcon_endpoints[2];
static struct udc_request *usbcon_req_in;
static struct udc_request *usbcon_req_out;

static bool usbcon_up;
static volatile bool usbcon_online;

/* TX: ring filled by putc, drained into a DMA buffer one chunk at a time */
static char tx_ring[USBCON_TX_RING];
static unsigned int tx_head, tx_tail; /* head = write pos, tail = read pos */
static uint8_t tx_dma[USBCON_TX_CHUNK] __attribute__((aligned(CACHE_LINE)));
static volatile bool tx_pending;

/* RX: one chunk in flight, drained by getc */
static uint8_t rx_dma[USBCON_RX_CHUNK] __attribute__((aligned(CACHE_LINE)));
static volatile bool rx_queued;
static volatile bool rx_ready;
static volatile unsigned int rx_len;
static unsigned int rx_pos;

static struct udc_device usbcon_udc_device = {
	.vendor_id    = 0x1d6b,  /* Linux Foundation */
	.product_id   = 0x0104,  /* Multifunction Composite Gadget */
	.version_id   = 0x0100,
	.manufacturer = "lk2nd",
	.product      = "Serial Console",
};

static void usbcon_notify(struct udc_gadget *gadget, unsigned event)
{
	if (event == UDC_EVENT_ONLINE)
		usbcon_online = true;
	else if (event == UDC_EVENT_OFFLINE)
		usbcon_online = false;
}

static struct udc_gadget usbcon_gadget = {
	.notify        = usbcon_notify,
	.ifc_class     = 0xff, /* vendor specific */
	.ifc_subclass  = 0xff,
	.ifc_protocol  = 0xff,
	.ifc_endpoints = 2,
	.ifc_string    = "serial",
	.ept           = usbcon_endpoints,
};

static void usbcon_tx_complete(struct udc_request *req, unsigned actual, int status)
{
	tx_pending = false;
}

static void usbcon_rx_complete(struct udc_request *req, unsigned actual, int status)
{
	rx_len = (status == 0) ? actual : 0;
	rx_ready = true;
}

/* Push out the next TX chunk if the ring has data and the ep is free */
static void usbcon_tx_flush(void)
{
	unsigned int len = 0;

	if (!usbcon_up || !usbcon_online || tx_pending)
		return;

	while (tx_tail != tx_head && len < USBCON_TX_CHUNK) {
		tx_dma[len++] = tx_ring[tx_tail];
		tx_tail = (tx_tail + 1) % USBCON_TX_RING;
	}
	if (!len)
		return;

	arch_clean_invalidate_cache_range((addr_t)tx_dma,
					  ROUNDUP(len, CACHE_LINE));

	usbcon_req_in->buf = (void *)PA((addr_t)tx_dma);
	usbcon_req_in->length = len;
	usbcon_req_in->complete = usbcon_tx_complete;

	tx_pending = true;
	if (udc_request_queue(usbcon_endpoints[0], usbcon_req_in) < 0)
		tx_pending = false;
}

/**
 * lk2nd_usbcon_putc() - Mirror a console character to the USB host.
 *
 * Non-blocking: characters are dropped if the ring is full (e.g. no
 * terminal attached on the host side).
 */
void lk2nd_usbcon_putc(char c)
{
	unsigned int next;

	if (!usbcon_up)
		return;

	if (c == '\n')
		lk2nd_usbcon_putc('\r');

	next = (tx_head + 1) % USBCON_TX_RING;
	if (next != tx_tail) { /* drop when full */
		tx_ring[tx_head] = c;
		tx_head = next;
	}

	if (c == '\n' || ((tx_head - tx_tail) % USBCON_TX_RING) >= USBCON_TX_CHUNK)
		usbcon_tx_flush();
}

/**
 * lk2nd_usbcon_getc() - Fetch a character sent by the USB host.
 *
 * Non-blocking; returns 0 and stores the character, or -1 if none is
 * pending. Also drives the receive state machine.
 */
int lk2nd_usbcon_getc(char *c)
{
	if (!usbcon_up || !usbcon_online)
		return -1;

	/* Flush any buffered output while we are polled */
	usbcon_tx_flush();

	if (rx_ready) {
		if (rx_pos == 0)
			arch_invalidate_cache_range((addr_t)rx_dma,
						    ROUNDUP(sizeof(rx_dma), CACHE_LINE));
		if (rx_pos < rx_len) {
			*c = rx_dma[rx_pos++];
			return 0;
		}
		/* Buffer consumed, allow a new transfer */
		rx_ready = false;
		rx_queued = false;
		rx_pos = 0;
	}

	if (!rx_queued) {
		usbcon_req_out->buf = (void *)PA((addr_t)rx_dma);
		usbcon_req_out->length = sizeof(rx_dma);
		usbcon_req_out->complete = usbcon_rx_complete;

		rx_queued = true;
		if (udc_request_queue(usbcon_endpoints[1], usbcon_req_out) < 0)
			rx_queued = false;
	}

	return -1;
}

/**
 * lk2nd_usbcon_start() - Bring up the USB serial console gadget.
 */
int lk2nd_usbcon_start(void)
{
	if (usbcon_up)
		return 0;

	/* Only the HSUSB (chipidea) controller is supported for now */
	if (!strcmp(target_usb_controller(), "dwc"))
		return -1;

	if (udc_init(&usbcon_udc_device) < 0) {
		dprintf(CRITICAL, "usbcon: udc_init failed\n");
		return -1;
	}

	usbcon_endpoints[0] = udc_endpoint_alloc(UDC_TYPE_BULK_IN, 512);
	usbcon_endpoints[1] = udc_endpoint_alloc(UDC_TYPE_BULK_OUT, 512);
	if (!usbcon_endpoints[0] || !usbcon_endpoints[1])
		return -1;

	usbcon_req_in = udc_request_alloc();
	usbcon_req_out = udc_request_alloc();
	if (!usbcon_req_in || !usbcon_req_out)
		return -1;

	if (udc_register_gadget(&usbcon_gadget) < 0)
		return -1;

	if (udc_start() < 0)
		return -1;

	tx_head = tx_tail = 0;
	tx_pending = false;
	rx_queued = rx_ready = false;
	rx_pos = 0;
	usbcon_up = true;

	return 0;
}

/**
 * lk2nd_usbcon_stop() - Tear the gadget down (e.g. before fastboot
 * takes over the controller or before booting).
 */
void lk2nd_usbcon_stop(void)
{
	if (!usbcon_up)
		return;

	usbcon_up = false;
	usbcon_online = false;

	udc_stop();

	if (usbcon_req_in) {
		udc_request_free(usbcon_req_in);
		usbcon_req_in = NULL;
	}
	if (usbcon_req_out) {
		udc_request_free(usbcon_req_out);
		usbcon_req_out = NULL;
	}
	if (usbcon_endpoints[0]) {
		udc_endpoint_free(usbcon_endpoints[0]);
		usbcon_endpoints[0] = NULL;
	}
	if (usbcon_endpoints[1]) {
		udc_endpoint_free(usbcon_endpoints[1]);
		usbcon_endpoints[1] = NULL;
	}
}
