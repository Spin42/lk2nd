/*
 * Copyright (c) 2009 Travis Geiselbrecht
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
#include <printf.h>
#include <string.h>
#include <compiler.h>
#include <stdlib.h>
#include <arch.h>
#include <crc32.h>
#include <lib/bio.h>
#include <lib/partition.h>
#include <partition_parser.h>

struct chs {
	uint8_t c;
	uint8_t h;
	uint8_t s;
} __PACKED;

struct mbr_part {
	uint8_t status;
	struct chs start;
	uint8_t type;
	struct chs end;
	uint32_t lba_start;
	uint32_t lba_length;
} __PACKED;

#define GPT_MAX_PUBLISH 16
#define MBR_TYPE_GPT_PROTECTIVE 0xee

/*
 * Extract the partition name the same way the eMMC GPT parser does
 * (mmc_boot_read_gpt): names are UTF-16LE, only english is supported
 * so the 2nd byte of each character is dropped.
 */
static char *gpt_entry_name(const unsigned char *utf16_name)
{
	char *name = malloc(MAX_GPT_NAME_SIZE / 2 + 1);
	unsigned int n;

	if (!name)
		return NULL;

	for (n = 0; n < MAX_GPT_NAME_SIZE / 2; n++)
		name[n] = utf16_name[n * 2];
	name[MAX_GPT_NAME_SIZE / 2] = '\0';

	return name;
}

/*
 * Look for a GPT on the device and publish its partitions.
 *
 * This is a bio-device version of the boot-eMMC-only GPT parser in
 * platform/msm_shared/partition_parser.c and reuses its layout macros;
 * the header is validated like partition_parse_gpt_header() does.
 *
 * Returns the number of published partitions, or a negative value
 * when no valid GPT was found.
 */
static int publish_gpt(bdev_t *dev, const char *device, off_t offset)
{
	unsigned long long first_usable_lba, entries_lba, first_lba, last_lba;
	unsigned int header_size, max_partition_count, partition_entry_size;
	unsigned int part_entry_cnt;
	uint32_t crc_val, crc_val_org;
	unsigned int i, count = 0;
	int err;

	STACKBUF_DMA_ALIGN(data, dev->block_size);

	err = bio_read(dev, data, offset + GPT_LBA * dev->block_size,
		       dev->block_size);
	if (err != (int)dev->block_size)
		return (err < 0) ? err : -1;

	/* Check GPT Signature */
	if (((uint32_t *)data)[0] != GPT_SIGNATURE_2 ||
	    ((uint32_t *)data)[1] != GPT_SIGNATURE_1)
		return -1;

	header_size = GET_LWORD_FROM_BYTE(&data[HEADER_SIZE_OFFSET]);
	if (header_size < GPT_HEADER_SIZE || header_size > dev->block_size) {
		dprintf(INFO, "gpt on '%s': invalid header size\n", device);
		return -1;
	}

	crc_val_org = GET_LWORD_FROM_BYTE(&data[HEADER_CRC_OFFSET]);
	PUT_LONG(&data[HEADER_CRC_OFFSET], 0);
	crc_val = crc32(~0L, data, header_size) ^ (~0L);
	if (crc_val != crc_val_org) {
		dprintf(INFO, "gpt on '%s': header crc mismatch\n", device);
		return -1;
	}

	if (GET_LLWORD_FROM_BYTE(&data[PRIMARY_HEADER_OFFSET]) != GPT_LBA) {
		dprintf(INFO, "gpt on '%s': primary header LBA mismatch\n", device);
		return -1;
	}

	first_usable_lba = GET_LLWORD_FROM_BYTE(&data[FIRST_USABLE_LBA_OFFSET]);
	entries_lba = GET_LLWORD_FROM_BYTE(&data[PARTITION_ENTRIES_OFFSET]);
	max_partition_count = GET_LWORD_FROM_BYTE(&data[PARTITION_COUNT_OFFSET]);
	partition_entry_size = GET_LWORD_FROM_BYTE(&data[PENTRY_SIZE_OFFSET]);

	if (partition_entry_size < ENTRY_SIZE ||
	    partition_entry_size > dev->block_size ||
	    max_partition_count > NUM_PARTITIONS || entries_lba < 2) {
		dprintf(INFO, "gpt on '%s': invalid header\n", device);
		return -1;
	}

	dprintf(SPEW, "gpt on '%s': %u entries at lba %llu\n",
		device, max_partition_count, entries_lba);

	part_entry_cnt = dev->block_size / partition_entry_size;

	for (i = 0; i < max_partition_count && count < GPT_MAX_PUBLISH; i++) {
		const unsigned char *entry;
		char subdevice[128];
		bdev_t *subdev;

		/* Read the next block of partition entries */
		if (i % part_entry_cnt == 0) {
			err = bio_read(dev, data, offset +
				       (entries_lba + i / part_entry_cnt) *
				       dev->block_size, dev->block_size);
			if (err != (int)dev->block_size)
				break;
		}
		entry = &data[(i % part_entry_cnt) * partition_entry_size];

		/* An unused type GUID terminates the table */
		if (entry[0] == 0x00 && entry[1] == 0x00)
			break;

		first_lba = GET_LLWORD_FROM_BYTE(&entry[FIRST_LBA_OFFSET]);
		last_lba = GET_LLWORD_FROM_BYTE(&entry[LAST_LBA_OFFSET]);

		/* If the partition entry LBA is not valid, skip this entry */
		if (first_lba < first_usable_lba || first_lba > last_lba ||
		    last_lba >= dev->block_count) {
			dprintf(INFO, "gpt on '%s': entry %d lba not valid\n",
				device, i);
			continue;
		}

		sprintf(subdevice, "%sp%d", device, i);

		err = bio_publish_subdevice(device, subdevice, first_lba,
					    last_lba - first_lba + 1);
		if (err < 0) {
			dprintf(INFO, "error publishing subdevice '%s'\n", subdevice);
			continue;
		}

		subdev = bio_open(subdevice);
		if (subdev) {
			subdev->label = gpt_entry_name(&entry[PARTITION_NAME_OFFSET]);
			bio_close(subdev);
		}
		count++;
	}

	return count;
}

static status_t validate_mbr_partition(bdev_t *dev, const struct mbr_part *part)
{
	/* check for invalid types */
	if (part->type == 0)
		return -1;
	/* a protective MBR means the real partition table is a (corrupt) GPT */
	if (part->type == MBR_TYPE_GPT_PROTECTIVE)
		return -1;
	/* check for invalid status */
	if (part->status != 0x80 && part->status != 0x00)
		return -1;

	/* make sure the range fits within the device */
	if (part->lba_start >= dev->block_count)
		return -1;
	if ((part->lba_start + part->lba_length) > dev->block_count)
		return -1;

	/* that's about all we can do, MBR has no other good way to see if it's valid */

	return 0;
}

int partition_publish(const char *device, off_t offset)
{
	int err = 0;
	int count = 0;

	// clear any partitions that may have already existed
	partition_unpublish(device);

	bdev_t *dev = bio_open(device);
	if (!dev) {
		printf("partition_publish: unable to open device\n");
		return -1;
	}

	// get a dma aligned and padded block to read info
	STACKBUF_DMA_ALIGN(buf, dev->block_size);

	/* sniff for a GPT first */
	count = publish_gpt(dev, device, offset);
	if (count >= 0) {
		bio_close(dev);
		return count;
	}
	count = 0;

	/* sniff for MBR partition types */
	do {
		int i;

		err = bio_read(dev, buf, offset, 512);
		if (err < 0)
			goto err;

		/* look for the aa55 tag */
		if (buf[510] != 0x55 || buf[511] != 0xaa)
			break;

		/* see if a partition table makes sense here */
		struct mbr_part part[4];
		memcpy(part, buf + 446, sizeof(part));

#if DEBUGLEVEL >= SPEW
		dprintf(SPEW, "mbr partition table dump:\n");
		for (i=0; i < 4; i++) {
			dprintf(SPEW, "\t%i: status 0x%hhx, type 0x%hhx, start 0x%x, len 0x%x\n", i, part[i].status, part[i].type, part[i].lba_start, part[i].lba_length);
		}
#endif

		/* validate each of the partition entries */
		for (i=0; i < 4; i++) {
			if (validate_mbr_partition(dev, &part[i]) >= 0) {
				// publish it
				char subdevice[128];

				sprintf(subdevice, "%sp%d", device, i); 

				err = bio_publish_subdevice(device, subdevice, part[i].lba_start, part[i].lba_length);
				if (err < 0) {
					dprintf(INFO, "error publishing subdevice '%s'\n", subdevice);
					continue;
				}
				count++;
			}
		}
	} while(0);

	bio_close(dev);

err:
	return (err < 0) ? err : count;
}

int partition_unpublish(const char *device)
{
	int i;
	int count;
	bdev_t *dev;
	char devname[512];	

	count = 0;
	for (i=0; i < 16; i++) {
		sprintf(devname, "%sp%d", device, i);

		dev = bio_open(devname);
		if (!dev)
			continue;

		bio_unregister_device(dev);
		bio_close(dev);
		count++;
	}

	return count;
}

