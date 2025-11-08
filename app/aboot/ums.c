/*
 * Copyright (c) 2024, lk2nd UMS implementation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <debug.h>
#include <sys/types.h>
#include <list.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <platform.h>
#include <target.h>
#include <kernel/thread.h>
#include <kernel/event.h>
#include <kernel/timer.h>
#include <dev/udc.h>
#include <arch/ops.h>
#include <arch/defines.h>
#ifdef USB30_SUPPORT
#include <usb30_udc.h>
#endif
#include <lib/bio.h>
#include <lib/partition.h>
#include <platform/timer.h>
#include <stdbool.h>
#include "ums.h"

/* Fallback for CACHE_LINE if not defined */
#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

/* Global UMS device state */
static struct ums_device g_ums_device = {0};
static struct udc_endpoint *ums_endpoints[2];
static struct udc_request *ums_req_in = NULL;
static struct udc_request *ums_req_out = NULL;
static event_t ums_online;
static event_t ums_txn_done;
static bool ums_active = false;

/* Static CBW buffer - MUST NOT be on stack as USB DMA accesses it asynchronously */
static struct cbw ums_cbw_buffer __attribute__((aligned(CACHE_LINE)));

/* Controller abstraction (hsusb vs dwc), modeled after fastboot */
typedef struct {
    int (*udc_init)(struct udc_device *devinfo);
    int (*udc_register_gadget)(struct udc_gadget *gadget);
    int (*udc_start)(void);
    int (*udc_stop)(void);

    struct udc_endpoint *(*udc_endpoint_alloc)(unsigned type, unsigned maxpkt);
    void (*udc_endpoint_free)(struct udc_endpoint *ept);
    struct udc_request *(*udc_request_alloc)(void);
    void (*udc_request_free)(struct udc_request *req);
    int (*udc_request_queue)(struct udc_endpoint *ept, struct udc_request *req);
} ums_usb_if_t;

static ums_usb_if_t usb_if;

/* USB device and gadget descriptors */
static struct udc_device ums_udc_device = {
    .vendor_id    = 0x1d6b,  /* Linux Foundation */
    .product_id   = 0x0104,  /* Multifunction Composite Gadget */
    .version_id   = 0x0100,
    .manufacturer = "lk2nd",
    .product      = "Mass Storage",
};

static struct udc_gadget ums_gadget = {
    .notify        = ums_notify,
    .ifc_class     = UMS_CLASS,
    .ifc_subclass  = UMS_SUBCLASS,
    .ifc_protocol  = UMS_PROTOCOL,
    .ifc_endpoints = 2,
    .ifc_string    = "Mass Storage",
    .ept           = ums_endpoints,
};

/* Request completion callback */
static void ums_req_complete(struct udc_request *req, unsigned actual, int status)
{
    dprintf(ALWAYS, "[UMS] ums_req_complete ENTRY: req=%p actual=%u status=%d\n",
            req, actual, status);

    if (!req) {
        dprintf(CRITICAL, "[UMS] ums_req_complete: NULL request pointer!\n");
        return;
    }

    dprintf(ALWAYS, "[UMS] ums_req_complete: Setting req->length = %u\n", actual);
    req->length = actual;

    dprintf(ALWAYS, "[UMS] ums_req_complete: Signaling event\n");
    event_signal(&ums_txn_done, 0);

    dprintf(ALWAYS, "[UMS] ums_req_complete EXIT\n");
}

/* USB gadget event notification */
static void ums_notify(struct udc_gadget *gadget, unsigned event)
{
    dprintf(SPEW, "ums_notify: event %d\n", event);

    switch (event) {
    case UDC_EVENT_ONLINE:
        dprintf(INFO, "UMS: USB connected\n");
        event_signal(&ums_online, 0);
        break;
    case UDC_EVENT_OFFLINE:
        dprintf(INFO, "UMS: USB disconnected\n");
        break;
    }
}

/* Set SCSI sense data */
static void ums_set_sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
    g_ums_device.sense_key = key;
    g_ums_device.asc = asc;
    g_ums_device.ascq = ascq;
}

/* Send Command Status Wrapper */
static void ums_send_csw(uint32_t tag, uint32_t residue, uint8_t status)
{
    struct csw csw;

    csw.signature = CSW_SIGNATURE;
    csw.tag = tag;
    csw.data_residue = residue;
    csw.status = status;

    ums_req_in->buf = &csw;
    ums_req_in->length = sizeof(csw);
    ums_req_in->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[0], ums_req_in);
    event_wait(&ums_txn_done);
}

/* SCSI TEST UNIT READY command */
static int ums_scsi_test_unit_ready(struct cbw *cbw)
{
    dprintf(SPEW, "UMS: TEST UNIT READY\n");

    if (g_ums_device.is_mounted) {
        ums_set_sense(SCSI_SENSE_NO_SENSE, 0, 0);
        return 0;
    } else {
        ums_set_sense(SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }
}

/* SCSI REQUEST SENSE command */
static int ums_scsi_request_sense(struct cbw *cbw)
{
    uint8_t sense_data[18] = {0};

    dprintf(SPEW, "UMS: REQUEST SENSE\n");

    sense_data[0] = 0x70;  /* Response Code */
    sense_data[2] = g_ums_device.sense_key;
    sense_data[7] = 10;    /* Additional Sense Length */
    sense_data[12] = g_ums_device.asc;
    sense_data[13] = g_ums_device.ascq;

    /* Send sense data */
    ums_req_in->buf = sense_data;
    ums_req_in->length = MIN(cbw->data_transfer_length, sizeof(sense_data));
    ums_req_in->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[0], ums_req_in);
    event_wait(&ums_txn_done);

    /* Clear sense after reporting */
    ums_set_sense(SCSI_SENSE_NO_SENSE, 0, 0);

    return 0;
}

/* SCSI INQUIRY command */
static int ums_scsi_inquiry(struct cbw *cbw)
{
    struct scsi_inquiry_data inquiry = {0};

    dprintf(SPEW, "UMS: INQUIRY\n");

    inquiry.peripheral_device_type = 0;  /* Direct access block device */
    inquiry.peripheral_qualifier = 0;
    inquiry.rmb = 1;  /* Removable medium */
    inquiry.version = 4;  /* SPC-2 */
    inquiry.response_data_format = 2;
    inquiry.additional_length = sizeof(inquiry) - 5;

    memcpy(inquiry.vendor_id, "lk2nd   ", 8);
    memcpy(inquiry.product_id, "Mass Storage    ", 16);
    memcpy(inquiry.product_revision, "1.0 ", 4);

    /* Send inquiry data */
    ums_req_in->buf = &inquiry;
    ums_req_in->length = MIN(cbw->data_transfer_length, sizeof(inquiry));
    ums_req_in->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[0], ums_req_in);
    event_wait(&ums_txn_done);

    return 0;
}

/* SCSI READ CAPACITY command */
static int ums_scsi_read_capacity(struct cbw *cbw)
{
    struct scsi_read_capacity_data capacity = {0};

    dprintf(SPEW, "UMS: READ CAPACITY\n");

    if (!g_ums_device.is_mounted) {
        ums_set_sense(SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    /* Convert to big-endian */
    capacity.last_logical_block = __builtin_bswap32(g_ums_device.block_count - 1);
    capacity.logical_block_length = __builtin_bswap32(g_ums_device.block_size);

    /* Send capacity data */
    ums_req_in->buf = &capacity;
    ums_req_in->length = MIN(cbw->data_transfer_length, sizeof(capacity));
    ums_req_in->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[0], ums_req_in);
    event_wait(&ums_txn_done);

    return 0;
}

/* SCSI READ 10 command */
static int ums_scsi_read_10(struct cbw *cbw)
{
    uint32_t lba, transfer_length;
    uint64_t offset;
    int ret;

    if (!g_ums_device.is_mounted || !g_ums_device.bio_dev) {
        ums_set_sense(SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    /* Extract LBA and transfer length from CDB */
    lba = (cbw->cb[2] << 24) | (cbw->cb[3] << 16) | (cbw->cb[4] << 8) | cbw->cb[5];
    transfer_length = (cbw->cb[7] << 8) | cbw->cb[8];

    dprintf(SPEW, "UMS: READ 10 - LBA %u, length %u\n", lba, transfer_length);

    /* Validate bounds */
    if (lba >= g_ums_device.block_count || (lba + transfer_length) > g_ums_device.block_count) {
        ums_set_sense(SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
        return -1;
    }

    offset = (uint64_t)lba * g_ums_device.block_size;

    /* Read data from partition */
    ret = bio_read(g_ums_device.bio_dev, g_ums_device.transfer_buffer,
                   offset, transfer_length * g_ums_device.block_size);
    if (ret < 0) {
        dprintf(CRITICAL, "UMS: bio_read failed: %d\n", ret);
        ums_set_sense(SCSI_SENSE_MEDIUM_ERROR, 0, 0);
        return -1;
    }

    /* Send data to host */
    ums_req_in->buf = g_ums_device.transfer_buffer;
    ums_req_in->length = transfer_length * g_ums_device.block_size;
    ums_req_in->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[0], ums_req_in);
    event_wait(&ums_txn_done);

    return 0;
}

/* SCSI WRITE 10 command */
static int ums_scsi_write_10(struct cbw *cbw)
{
    uint32_t lba, transfer_length;
    uint64_t offset;
    int ret;

    if (!g_ums_device.is_mounted || !g_ums_device.bio_dev) {
        ums_set_sense(SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    if (g_ums_device.is_read_only) {
        ums_set_sense(SCSI_SENSE_ILLEGAL_REQUEST, 0x27, 0);  /* Write protected */
        return -1;
    }

    /* Extract LBA and transfer length from CDB */
    lba = (cbw->cb[2] << 24) | (cbw->cb[3] << 16) | (cbw->cb[4] << 8) | cbw->cb[5];
    transfer_length = (cbw->cb[7] << 8) | cbw->cb[8];

    dprintf(SPEW, "UMS: WRITE 10 - LBA %u, length %u\n", lba, transfer_length);

    /* Validate bounds */
    if (lba >= g_ums_device.block_count || (lba + transfer_length) > g_ums_device.block_count) {
        ums_set_sense(SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
        return -1;
    }

    /* Receive data from host */
    ums_req_out->buf = g_ums_device.transfer_buffer;
    ums_req_out->length = transfer_length * g_ums_device.block_size;
    ums_req_out->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[1], ums_req_out);
    event_wait(&ums_txn_done);

    /* Invalidate cache to ensure CPU reads fresh data written by USB DMA */
    arch_invalidate_cache_range((addr_t)g_ums_device.transfer_buffer,
                                ROUNDUP(transfer_length * g_ums_device.block_size, CACHE_LINE));

    offset = (uint64_t)lba * g_ums_device.block_size;

    /* Write data to partition */
    ret = bio_write(g_ums_device.bio_dev, g_ums_device.transfer_buffer,
                    offset, transfer_length * g_ums_device.block_size);
    if (ret < 0) {
        dprintf(CRITICAL, "UMS: bio_write failed: %d\n", ret);
        ums_set_sense(SCSI_SENSE_MEDIUM_ERROR, 0, 0);
        return -1;
    }

    return 0;
}

/* SCSI MODE SENSE 6 command */
static int ums_scsi_mode_sense_6(struct cbw *cbw)
{
    uint8_t mode_data[4] = {0};

    dprintf(SPEW, "UMS: MODE SENSE 6\n");

    mode_data[0] = 3;  /* Mode data length */
    mode_data[1] = 0;  /* Medium type */
    mode_data[2] = g_ums_device.is_read_only ? 0x80 : 0x00;  /* Device-specific parameter */
    mode_data[3] = 0;  /* Block descriptor length */

    /* Send mode data */
    ums_req_in->buf = mode_data;
    ums_req_in->length = MIN(cbw->data_transfer_length, sizeof(mode_data));
    ums_req_in->complete = ums_req_complete;

    usb_if.udc_request_queue(ums_endpoints[0], ums_req_in);
    event_wait(&ums_txn_done);

    return 0;
}

/* Handle SCSI command */
static int ums_handle_scsi_command(struct cbw *cbw)
{
    int ret = 0;

    switch (cbw->cb[0]) {
    case SCSI_TEST_UNIT_READY:
        ret = ums_scsi_test_unit_ready(cbw);
        break;
    case SCSI_REQUEST_SENSE:
        ret = ums_scsi_request_sense(cbw);
        break;
    case SCSI_INQUIRY:
        ret = ums_scsi_inquiry(cbw);
        break;
    case SCSI_READ_CAPACITY:
        ret = ums_scsi_read_capacity(cbw);
        break;
    case SCSI_READ_10:
        ret = ums_scsi_read_10(cbw);
        break;
    case SCSI_WRITE_10:
        ret = ums_scsi_write_10(cbw);
        break;
    case SCSI_MODE_SENSE_6:
        ret = ums_scsi_mode_sense_6(cbw);
        break;
    case SCSI_START_STOP_UNIT:
    case SCSI_ALLOW_MEDIUM_REMOVAL:
    case SCSI_VERIFY_10:
        /* These commands are ignored but return success */
        dprintf(SPEW, "UMS: Ignoring SCSI command 0x%02x\n", cbw->cb[0]);
        ret = 0;
        break;
    default:
        dprintf(SPEW, "UMS: Unsupported SCSI command 0x%02x\n", cbw->cb[0]);
        ums_set_sense(SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_COMMAND, 0);
        ret = -1;
        break;
    }

    return ret;
}

/* Handle Command Block Wrapper */
static int ums_handle_cbw(struct cbw *cbw)
{
    int ret;
    uint32_t residue = 0;
    uint8_t status = CSW_STATUS_GOOD;

    dprintf(ALWAYS, "[UMS] ums_handle_cbw ENTRY: cbw=%p\n", cbw);

    /* Validate CBW signature */
    if (cbw->signature != CBW_SIGNATURE) {
        /* Some hosts may send a zero-length packet or residual bytes during reset; just stall */
        dprintf(CRITICAL, "UMS: Invalid CBW signature: 0x%08x (expect %08x)\n", cbw->signature, CBW_SIGNATURE);
        return -1;
    }

    dprintf(ALWAYS, "UMS: CBW tag=0x%08x, length=%u, flags=0x%02x, lun=%u, cb_length=%u, SCSI_CMD=0x%02x\n",
            cbw->tag, cbw->data_transfer_length, cbw->flags, cbw->lun, cbw->cb_length, cbw->cb[0]);

    /* Handle SCSI command */
    dprintf(ALWAYS, "[UMS] Calling ums_handle_scsi_command for SCSI 0x%02x\n", cbw->cb[0]);
    ret = ums_handle_scsi_command(cbw);
    dprintf(ALWAYS, "[UMS] ums_handle_scsi_command returned %d\n", ret);

    if (ret < 0) {
        status = CSW_STATUS_FAILED;
        residue = cbw->data_transfer_length;
    }

    /* Send Command Status Wrapper */
    dprintf(ALWAYS, "[UMS] Sending CSW: tag=0x%08x residue=%u status=%u\n", cbw->tag, residue, status);
    ums_send_csw(cbw->tag, residue, status);
    dprintf(ALWAYS, "[UMS] ums_handle_cbw EXIT\n");

    return ret;
}

/* UMS main thread */
static int ums_thread(void *arg)
{
    dprintf(ALWAYS, "UMS: Starting mass storage mode for partition '%s'\n",
            g_ums_device.partition_name);

    dprintf(ALWAYS, "UMS: Waiting for USB ONLINE event (connect cable/host)\n");

    /* Wait for USB connection */
    event_wait(&ums_online);

    dprintf(ALWAYS, "UMS: USB ONLINE event received\n");

    /* Give the host time to enumerate and send SET_CONFIGURATION */
    dprintf(ALWAYS, "UMS: Waiting for host enumeration to complete...\n");
    thread_sleep(500);

    dprintf(ALWAYS, "UMS: Entering CBW loop\n");
    dprintf(ALWAYS, "UMS: Sanity check - usb_if.udc_request_queue=%p ums_endpoints[1]=%p ums_req_out=%p\n",
            usb_if.udc_request_queue, ums_endpoints[1], ums_req_out);
    dprintf(ALWAYS, "UMS: ums_req_complete callback address = %p\n", ums_req_complete);

    /* Verify function pointers are in valid memory range (around 0x0f9xxxxx) */
    uintptr_t func_addr = (uintptr_t)usb_if.udc_request_queue;
    if (func_addr < 0x0f900000 || func_addr > 0x0fa00000) {
        dprintf(CRITICAL, "UMS: INVALID udc_request_queue address %p!\n", usb_if.udc_request_queue);
        return -1;
    }    while (ums_active) {
        /* Clear the CBW buffer before receiving new data */
        memset(&ums_cbw_buffer, 0, sizeof(ums_cbw_buffer));

        /* Receive CBW - using static buffer instead of stack to avoid DMA corruption */
        ums_req_out->buf = &ums_cbw_buffer;
        ums_req_out->length = sizeof(ums_cbw_buffer);
        ums_req_out->complete = ums_req_complete;

        /* Safety check before calling */
        if (!usb_if.udc_request_queue || !ums_endpoints[1] || !ums_req_out) {
            dprintf(CRITICAL, "UMS: NULL pointer detected before queue! udc_request_queue=%p ep=%p req=%p\n",
                    usb_if.udc_request_queue, ums_endpoints[1], ums_req_out);
            break;
        }

        dprintf(ALWAYS, "UMS: About to queue CBW receive:\n");
        dprintf(ALWAYS, "  ums_req_out=%p\n", ums_req_out);
        dprintf(ALWAYS, "  ums_req_out->buf=%p\n", ums_req_out->buf);
        dprintf(ALWAYS, "  ums_req_out->length=%u\n", ums_req_out->length);
        dprintf(ALWAYS, "  ums_req_out->complete=%p (expected %p)\n",
                ums_req_out->complete, ums_req_complete);

        /* Verify the callback hasn't been corrupted */
        if (ums_req_out->complete != ums_req_complete) {
            dprintf(CRITICAL, "UMS: CORRUPTION! complete callback changed from %p to %p!\n",
                    ums_req_complete, ums_req_out->complete);
            /* Restore it */
            ums_req_out->complete = ums_req_complete;
        }

        dprintf(ALWAYS, "UMS: Calling udc_request_queue at %p...\n", usb_if.udc_request_queue);
        int ret = usb_if.udc_request_queue(ums_endpoints[1], ums_req_out);
        dprintf(ALWAYS, "UMS: udc_request_queue returned %d\n", ret);

        if (ret != 0) {
            dprintf(CRITICAL, "UMS: udc_request_queue failed with ret=%d\n", ret);
            break;
        }

        event_wait(&ums_txn_done);

        /* Invalidate cache to ensure CPU reads fresh CBW data written by USB DMA */
        arch_invalidate_cache_range((addr_t)&ums_cbw_buffer, ROUNDUP(sizeof(ums_cbw_buffer), CACHE_LINE));

        if (ums_req_out->length == sizeof(ums_cbw_buffer)) {
            ums_handle_cbw(&ums_cbw_buffer);
        }
    }

    dprintf(INFO, "UMS: Mass storage mode ended\n");
    return 0;
}

/* Mount partition for UMS access */
int ums_mount_partition(const char *partition_name)
{
    bdev_t *dev;
    const char *mapped_name = NULL;

    if (!partition_name) {
        dprintf(CRITICAL, "UMS: Invalid partition name\n");
        return -1;
    }

    dprintf(ALWAYS, "UMS: Trying to open partition '%s' for export\n", partition_name);

    /* Open block device for partition by name; if it fails, try label mapping */
    dev = bio_open(partition_name);
    if (!dev) {
        /* Walk device list to find a label match */
        struct bdev_struct *bds = bio_get_bdevs();
        if (bds) {
            mutex_acquire(&bds->lock);
            struct list_node *ln;
            list_for_every(&bds->list, ln) {
                bdev_t *entry = (bdev_t *)ln; /* node is first field in bdev_t */
                if (entry->label && !strcmp(entry->label, partition_name)) {
                    mapped_name = entry->name;
                    break;
                }
            }
            mutex_release(&bds->lock);
        }
        if (mapped_name) {
            dprintf(ALWAYS, "UMS: Label '%s' resolved to device '%s'\n", partition_name, mapped_name);
            dev = bio_open(mapped_name);
        }
    }
    if (!dev) {
        dprintf(CRITICAL, "UMS: Failed to open partition '%s' (no device or label match)\n", partition_name);
        dprintf(ALWAYS, "UMS: Available devices (name -> label):\n");
        struct bdev_struct *bds = bio_get_bdevs();
        if (bds) {
            mutex_acquire(&bds->lock);
            struct list_node *ln;
            list_for_every(&bds->list, ln) {
                bdev_t *entry = (bdev_t *)ln; /* node is first field in bdev_t */
                dprintf(ALWAYS, "  %s -> %s\n", entry->name, entry->label ? entry->label : "(none)");
            }
            mutex_release(&bds->lock);
        }
        return -1;
    }

    g_ums_device.bio_dev = dev;
    g_ums_device.block_count = dev->block_count;
    g_ums_device.block_size = dev->block_size;
    strlcpy(g_ums_device.partition_name, mapped_name ? mapped_name : partition_name, sizeof(g_ums_device.partition_name));
    g_ums_device.is_mounted = true;
    g_ums_device.is_read_only = false;  /* Allow writes by default */

    dprintf(ALWAYS, "UMS: Mounted partition '%s' - %llu blocks of %u bytes\n",
            partition_name, g_ums_device.block_count, g_ums_device.block_size);

    return 0;
}

/* Unmount partition */
void ums_unmount_partition(void)
{
    if (g_ums_device.bio_dev) {
        bio_close(g_ums_device.bio_dev);
        g_ums_device.bio_dev = NULL;
    }

    g_ums_device.is_mounted = false;
    memset(g_ums_device.partition_name, 0, sizeof(g_ums_device.partition_name));

    dprintf(INFO, "UMS: Partition unmounted\n");
}

/* Initialize UMS */
int ums_init(void)
{
    /* Allocate transfer buffer */
    g_ums_device.transfer_buffer = memalign(4096, UMS_BUFFER_SIZE);
    if (!g_ums_device.transfer_buffer) {
        dprintf(CRITICAL, "UMS: Failed to allocate transfer buffer\n");
        return -1;
    }
    dprintf(ALWAYS, "UMS: Transfer buffer @%p size=%u\n", g_ums_device.transfer_buffer, UMS_BUFFER_SIZE);

    /* Initialize events */
    event_init(&ums_online, false, EVENT_FLAG_AUTOUNSIGNAL);
    event_init(&ums_txn_done, false, EVENT_FLAG_AUTOUNSIGNAL);

    /* Select controller implementation (dwc vs hsusb), mirroring fastboot */
    dprintf(ALWAYS, "UMS: Selecting USB controller: %s\n", target_usb_controller());
    if (!strcmp(target_usb_controller(), "dwc")) {
#ifdef USB30_SUPPORT
        ums_udc_device.t_usb_if = target_usb30_init();
        usb_if.udc_init            = usb30_udc_init;
        usb_if.udc_register_gadget = usb30_udc_register_gadget;
        usb_if.udc_start           = usb30_udc_start;
        usb_if.udc_stop            = usb30_udc_stop;
    usb_if.udc_endpoint_alloc  = usb30_udc_endpoint_alloc;
    /* No usb30_udc_endpoint_free in header; leave free as NULL */
    usb_if.udc_endpoint_free   = NULL;
        usb_if.udc_request_alloc   = usb30_udc_request_alloc;
        usb_if.udc_request_free    = usb30_udc_request_free;
        usb_if.udc_request_queue   = usb30_udc_request_queue;
#else
        dprintf(CRITICAL, "UMS: USB30_SUPPORT not enabled for DWC target\n");
        return -1;
#endif
    } else {
        usb_if.udc_init            = udc_init;
        usb_if.udc_register_gadget = udc_register_gadget;
        usb_if.udc_start           = udc_start;
        usb_if.udc_stop            = udc_stop;
        usb_if.udc_endpoint_alloc  = udc_endpoint_alloc;
        usb_if.udc_endpoint_free   = udc_endpoint_free;
        usb_if.udc_request_alloc   = udc_request_alloc;
        usb_if.udc_request_free    = udc_request_free;
        usb_if.udc_request_queue   = udc_request_queue;
    }

    /* Initialize UDC */
    dprintf(ALWAYS, "UMS: Initializing UDC (vid=0x%04x pid=0x%04x)\n", ums_udc_device.vendor_id, ums_udc_device.product_id);
    int ret;
    dprintf(ALWAYS, "UMS: Calling udc_init() via iface...\n");
    ret = usb_if.udc_init(&ums_udc_device);
    dprintf(ALWAYS, "UMS: udc_init(iface) returned %d\n", ret);
    if (ret) {
        dprintf(CRITICAL, "UMS: Failed to initialize UDC (ret=%d)\n", ret);
        return -1;
    }
    dprintf(ALWAYS, "UMS: UDC initialized (controller='%s')\n", target_usb_controller());

    /* Allocate endpoints - MUST happen after udc_init() for HSUSB */
    dprintf(ALWAYS, "UMS: Allocating endpoints (BULK IN/OUT) post-udc_init before gadget registration)\n");
    ums_endpoints[0] = usb_if.udc_endpoint_alloc(UDC_TYPE_BULK_IN, 512);
    ums_endpoints[1] = usb_if.udc_endpoint_alloc(UDC_TYPE_BULK_OUT, 512);
    if (!ums_endpoints[0] || !ums_endpoints[1]) {
        dprintf(CRITICAL, "UMS: Failed to allocate endpoints\n");
        return -1;
    }
    dprintf(ALWAYS, "UMS: Endpoint[IN]=%p Endpoint[OUT]=%p\n", ums_endpoints[0], ums_endpoints[1]);

    /* Allocate requests */
    dprintf(ALWAYS, "UMS: Allocating requests\n");
    ums_req_in = usb_if.udc_request_alloc();
    ums_req_out = usb_if.udc_request_alloc();
    if (!ums_req_in || !ums_req_out) {
        dprintf(CRITICAL, "UMS: Failed to allocate requests\n");
        return -1;
    }
    dprintf(ALWAYS, "UMS: Requests allocated in=%p out=%p\n", ums_req_in, ums_req_out);

    /* Register gadget AFTER endpoints are valid so descriptor fill works */
    dprintf(ALWAYS, "UMS: Registering UMS gadget (with endpoints)\n");
    dprintf(ALWAYS, "UMS: Calling udc_register_gadget()...\n");
    ret = usb_if.udc_register_gadget(&ums_gadget);
    dprintf(ALWAYS, "UMS: udc_register_gadget() returned %d\n", ret);
    if (ret) {
        dprintf(CRITICAL, "UMS: Failed to register gadget (ret=%d)\n", ret);
        return -1;
    }
    dprintf(ALWAYS, "UMS: Gadget registered; endpoints=%d\n", ums_gadget.ifc_endpoints);

    dprintf(INFO, "UMS: Initialized successfully\n");
    return 0;
}

/* Enter UMS mode */
int ums_enter_mode(const char *partition_name)
{
    thread_t *thr;
    int ret;

    if (ums_active) {
        dprintf(ALWAYS, "UMS: Already active (early return)\n");
        return 0;
    }

    dprintf(ALWAYS, "UMS: Enter mode requested for partition '%s' (enter)\n", partition_name ? partition_name : "(null)");
    dprintf(ALWAYS, "UMS: Sanity check -> ums_active=%d transfer_buffer=%p\n", ums_active, g_ums_device.transfer_buffer);

    /* Initialize UMS */
    if (ums_init()) {
        dprintf(CRITICAL, "UMS: Initialization failed\n");
        return -1;
    }

    /* Mount partition with retry in case block devices not yet published */
    int mount_attempts = 0;
    const int max_attempts = 30; /* ~3s total (100ms sleep) */
    while (mount_attempts < max_attempts) {
        if (ums_mount_partition(partition_name) == 0)
            break;
        if (mount_attempts == 0)
            dprintf(INFO, "UMS: Initial mount failed, waiting for block devices to appear...\n");
        mount_attempts++;
        thread_sleep(100);
    }
    if (!g_ums_device.is_mounted) {
        dprintf(CRITICAL, "UMS: Failed to mount partition after %d attempts\n", mount_attempts);
        return -1;
    }
    if (mount_attempts)
        dprintf(INFO, "UMS: Partition mounted after %d retry attempts\n", mount_attempts);

    /* Start USB */
    dprintf(ALWAYS, "UMS: Starting UDC (connecting to host)\n");
    dprintf(ALWAYS, "UMS: Calling udc_start()...\n");
    ret = usb_if.udc_start();
    dprintf(ALWAYS, "UMS: udc_start() returned %d\n", ret);
    if (ret) {
        dprintf(CRITICAL, "UMS: Failed to start UDC (ret=%d)\n", ret);
        return -1;
    }
    dprintf(ALWAYS, "UMS: UDC started; waiting for host enumeration (ONLINE event)\n");

    ums_active = true;

    /* Start UMS thread */
    dprintf(ALWAYS, "UMS: Creating worker thread\n");
    thr = thread_create("ums", &ums_thread, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    if (!thr) {
        dprintf(CRITICAL, "UMS: Failed to create thread\n");
    ums_active = false;
        return -1;
    }
    thread_resume(thr);
    dprintf(ALWAYS, "UMS: Worker thread started\n");

    dprintf(INFO, "UMS: Mass storage mode active. Connect USB cable.\n");
    dprintf(INFO, "UMS: Press 'q' to exit...\n");

    /* Wait for explicit quit key ('q') to exit; ignore any other chars to avoid spurious exit */
    /* Drain any buffered input first to avoid immediately consuming stale chars */
    {
        char tmp;
        while (dgetc(&tmp, false) == 0) { /* drain */ }
        thread_sleep(50);
    }
    char c;
    while (ums_active) {
        if (dgetc(&c, false) == 0) {
            if (c == 'q' || c == 'Q') {
                dprintf(INFO, "UMS: Quit key '%c' pressed, exiting mass storage mode\n", c);
                break;
            } else {
                dprintf(SPEW, "UMS: Ignoring non-quit key '%c'\n", c);
            }
        }
        thread_sleep(100);
    }

    ums_exit_mode();
    return 0;
}

/* Exit UMS mode */
void ums_exit_mode(void)
{
    if (!ums_active) {
        return;
    }

    dprintf(INFO, "UMS: Exiting mass storage mode\n");

    ums_active = false;

    /* Stop USB */
    usb_if.udc_stop();

    /* Unmount partition */
    ums_unmount_partition();

    /* Free resources */
    if (g_ums_device.transfer_buffer) {
        free(g_ums_device.transfer_buffer);
        g_ums_device.transfer_buffer = NULL;
    }

    if (ums_req_in) {
        usb_if.udc_request_free(ums_req_in);
        ums_req_in = NULL;
    }

    if (ums_req_out) {
        usb_if.udc_request_free(ums_req_out);
        ums_req_out = NULL;
    }

    if (ums_endpoints[0] && usb_if.udc_endpoint_free) {
        usb_if.udc_endpoint_free(ums_endpoints[0]);
        ums_endpoints[0] = NULL;
    }

    if (ums_endpoints[1] && usb_if.udc_endpoint_free) {
        usb_if.udc_endpoint_free(ums_endpoints[1]);
        ums_endpoints[1] = NULL;
    }

    memset(&g_ums_device, 0, sizeof(g_ums_device));

    dprintf(INFO, "UMS: Cleanup complete\n");
}

/* Countdown check for UMS entry */
int ums_countdown_check(void)
{
    int countdown = UMS_COUNTDOWN_SECONDS;
    char c;
    bool triggered = false;

    dprintf(ALWAYS, "\n");
    dprintf(ALWAYS, "=== lk2nd Boot Menu Countdown ===\n");
    dprintf(ALWAYS, "Press SPACE (or any key) within %d seconds to open the fastboot menu.\n", countdown);
    dprintf(ALWAYS, "(Pressing other keys spams but only first key matters.)\n\n");

    while (countdown > 0 && !triggered) {
        dprintf(ALWAYS, "Opening menu in %d seconds... ", countdown);
        uint64_t second_start = current_time_hires();
        while ((current_time_hires() - second_start) < 1000000) {
            if (dgetc(&c, false) == 0) {
                dprintf(ALWAYS, "\nKey '%c' pressed -> showing menu...\n", (c >= 32 && c <= 126) ? c : '?');
                triggered = true;
                break;
            }
            thread_sleep(10);
        }
        if (!triggered) {
            dprintf(ALWAYS, "\r");
            countdown--;
        }
    }

    if (triggered)
        return 1;

    dprintf(ALWAYS, "\nNo key pressed, continuing normal boot...\n\n");
    return 0;
}
