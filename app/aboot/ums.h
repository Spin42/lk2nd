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

#ifndef __APP_UMS_H
#define __APP_UMS_H

#include <stdint.h>
#include <lib/bio.h>

/* Forward declarations */
struct udc_gadget;
struct cbw;

/* USB Mass Storage Class definitions */
#define UMS_CLASS               0x08
#define UMS_SUBCLASS            0x06    /* SCSI transparent command set */
#define UMS_PROTOCOL            0x50    /* Bulk-Only Transport */

/* Bulk-Only Transport (BOT) definitions */
#define CBW_SIGNATURE           0x43425355  /* "USBC" */
#define CSW_SIGNATURE           0x53425355  /* "USBS" */

#define CBW_FLAG_DATA_OUT       0x00
#define CBW_FLAG_DATA_IN        0x80

#define CSW_STATUS_GOOD         0x00
#define CSW_STATUS_FAILED       0x01
#define CSW_STATUS_PHASE_ERROR  0x02

/* SCSI commands */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_FORMAT_UNIT        0x04
#define SCSI_INQUIRY            0x12
#define SCSI_START_STOP_UNIT    0x1B
#define SCSI_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_READ_FORMAT_CAPACITIES 0x23
#define SCSI_READ_CAPACITY      0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_VERIFY_10          0x2F
#define SCSI_MODE_SELECT_6      0x15
#define SCSI_MODE_SENSE_6       0x1A
#define SCSI_MODE_SELECT_10     0x55
#define SCSI_MODE_SENSE_10      0x5A

/* SCSI sense keys */
#define SCSI_SENSE_NO_SENSE     0x00
#define SCSI_SENSE_NOT_READY    0x02
#define SCSI_SENSE_MEDIUM_ERROR 0x03
#define SCSI_SENSE_ILLEGAL_REQUEST 0x05
#define SCSI_SENSE_UNIT_ATTENTION  0x06

/* Additional Sense Codes */
#define SCSI_ASC_INVALID_COMMAND    0x20
#define SCSI_ASC_INVALID_FIELD_IN_CDB 0x24
#define SCSI_ASC_MEDIUM_NOT_PRESENT 0x3A

/* Configuration */
#define UMS_MAX_PARTITION_NAME  32

/*
 * Transfer buffer sizing.
 * UMS uses a portion of the scratch region (same area fastboot uses).
 * The default is 1 MiB; the actual size is capped at half the scratch
 * region so that other subsystems still have room.
 */
#define UMS_BUFFER_SIZE_DEFAULT (1 * 1024 * 1024)

/*
 * USB controller transfer limits (must match HSUSB / DWC drivers).
 * HSUSB: 2 TDs x 16 KiB = 32 KiB per udc_request_queue() call.
 * DWC:   single TRB can do ~16 MiB.
 */
#define UMS_HSUSB_MAX_XFER      (32 * 1024)
#define UMS_DWC_MAX_XFER         (16 * 1024 * 1024)

/* Command Block Wrapper (CBW) */
struct cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
} __attribute__((packed));

/* Command Status Wrapper (CSW) */
struct csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;
} __attribute__((packed));

/* SCSI Standard Inquiry Data */
struct scsi_inquiry_data {
    uint8_t peripheral_device_type:5;
    uint8_t peripheral_qualifier:3;
    uint8_t reserved1:7;
    uint8_t rmb:1;
    uint8_t version;
    uint8_t response_data_format:4;
    uint8_t hisup:1;
    uint8_t normaca:1;
    uint8_t obsolete1:2;
    uint8_t additional_length;
    uint8_t protect:1;
    uint8_t reserved2:2;
    uint8_t third_party_copy:1;
    uint8_t tpgs:2;
    uint8_t acc:1;
    uint8_t sccs:1;
    uint8_t addr16:1;
    uint8_t reserved3:2;
    uint8_t multi_port:1;
    uint8_t vs1:1;
    uint8_t enc_serv:1;
    uint8_t bque:1;
    uint8_t vs2:1;
    uint8_t cmd_que:1;
    uint8_t reserved4:1;
    uint8_t linked:1;
    uint8_t sync:1;
    uint8_t wbus16:1;
    uint8_t obsolete2:2;
    char vendor_id[8];
    char product_id[16];
    char product_revision[4];
} __attribute__((packed));

/* SCSI Read Capacity Data */
struct scsi_read_capacity_data {
    uint32_t last_logical_block;
    uint32_t logical_block_length;
} __attribute__((packed));

/* UMS device state */
struct ums_device {
    bdev_t *bio_dev;
    uint64_t block_count;
    uint32_t block_size;
    char partition_name[UMS_MAX_PARTITION_NAME];
    bool is_mounted;
    bool is_read_only;

    /* Transfer state */
    void *transfer_buffer;
    uint32_t transfer_length;
    uint32_t transfer_offset;
    bool transfer_in_progress;

    /* SCSI state */
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;
};

/* Public API */
int ums_enter_mode(const char *partition_name);
void ums_exit_mode(void);
int ums_init(void);
int ums_mount_partition(const char *partition_name);
void ums_unmount_partition(void);

#endif /* __APP_UMS_H */
