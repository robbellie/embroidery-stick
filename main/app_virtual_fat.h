/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "embroidery_protocol.h"

/*
 * Virtual FAT16 filesystem generator.
 *
 * Disk layout (all LBAs absolute):
 *   LBA 0          : MBR  (generated)
 *   LBA 1..4       : FAT16 volume reserved sectors (BPB at LBA 1)
 *   LBA 5..         : FAT1
 *   FAT2 (copy)
 *   Root directory  (512 entries = 32 sectors)
 *   Data area       (cluster 2 onwards)
 *
 * FAT and root-dir are generated once from the file list and held in RAM.
 * Data sectors are served from the cache (app_cache).
 */

typedef enum {
    VFAT_READ_OK,       /* buffer filled */
    VFAT_READ_PENDING,  /* cache miss; fetch queued — caller should stall */
    VFAT_READ_ERROR,    /* LBA out of range */
} vfat_read_result_t;

/*
 * Initialise (or re-initialise) the virtual FAT from a file list.
 * Re-initialisation is safe while USB is detached (refresh flow).
 */
bool vfat_init(const proto_file_info_t *files, uint16_t count, const char *volume_label);
void vfat_deinit(void);

uint32_t vfat_get_sector_count(void);   /* total disk sectors including MBR */
uint16_t vfat_get_sector_size(void);    /* always 512 */

/*
 * Read sectors into buffer.  bufsize must be a multiple of 512.
 * Returns VFAT_READ_PENDING if any data sector is not yet cached.
 */
vfat_read_result_t vfat_read_sectors(uint32_t lba, uint8_t *buf, uint32_t bufsize);
