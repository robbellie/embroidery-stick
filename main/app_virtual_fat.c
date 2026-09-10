/* SPDX-License-Identifier: Apache-2.0 */
#include "app_virtual_fat.h"
#include "app_cache.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

static const char *TAG = "vfat";

/* ---- FAT16 geometry ---------------------------------------------------- */
#define SECTOR_SIZE         512u
#define SECTORS_PER_CLUSTER 8u                          /* 4 KB clusters     */
#define CLUSTER_BYTES       (SECTOR_SIZE * SECTORS_PER_CLUSTER)
#define NUM_FATS            2u
#define ROOT_ENTRY_COUNT    512u                        /* max root-level entries */
#define ROOT_DIR_SECTORS    (ROOT_ENTRY_COUNT * 32u / SECTOR_SIZE)   /* = 32 */
#define RESERVED_SECTORS    4u                          /* within FAT volume */
#define MBR_SECTORS         1u                          /* sector 0 = MBR    */

/* First sector of the FAT volume relative to disk start */
#define VOL_START_LBA       MBR_SECTORS                /* = 1 */

/* Within the FAT volume reserved area: BPB is at volume LBA 0 (= disk LBA 1) */
#define FAT1_VOL_LBA        RESERVED_SECTORS            /* = 4 */

/* Absolute disk LBAs — computed after fat_sectors is known */
#define FAT1_LBA            (VOL_START_LBA + FAT1_VOL_LBA)  /* = 5 */

/* FAT16 directory entry (32 bytes) */
typedef struct __attribute__((packed)) {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;   /* always 0 for FAT16 */
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat16_dir_entry_t;

#define ATTR_ARCHIVE   0x20u
#define ATTR_DIRECTORY 0x10u

/*
 * One node in the tree — either a file (served via app_cache/the network)
 * or a directory (served from its own pre-built entry table in
 * s.dir_data, synthesized locally, never fetched). Mirrors the wire's
 * flat array + parent-pointer shape 1:1: s.nodes[i] always corresponds to
 * the i-th entry of the proto_file_info_t array passed to vfat_init(),
 * and — guaranteed by the producer's id-assignment scheme (id == array
 * position) — s.nodes[id] is always a direct, O(1) lookup by id.
 */
typedef struct {
    uint16_t id;
    uint16_t parent_id;     /* EMBROIDERY_ROOT_PARENT_ID = lives directly in root */
    bool     is_dir;
    /* file fields (is_dir == false) */
    uint32_t file_size;
    uint16_t start_cluster; /* 0 = no cluster (empty file) */
    uint32_t num_clusters;
    /* directory fields (is_dir == true); unused/zero for root itself,
     * which keeps its own dedicated s.root_dir buffer and never gets a
     * node of its own here */
    uint16_t dir_start_cluster;
    uint32_t dir_num_clusters;
    uint32_t dir_data_offset; /* byte offset into s.dir_data */
} fs_node_t;

/* ---- Module state ------------------------------------------------------ */
static struct {
    uint8_t       mbr[SECTOR_SIZE];
    uint8_t       bpb[SECTOR_SIZE];    /* BPB sits in reserved sector 0 of volume */
    uint8_t      *fat_data;            /* 2 × fat_sectors × 512 bytes */
    uint8_t      *root_dir;            /* ROOT_DIR_SECTORS × 512 bytes */
    uint8_t      *dir_data;            /* every non-root directory's own entry table, back-to-back */
    uint32_t      dir_data_bytes;
    fs_node_t    *nodes;
    uint16_t      node_count;
    uint32_t      fat_sectors;
    uint32_t      fat2_lba;            /* absolute disk LBA */
    uint32_t      rootdir_lba;
    uint32_t      data_lba;
    uint32_t      total_sectors;       /* entire disk including MBR */
    bool          initialized;
} s;

/* ---- Helpers ----------------------------------------------------------- */

static uint32_t div_ceil(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static void make_83(const char *src, char name_out[8], char ext_out[3])
{
    memset(name_out, ' ', 8);
    memset(ext_out,  ' ', 3);
    const char *dot = strrchr(src, '.');
    size_t nlen = dot ? (size_t)(dot - src) : strlen(src);
    if (nlen > 8) nlen = 8;
    for (size_t i = 0; i < nlen; i++) name_out[i] = (char)toupper((unsigned char)src[i]);
    if (dot && dot[1]) {
        size_t elen = strlen(dot + 1);
        if (elen > 3) elen = 3;
        for (size_t i = 0; i < elen; i++) ext_out[i] = (char)toupper((unsigned char)dot[1 + i]);
    }
}

static uint16_t to_fat_time(uint32_t mtime)
{
    struct tm t; time_t tt = (time_t)mtime; gmtime_r(&tt, &t);
    return (uint16_t)(((t.tm_hour & 0x1f) << 11) | ((t.tm_min & 0x3f) << 5) | ((t.tm_sec / 2) & 0x1f));
}

static uint16_t to_fat_date(uint32_t mtime)
{
    struct tm t; time_t tt = (time_t)mtime; gmtime_r(&tt, &t);
    int year = t.tm_year - 80; if (year < 0) year = 0;
    return (uint16_t)(((year & 0x7f) << 9) | (((t.tm_mon + 1) & 0x0f) << 5) | (t.tm_mday & 0x1f));
}

static void set_dir_entry_timestamps(fat16_dir_entry_t *e, uint32_t mtime)
{
    if (!mtime) return;
    uint16_t ft = to_fat_time(mtime);
    uint16_t fd = to_fat_date(mtime);
    e->crt_time = ft; e->crt_date = fd;
    e->wrt_time = ft; e->wrt_date = fd;
    e->lst_acc_date = fd;
}

/* O(1): guaranteed by the wire producer's id-assignment scheme (id ==
 * array position at the time the catalog was flattened). Returns NULL for
 * an out-of-range id as a defensive fallback, not something expected in
 * normal operation. */
static fs_node_t *find_node_by_id(uint16_t id)
{
    return (id < s.node_count) ? &s.nodes[id] : NULL;
}

/* ---- MBR --------------------------------------------------------------- */

static void build_mbr(uint32_t vol_sectors)
{
    memset(s.mbr, 0, SECTOR_SIZE);
    uint8_t *p = s.mbr + 446;
    p[0] = 0x80;                                /* bootable                */
    p[1] = 0x00; p[2] = 0x01; p[3] = 0x00;    /* CHS start (ignore)      */
    p[4] = 0x0E;                                /* type: FAT16 with LBA    */
    p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;    /* CHS end (ignore)        */
    /* LBA start = 1 (after MBR) */
    p[8]  = 0x01; p[9]  = 0x00; p[10] = 0x00; p[11] = 0x00;
    /* LBA size = volume sectors */
    p[12] = (uint8_t)(vol_sectors);
    p[13] = (uint8_t)(vol_sectors >> 8);
    p[14] = (uint8_t)(vol_sectors >> 16);
    p[15] = (uint8_t)(vol_sectors >> 24);
    s.mbr[510] = 0x55;
    s.mbr[511] = 0xAA;
}

/* ---- BPB --------------------------------------------------------------- */

static void build_bpb(uint32_t vol_sectors, uint16_t fat_sz, const char *label)
{
    memset(s.bpb, 0, SECTOR_SIZE);
    uint8_t *b = s.bpb;

    /* Jump + OEM */
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);

    /* BIOS Parameter Block */
    b[11] = (SECTOR_SIZE) & 0xFF; b[12] = (SECTOR_SIZE) >> 8;    /* bytes/sector     */
    b[13] = SECTORS_PER_CLUSTER;                                   /* sec/cluster      */
    b[14] = RESERVED_SECTORS; b[15] = 0;                          /* reserved sectors */
    b[16] = NUM_FATS;                                              /* # FATs           */
    b[17] = ROOT_ENTRY_COUNT & 0xFF; b[18] = ROOT_ENTRY_COUNT >> 8; /* root entries   */
    b[19] = 0; b[20] = 0;                                         /* total16 = 0      */
    b[21] = 0xF8;                                                  /* media            */
    b[22] = fat_sz & 0xFF; b[23] = fat_sz >> 8;                  /* FAT sectors      */
    b[24] = 63; b[25] = 0;                                        /* sec/track        */
    b[26] = 255; b[27] = 0;                                       /* # heads          */
    /* hidden sectors = MBR_SECTORS = 1 */
    b[28] = MBR_SECTORS; b[29] = 0; b[30] = 0; b[31] = 0;
    /* total32 */
    b[32] = (uint8_t)vol_sectors;
    b[33] = (uint8_t)(vol_sectors >> 8);
    b[34] = (uint8_t)(vol_sectors >> 16);
    b[35] = (uint8_t)(vol_sectors >> 24);

    /* Extended BPB (FAT16) */
    b[36] = 0x80;   /* drive number */
    b[37] = 0x00;
    b[38] = 0x29;   /* boot signature */
    /* volume ID (fixed, not critical) */
    b[39] = 0xDE; b[40] = 0xAD; b[41] = 0xBE; b[42] = 0xEF;
    /* volume label, padded with spaces */
    char lbl[11]; memset(lbl, ' ', 11);
    size_t llen = strlen(label); if (llen > 11) llen = 11;
    for (size_t i = 0; i < llen; i++) lbl[i] = (char)toupper((unsigned char)label[i]);
    memcpy(b + 43, lbl, 11);
    memcpy(b + 54, "FAT12   ", 8);

    b[510] = 0x55; b[511] = 0xAA;
}

/* ---- FAT table --------------------------------------------------------- */

/* FAT12: each entry is 12 bits, packed 2 entries per 3 bytes.
 * Linux determines FAT type solely from cluster count:
 *   < 4085 clusters → FAT12,  < 65525 → FAT16,  else FAT32.
 * Our small disks have far fewer than 4085 clusters, so we must
 * write a proper FAT12 table or the host will misread the chain. */
static void fat12_set(uint8_t *fat, uint16_t n, uint16_t val)
{
    uint32_t off = n + (n >> 1);  /* n * 3 / 2 */
    if (n & 1) {
        fat[off]     = (fat[off] & 0x0F) | (uint8_t)((val & 0x0F) << 4);
        fat[off + 1] = (uint8_t)(val >> 4);
    } else {
        fat[off]     = (uint8_t)(val & 0xFF);
        fat[off + 1] = (fat[off + 1] & 0xF0) | (uint8_t)((val >> 8) & 0x0F);
    }
}

/* Chains whichever cluster range applies — a file's data clusters or a
 * directory's own entry-table clusters — identical mechanism either way,
 * since both are ordinary data-area content as far as the FAT itself is
 * concerned. */
static void build_fat(void)
{
    uint32_t fat_bytes = s.fat_sectors * SECTOR_SIZE;
    memset(s.fat_data, 0, fat_bytes * NUM_FATS);

    uint8_t *fat = s.fat_data;
    fat12_set(fat, 0, 0xFF8);   /* media byte */
    fat12_set(fat, 1, 0xFFF);   /* reserved   */

    for (uint16_t i = 0; i < s.node_count; i++) {
        uint16_t start; uint32_t nc;
        if (s.nodes[i].is_dir) {
            start = s.nodes[i].dir_start_cluster;
            nc    = s.nodes[i].dir_num_clusters;
        } else {
            start = s.nodes[i].start_cluster;
            nc    = s.nodes[i].num_clusters;
        }
        for (uint32_t j = 0; j + 1 < nc; j++) {
            fat12_set(fat, (uint16_t)(start + j), (uint16_t)(start + j + 1));
        }
        if (nc > 0) fat12_set(fat, (uint16_t)(start + nc - 1), 0xFFF);
    }

    /* FAT2 = copy of FAT1 */
    memcpy(s.fat_data + fat_bytes, s.fat_data, fat_bytes);
}

/* ---- Root directory ------------------------------------------------------
 * Root keeps its own fixed-size, fixed-location buffer and is never a
 * node in s.nodes[] itself — real FAT16 behavior (only FAT32 puts the
 * root in the ordinary data area; out of scope here). Only entries whose
 * parent_id is the root are written here; nested entries are handled by
 * build_dir_table() instead. */

static void build_root_dir(const proto_file_info_t *infos)
{
    memset(s.root_dir, 0, ROOT_DIR_SECTORS * SECTOR_SIZE);
    fat16_dir_entry_t *dir = (fat16_dir_entry_t *)s.root_dir;

    uint16_t out_idx = 0;
    for (uint16_t i = 0; i < s.node_count; i++) {
        if (s.nodes[i].parent_id != EMBROIDERY_ROOT_PARENT_ID) continue;

        fat16_dir_entry_t *e = &dir[out_idx++];
        make_83(infos[i].name, e->name, e->ext);
        e->fst_clus_hi = 0;
        if (s.nodes[i].is_dir) {
            e->attr      = ATTR_DIRECTORY;
            e->fst_clus_lo = s.nodes[i].dir_start_cluster;
            e->file_size   = 0;
        } else {
            e->attr      = ATTR_ARCHIVE;
            e->fst_clus_lo = (s.nodes[i].num_clusters > 0) ? s.nodes[i].start_cluster : 0;
            e->file_size   = infos[i].size;
        }
        set_dir_entry_timestamps(e, infos[i].mtime);
    }
}

/* ---- Non-root directory entry tables ------------------------------------
 * Every non-root directory's own entries live in ordinary data-area
 * clusters, exactly like file content — this is what lets a subdirectory
 * be "just another kind of data-area content" rather than a structurally
 * new concept. Writes "." and ".." first (every directory has exactly
 * these two, even an empty one — accounted for in vfat_init()'s cluster
 * sizing), then one entry per child, in the same flattened order the
 * producer already emitted them in.
 *
 * Not recursive: driven by a flat scan of s.nodes[] for this one
 * directory's direct children, called once per directory from vfat_init()
 * after every node's cluster numbers are already known. Nesting depth has
 * no effect on this function at all — a directory only ever references
 * its own immediate parent/children by cluster number, never a path —
 * which also means no C stack-depth concern for deep nesting.
 */
static void build_dir_table(const proto_file_info_t *infos, fs_node_t *dnode)
{
    fat16_dir_entry_t *entries = (fat16_dir_entry_t *)(s.dir_data + dnode->dir_data_offset);
    size_t idx = 0;

    /* "." -> self */
    memset(&entries[idx], 0, sizeof(entries[idx]));
    memset(entries[idx].name, ' ', 8); entries[idx].name[0] = '.';
    memset(entries[idx].ext,  ' ', 3);
    entries[idx].attr = ATTR_DIRECTORY;
    entries[idx].fst_clus_lo = dnode->dir_start_cluster;
    idx++;

    /* ".." -> parent (cluster 0 conventionally means "the root", which has
     * no cluster number of its own) */
    memset(&entries[idx], 0, sizeof(entries[idx]));
    memset(entries[idx].name, ' ', 8); entries[idx].name[0] = entries[idx].name[1] = '.';
    memset(entries[idx].ext,  ' ', 3);
    entries[idx].attr = ATTR_DIRECTORY;
    if (dnode->parent_id == EMBROIDERY_ROOT_PARENT_ID) {
        entries[idx].fst_clus_lo = 0;
    } else {
        fs_node_t *parent = find_node_by_id(dnode->parent_id);
        entries[idx].fst_clus_lo = parent ? parent->dir_start_cluster : 0;
    }
    idx++;

    /* Children, in the producer's already-sorted flattened order */
    for (uint16_t i = 0; i < s.node_count; i++) {
        if (s.nodes[i].parent_id != dnode->id) continue;

        fat16_dir_entry_t *e = &entries[idx++];
        make_83(infos[i].name, e->name, e->ext);
        e->fst_clus_hi = 0;
        if (s.nodes[i].is_dir) {
            e->attr        = ATTR_DIRECTORY;
            e->fst_clus_lo = s.nodes[i].dir_start_cluster;
            e->file_size   = 0;
        } else {
            e->attr        = ATTR_ARCHIVE;
            e->fst_clus_lo = (s.nodes[i].num_clusters > 0) ? s.nodes[i].start_cluster : 0;
            e->file_size   = infos[i].size;
        }
        set_dir_entry_timestamps(e, infos[i].mtime);
    }
    /* Remaining slots in the last cluster stay zeroed (already the case —
     * s.dir_data is zero-filled at allocation time) — a first name byte
     * of 0x00 correctly marks "no more entries" per the FAT spec. */
}

/* ---- Public API -------------------------------------------------------- */

bool vfat_init(const proto_file_info_t *files, uint16_t count, const char *volume_label)
{
    vfat_deinit();

    if (count > ROOT_ENTRY_COUNT) {
        ESP_LOGW(TAG, "Truncating entry list from %u to %u", count, ROOT_ENTRY_COUNT);
        count = ROOT_ENTRY_COUNT;
    }

    s.nodes = calloc(count, sizeof(fs_node_t));
    if (!s.nodes && count > 0) return false;
    s.node_count = count;

    /* Pass A: copy id/parent/is_dir straight from the wire, and tally
     * each directory's child count (indexed directly by id, relying on
     * the producer's id==array-position guarantee) so directory cluster
     * sizing (Pass B) knows exactly how big each one's own entry table
     * needs to be — no fixed per-directory cap, sized to actual content. */
    uint32_t *child_count = count ? calloc(count, sizeof(uint32_t)) : NULL;
    if (count > 0 && !child_count) { vfat_deinit(); return false; }

    for (uint16_t i = 0; i < count; i++) {
        s.nodes[i].id        = files[i].id;
        s.nodes[i].parent_id = files[i].parent_id;
        s.nodes[i].is_dir    = files[i].is_dir;
        if (!files[i].is_dir) {
            s.nodes[i].file_size = files[i].size;
        }
        if (files[i].parent_id != EMBROIDERY_ROOT_PARENT_ID && files[i].parent_id < count) {
            child_count[files[i].parent_id]++;
        }
    }

    /* Pass B: allocate clusters. Files: unchanged from before (ceil(size /
     * CLUSTER_BYTES), 0 clusters for an empty file). Directories: always
     * >=1 cluster (child_count+2 for "."/".." even when empty), chained
     * via the same generic fat12_set() used for file data — a directory
     * spanning multiple clusters needs no special handling beyond this
     * arithmetic, identical mechanism to a large file's cluster chain. */
    uint16_t next_cluster = 2;
    uint32_t total_data_clusters = 0;
    uint32_t dir_data_bytes = 0;

    for (uint16_t i = 0; i < count; i++) {
        if (s.nodes[i].is_dir) {
            uint32_t entries_needed = child_count[s.nodes[i].id] + 2; /* +2: "." and ".." */
            uint32_t bytes_needed   = entries_needed * sizeof(fat16_dir_entry_t);
            uint32_t nc             = div_ceil(bytes_needed, CLUSTER_BYTES);
            s.nodes[i].dir_num_clusters  = nc;
            s.nodes[i].dir_start_cluster = next_cluster;
            s.nodes[i].dir_data_offset   = dir_data_bytes;
            dir_data_bytes      += nc * CLUSTER_BYTES;
            next_cluster        += (uint16_t)nc;
            total_data_clusters += nc;
        } else {
            uint32_t nc = (s.nodes[i].file_size == 0) ? 0 : div_ceil(s.nodes[i].file_size, CLUSTER_BYTES);
            s.nodes[i].num_clusters  = nc;
            s.nodes[i].start_cluster = (nc > 0) ? next_cluster : 0;
            next_cluster        += (uint16_t)nc;
            total_data_clusters += nc;
        }
    }
    free(child_count);
    s.dir_data_bytes = dir_data_bytes;

    /* FAT12: 12 bits (1.5 bytes) per entry, entries 0..(total_data_clusters+1) */
    uint32_t fat_entries = total_data_clusters + 2;
    s.fat_sectors = div_ceil((fat_entries * 3 + 1) / 2, SECTOR_SIZE);
    if (s.fat_sectors == 0) s.fat_sectors = 1;

    /* Absolute disk LBAs */
    s.fat2_lba    = FAT1_LBA + s.fat_sectors;
    s.rootdir_lba = s.fat2_lba + s.fat_sectors;
    s.data_lba    = s.rootdir_lba + ROOT_DIR_SECTORS;

    /* Total disk sector count */
    uint32_t vol_sectors = RESERVED_SECTORS
                         + NUM_FATS * s.fat_sectors
                         + ROOT_DIR_SECTORS
                         + total_data_clusters * SECTORS_PER_CLUSTER;
    s.total_sectors = MBR_SECTORS + vol_sectors;

    /* Allocate FAT, root dir, and (if any non-root directories exist) the
     * shared directory-entry-table buffer. */
    uint32_t fat_alloc = NUM_FATS * s.fat_sectors * SECTOR_SIZE;
    s.fat_data = heap_caps_malloc(fat_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.fat_data) s.fat_data = malloc(fat_alloc);
    s.root_dir = heap_caps_malloc(ROOT_DIR_SECTORS * SECTOR_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.root_dir) s.root_dir = malloc(ROOT_DIR_SECTORS * SECTOR_SIZE);

    if (dir_data_bytes > 0) {
        s.dir_data = heap_caps_malloc(dir_data_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.dir_data) s.dir_data = malloc(dir_data_bytes);
        if (s.dir_data) memset(s.dir_data, 0, dir_data_bytes);
    }

    if (!s.fat_data || !s.root_dir || (dir_data_bytes > 0 && !s.dir_data)) {
        ESP_LOGE(TAG, "Allocation failed");
        vfat_deinit();
        return false;
    }

    build_mbr(vol_sectors);
    build_bpb(vol_sectors, (uint16_t)s.fat_sectors, volume_label);
    build_fat();
    build_root_dir(files);
    for (uint16_t i = 0; i < count; i++) {
        if (s.nodes[i].is_dir) build_dir_table(files, &s.nodes[i]);
    }

    s.initialized = true;
    ESP_LOGI(TAG, "init: %u entries, fat_sectors=%lu, total_sectors=%lu (%.1f MB)",
             count, s.fat_sectors, s.total_sectors, s.total_sectors * SECTOR_SIZE / 1048576.0f);
    return true;
}

void vfat_deinit(void)
{
    s.initialized = false;  /* must be first: prevents concurrent reads from accessing freed pointers */
    free(s.nodes);    s.nodes    = NULL;
    s.node_count = 0;
    free(s.fat_data);  s.fat_data  = NULL;
    free(s.root_dir);  s.root_dir  = NULL;
    free(s.dir_data);  s.dir_data  = NULL;
    s.dir_data_bytes = 0;
}

uint32_t vfat_get_sector_count(void) { return s.total_sectors; }
uint16_t vfat_get_sector_size(void)  { return SECTOR_SIZE; }

/* ---- Sector read ------------------------------------------------------- */

/* Finds which node (file or directory) owns a given data-area cluster.
 * O(n) linear scan across both cluster-range kinds — same style already
 * used here for files alone before this change; fine at <=512 nodes. */
static fs_node_t *find_node_owning_cluster(uint16_t cluster_num)
{
    for (uint16_t i = 0; i < s.node_count; i++) {
        fs_node_t *n = &s.nodes[i];
        if (n->is_dir) {
            if (n->dir_num_clusters > 0 &&
                cluster_num >= n->dir_start_cluster &&
                cluster_num <  n->dir_start_cluster + n->dir_num_clusters) {
                return n;
            }
        } else {
            if (n->num_clusters > 0 &&
                cluster_num >= n->start_cluster &&
                cluster_num <  n->start_cluster + n->num_clusters) {
                return n;
            }
        }
    }
    return NULL;
}

static vfat_read_result_t read_one_sector(uint32_t lba, uint8_t *dst)
{
    if (lba == 0) {
        memcpy(dst, s.mbr, SECTOR_SIZE);
        return VFAT_READ_OK;
    }

    /* Reserved sectors of the FAT volume (BPB at disk LBA 1) */
    if (lba < VOL_START_LBA + RESERVED_SECTORS) {
        if (lba == VOL_START_LBA) {
            memcpy(dst, s.bpb, SECTOR_SIZE);
        } else {
            memset(dst, 0, SECTOR_SIZE);
        }
        return VFAT_READ_OK;
    }

    /* FAT1 */
    if (lba >= FAT1_LBA && lba < s.fat2_lba) {
        uint32_t off = (lba - FAT1_LBA) * SECTOR_SIZE;
        memcpy(dst, s.fat_data + off, SECTOR_SIZE);
        return VFAT_READ_OK;
    }

    /* FAT2 */
    if (lba >= s.fat2_lba && lba < s.rootdir_lba) {
        uint32_t off = (lba - s.fat2_lba) * SECTOR_SIZE;
        /* FAT2 follows FAT1 in fat_data */
        memcpy(dst, s.fat_data + s.fat_sectors * SECTOR_SIZE + off, SECTOR_SIZE);
        return VFAT_READ_OK;
    }

    /* Root directory — always this dedicated fixed-size buffer, never the
     * data-area dispatch below (root has a fixed location in real FAT16;
     * only FAT32 puts the root in the data area too, out of scope here). */
    if (lba >= s.rootdir_lba && lba < s.data_lba) {
        uint32_t off = (lba - s.rootdir_lba) * SECTOR_SIZE;
        memcpy(dst, s.root_dir + off, SECTOR_SIZE);
        return VFAT_READ_OK;
    }

    /* Data area: files AND non-root directories' own entry tables both
     * live here, distinguished by which node owns the cluster. */
    if (lba >= s.data_lba && lba < s.total_sectors) {
        uint32_t lba_in_data   = lba - s.data_lba;
        uint32_t cluster_idx   = lba_in_data / SECTORS_PER_CLUSTER;
        uint32_t sector_in_cl  = lba_in_data % SECTORS_PER_CLUSTER;
        uint16_t cluster_num   = (uint16_t)(cluster_idx + 2);

        fs_node_t *owner = find_node_owning_cluster(cluster_num);
        if (!owner) {
            /* Unallocated cluster: return zeros */
            memset(dst, 0, SECTOR_SIZE);
            return VFAT_READ_OK;
        }

        if (owner->is_dir) {
            /* Directory content: served straight from the pre-built RAM
             * table, never touches app_cache/the network. */
            uint32_t byte_offset = (uint32_t)(cluster_num - owner->dir_start_cluster) * CLUSTER_BYTES
                                 + sector_in_cl * SECTOR_SIZE;
            uint32_t dir_table_bytes = owner->dir_num_clusters * CLUSTER_BYTES;
            if (byte_offset >= dir_table_bytes) {
                memset(dst, 0, SECTOR_SIZE);
                return VFAT_READ_OK;
            }
            memcpy(dst, s.dir_data + owner->dir_data_offset + byte_offset, SECTOR_SIZE);
            return VFAT_READ_OK;
        }

        /* File content — unchanged from before this change, aside from
         * `owner` replacing the old file-only `fl`. */
        uint32_t byte_offset = (uint32_t)(cluster_num - owner->start_cluster) * CLUSTER_BYTES
                             + sector_in_cl * SECTOR_SIZE;

        if (byte_offset >= owner->file_size) {
            memset(dst, 0, SECTOR_SIZE);
            return VFAT_READ_OK;
        }

        uint8_t *chunk_ptr;
        uint32_t available;
        if (!cache_get(owner->id, byte_offset, &chunk_ptr, &available)) {
            return VFAT_READ_PENDING;
        }

        uint32_t bytes_in_file = owner->file_size - byte_offset;
        uint32_t to_copy = SECTOR_SIZE;
        if (bytes_in_file < to_copy) to_copy = bytes_in_file;
        if (available      < to_copy) {
            /* Should not happen with properly sized chunks, but be safe */
            return VFAT_READ_PENDING;
        }

        memcpy(dst, chunk_ptr, to_copy);
        if (to_copy < SECTOR_SIZE) memset(dst + to_copy, 0, SECTOR_SIZE - to_copy);
        return VFAT_READ_OK;
    }

    /* Beyond end of disk */
    memset(dst, 0, SECTOR_SIZE);
    return VFAT_READ_ERROR;
}

vfat_read_result_t vfat_read_sectors(uint32_t lba, uint8_t *buf, uint32_t bufsize)
{
    if (!s.initialized) {
        /* Disk is temporarily unavailable (e.g. during refresh). Tell TinyUSB
         * to retry via the BUSY path rather than failing the command. */
        return VFAT_READ_PENDING;
    }

    uint32_t num_sectors = bufsize / SECTOR_SIZE;
    for (uint32_t i = 0; i < num_sectors; i++) {
        vfat_read_result_t r = read_one_sector(lba + i, buf + i * SECTOR_SIZE);
        if (r != VFAT_READ_OK) return r;
    }
    return VFAT_READ_OK;
}
