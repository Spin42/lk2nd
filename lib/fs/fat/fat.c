// SPDX-License-Identifier: BSD-3-Clause
/*
 * LK filesystem glue for FatFs (read-only).
 *
 * Bridges ChaN's FatFs (ff.c) to LK's fs_api and backs the FatFs
 * diskio layer with LK bio devices. One FatFs volume slot ("0:".."3:")
 * is allocated per mounted block device.
 */

#include <debug.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lib/bio.h>
#include <lib/fs.h>

#include "ff.h"
#include "diskio.h"

struct fat_volume {
	FATFS fs;
	bdev_t *dev;
	bool used;
};

static struct fat_volume volumes[FF_VOLUMES];

static status_t fresult_to_status(FRESULT res)
{
	switch (res) {
	case FR_OK:
		return NO_ERROR;
	case FR_NO_FILE:
	case FR_NO_PATH:
	case FR_NO_FILESYSTEM:
		return ERR_NOT_FOUND;
	case FR_DISK_ERR:
	case FR_NOT_READY:
		return ERR_IO;
	case FR_INVALID_NAME:
	case FR_INVALID_PARAMETER:
		return ERR_INVALID_ARGS;
	default:
		return ERR_NOT_VALID;
	}
}

/*
 * FatFs diskio backend: pdrv is the index into volumes[].
 */

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DSTATUS disk_status(BYTE pdrv)
{
	if (pdrv >= FF_VOLUMES || !volumes[pdrv].used)
		return STA_NOINIT;
	return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	bdev_t *dev;
	ssize_t ret;

	if (pdrv >= FF_VOLUMES || !volumes[pdrv].used)
		return RES_NOTRDY;

	dev = volumes[pdrv].dev;
	ret = bio_read(dev, buff, (off_t)sector * dev->block_size,
		       count * dev->block_size);
	if (ret != (ssize_t)(count * dev->block_size))
		return RES_ERROR;

	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	if (pdrv >= FF_VOLUMES || !volumes[pdrv].used)
		return RES_NOTRDY;

	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = volumes[pdrv].dev->block_count;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}

/*
 * LK fs_api implementation.
 */

static status_t fat_mount(bdev_t *dev, fscookie **cookie)
{
	struct fat_volume *vol = NULL;
	char drive[16];
	FRESULT res;
	int i;

	if (dev->block_size != FF_MAX_SS)
		return ERR_NOT_SUPPORTED;

	for (i = 0; i < FF_VOLUMES; i++) {
		if (!volumes[i].used) {
			vol = &volumes[i];
			break;
		}
	}
	if (!vol)
		return ERR_NO_MEMORY;

	vol->dev = dev;
	vol->used = true;

	snprintf(drive, sizeof(drive), "%d:", i);
	res = f_mount(&vol->fs, drive, 1);
	if (res != FR_OK) {
		vol->used = false;
		return fresult_to_status(res);
	}

	*cookie = (fscookie *)vol;
	return NO_ERROR;
}

static status_t fat_unmount(fscookie *cookie)
{
	struct fat_volume *vol = (struct fat_volume *)cookie;
	char drive[16];

	snprintf(drive, sizeof(drive), "%d:", (int)(vol - volumes));
	f_unmount(drive);
	vol->used = false;

	return NO_ERROR;
}

static void fat_path(struct fat_volume *vol, char *out, size_t out_len,
		     const char *path)
{
	snprintf(out, out_len, "%d:%s", (int)(vol - volumes), path);
}

static status_t fat_open_file(fscookie *cookie, const char *path,
			      filecookie **fcookie)
{
	struct fat_volume *vol = (struct fat_volume *)cookie;
	char full_path[FS_MAX_PATH_LEN];
	FRESULT res;
	FIL *fil;

	fil = malloc(sizeof(*fil));
	if (!fil)
		return ERR_NO_MEMORY;

	fat_path(vol, full_path, sizeof(full_path), path);
	res = f_open(fil, full_path, FA_READ);
	if (res != FR_OK) {
		free(fil);
		return fresult_to_status(res);
	}

	*fcookie = (filecookie *)fil;
	return NO_ERROR;
}

static status_t fat_stat_file(filecookie *fcookie, struct file_stat *stat)
{
	FIL *fil = (FIL *)fcookie;

	stat->is_dir = false;
	stat->size = f_size(fil);

	return NO_ERROR;
}

static ssize_t fat_read_file(filecookie *fcookie, void *buf, off_t offset,
			     size_t len)
{
	FIL *fil = (FIL *)fcookie;
	UINT bytes_read = 0;
	FRESULT res;

	res = f_lseek(fil, offset);
	if (res != FR_OK)
		return fresult_to_status(res);

	res = f_read(fil, buf, len, &bytes_read);
	if (res != FR_OK)
		return fresult_to_status(res);

	return bytes_read;
}

static status_t fat_close_file(filecookie *fcookie)
{
	FIL *fil = (FIL *)fcookie;

	f_close(fil);
	free(fil);

	return NO_ERROR;
}

static status_t fat_open_directory(fscookie *cookie, const char *path,
				   dircookie **dcookie)
{
	struct fat_volume *vol = (struct fat_volume *)cookie;
	char full_path[FS_MAX_PATH_LEN];
	FRESULT res;
	DIR *dir;

	dir = malloc(sizeof(*dir));
	if (!dir)
		return ERR_NO_MEMORY;

	fat_path(vol, full_path, sizeof(full_path), path);
	res = f_opendir(dir, full_path);
	if (res != FR_OK) {
		free(dir);
		return fresult_to_status(res);
	}

	*dcookie = (dircookie *)dir;
	return NO_ERROR;
}

static status_t fat_read_directory(dircookie *dcookie, struct dirent *ent)
{
	DIR *dir = (DIR *)dcookie;
	FILINFO info;
	FRESULT res;

	res = f_readdir(dir, &info);
	if (res != FR_OK)
		return fresult_to_status(res);

	/* End of directory */
	if (info.fname[0] == '\0')
		return ERR_NOT_FOUND;

	strlcpy(ent->name, info.fname, sizeof(ent->name));
	return NO_ERROR;
}

static status_t fat_close_directory(dircookie *dcookie)
{
	DIR *dir = (DIR *)dcookie;

	f_closedir(dir);
	free(dir);

	return NO_ERROR;
}

static const struct fs_api fat_api = {
	.mount = fat_mount,
	.unmount = fat_unmount,
	.open = fat_open_file,
	.stat = fat_stat_file,
	.read = fat_read_file,
	.close = fat_close_file,
	.opendir = fat_open_directory,
	.readdir = fat_read_directory,
	.closedir = fat_close_directory,
};

void fat_init(void)
{
	fs_register_type("fat", &fat_api);
}
