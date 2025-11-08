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
    if (!req) {
        dprintf(CRITICAL, "UMS: NULL request in completion callback!\n");
        return;
    }

    req->length = actual;
    event_signal(&ums_txn_done, 0);
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

    /* Validate CBW signature */
    if (cbw->signature != CBW_SIGNATURE) {
        dprintf(CRITICAL, "UMS: Invalid CBW signature: 0x%08x (expected 0x%08x)\n",
                cbw->signature, CBW_SIGNATURE);
        return -1;
    }

    dprintf(SPEW, "UMS: CBW tag=0x%08x, SCSI=0x%02x, length=%u\n",
            cbw->tag, cbw->cb[0], cbw->data_transfer_length);

    /* Handle SCSI command */
    ret = ums_handle_scsi_command(cbw);

    if (ret < 0) {
        status = CSW_STATUS_FAILED;
        residue = cbw->data_transfer_length;
    }

    /* Send Command Status Wrapper */
    ums_send_csw(cbw->tag, residue, status);

    return ret;
}

/* UMS main thread */
static int ums_thread(void *arg)
{
    dprintf(ALWAYS, "UMS: Starting mass storage mode for partition '%s'\n",
            g_ums_device.partition_name);

    dprintf(INFO, "UMS: Waiting for USB connection...\n");

    /* Wait for USB connection */
    event_wait(&ums_online);

    dprintf(INFO, "UMS: USB connected, waiting for enumeration\n");

    /* Give the host time to enumerate and send SET_CONFIGURATION */
    thread_sleep(500);

    dprintf(INFO, "UMS: Ready - processing SCSI commands\n");

    while (ums_active) {
        /* Clear the CBW buffer before receiving new data */
        memset(&ums_cbw_buffer, 0, sizeof(ums_cbw_buffer));

        /* Receive CBW - using static buffer instead of stack to avoid DMA corruption */
        ums_req_out->buf = &ums_cbw_buffer;
        ums_req_out->length = sizeof(ums_cbw_buffer);
        ums_req_out->complete = ums_req_complete;

        /* Queue request to receive next command */
        int ret = usb_if.udc_request_queue(ums_endpoints[1], ums_req_out);
        if (ret != 0) {
            dprintf(CRITICAL, "UMS: Failed to queue CBW request: %d\n", ret);
            break;
        }

        /* Wait for command to be received */
        event_wait(&ums_txn_done);

        /* Invalidate cache to ensure CPU reads fresh CBW data written by USB DMA */
        arch_invalidate_cache_range((addr_t)&ums_cbw_buffer, ROUNDUP(sizeof(ums_cbw_buffer), CACHE_LINE));

        /* Process the command if we got a full CBW */
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
            dprintf(INFO, "UMS: Resolved label '%s' to device '%s'\n", partition_name, mapped_name);
            dev = bio_open(mapped_name);
        }
    }
    if (!dev) {
        dprintf(CRITICAL, "UMS: Failed to open partition '%s'\n", partition_name);
        dprintf(INFO, "UMS: Available devices:\n");
        struct bdev_struct *bds = bio_get_bdevs();
        if (bds) {
            mutex_acquire(&bds->lock);
            struct list_node *ln;
            list_for_every(&bds->list, ln) {
                bdev_t *entry = (bdev_t *)ln;
                dprintf(INFO, "  %s -> %s\n", entry->name, entry->label ? entry->label : "(none)");
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
    g_ums_device.is_read_only = false;

    dprintf(INFO, "UMS: Mounted '%s' (%llu blocks x %u bytes)\n",
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

    /* Initialize events */
    event_init(&ums_online, false, EVENT_FLAG_AUTOUNSIGNAL);
    event_init(&ums_txn_done, false, EVENT_FLAG_AUTOUNSIGNAL);

    /* Select controller implementation (dwc vs hsusb) */
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
    dprintf(INFO, "UMS: Initializing USB controller (%s)\n", target_usb_controller());
    int ret = usb_if.udc_init(&ums_udc_device);
    if (ret) {
        dprintf(CRITICAL, "UMS: Failed to initialize UDC: %d\n", ret);
        return -1;
    }

    /* Allocate endpoints - MUST happen after udc_init() */
    ums_endpoints[0] = usb_if.udc_endpoint_alloc(UDC_TYPE_BULK_IN, 512);
    ums_endpoints[1] = usb_if.udc_endpoint_alloc(UDC_TYPE_BULK_OUT, 512);
    if (!ums_endpoints[0] || !ums_endpoints[1]) {
        dprintf(CRITICAL, "UMS: Failed to allocate endpoints\n");
        return -1;
    }

    /* Allocate requests */
    ums_req_in = usb_if.udc_request_alloc();
    ums_req_out = usb_if.udc_request_alloc();
    if (!ums_req_in || !ums_req_out) {
        dprintf(CRITICAL, "UMS: Failed to allocate requests\n");
        return -1;
    }

    /* Register gadget */
    ret = usb_if.udc_register_gadget(&ums_gadget);
    if (ret) {
        dprintf(CRITICAL, "UMS: Failed to register gadget: %d\n", ret);
        return -1;
    }

    dprintf(INFO, "UMS: Initialized successfully\n");
    return 0;
}

/* Enter UMS mode */
int ums_enter_mode(const char *partition_name)
{
    thread_t *thr;
    int ret;

    if (ums_active) {
        dprintf(INFO, "UMS: Already active\n");
        return 0;
    }

    dprintf(INFO, "UMS: Starting mass storage mode for partition '%s'\n",
            partition_name ? partition_name : "(null)");

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
            dprintf(INFO, "UMS: Waiting for block devices...\n");
        mount_attempts++;
        thread_sleep(100);
    }
    if (!g_ums_device.is_mounted) {
        dprintf(CRITICAL, "UMS: Failed to mount partition\n");
        return -1;
    }

    /* Start USB */
    dprintf(INFO, "UMS: Starting USB device\n");
    ret = usb_if.udc_start();
    if (ret) {
        dprintf(CRITICAL, "UMS: Failed to start USB: %d\n", ret);
        return -1;
    }

    ums_active = true;

    /* Start UMS thread */
    thr = thread_create("ums", &ums_thread, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    if (!thr) {
        dprintf(CRITICAL, "UMS: Failed to create thread\n");
        ums_active = false;
        return -1;
    }
    thread_resume(thr);

    dprintf(INFO, "UMS: Mass storage mode active\n");
    dprintf(INFO, "UMS: Connect USB cable to host\n");
    dprintf(INFO, "UMS: Press 'q' to exit\n");

    /* Wait for quit key */
    char tmp, c;
    /* Drain buffered input */
    while (dgetc(&tmp, false) == 0) { /* drain */ }
    thread_sleep(50);

    while (ums_active) {
        if (dgetc(&c, false) == 0) {
            if (c == 'q' || c == 'Q') {
                dprintf(INFO, "UMS: Exiting mass storage mode\n");
                break;
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
