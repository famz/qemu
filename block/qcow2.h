/*
 * Block driver for the QCOW version 2 format
 *
 * Copyright (c) 2004-2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef BLOCK_QCOW2_H
#define BLOCK_QCOW2_H

#include "block/aes.h"
#include "block/coroutine.h"

//#define DEBUG_ALLOC
//#define DEBUG_ALLOC2
//#define DEBUG_EXT

#define QCOW_MAGIC (('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)

#define QCOW_CRYPT_NONE 0
#define QCOW_CRYPT_AES  1

#define QCOW_MAX_CRYPT_CLUSTERS 32

/* indicate that the refcount of the referenced cluster is exactly one. */
#define QCOW_OFLAG_COPIED     (1LL << 63)
/* indicate that the cluster is compressed (they never have the copied flag) */
#define QCOW_OFLAG_COMPRESSED (1LL << 62)
/* The cluster reads as all zeros */
#define QCOW_OFLAG_ZERO (1LL << 0)

#define REFCOUNT_SHIFT 1 /* refcount size is 2 bytes */

#define MIN_CLUSTER_BITS 9
#define MAX_CLUSTER_BITS 21

#define L2_CACHE_SIZE 16

/* Must be at least 4 to cover all cases of refcount table growth */
#define REFCOUNT_CACHE_SIZE 4

#define DEFAULT_CLUSTER_SIZE 65536

typedef struct QCowHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t backing_file_offset;
    uint32_t backing_file_size;
    uint32_t cluster_bits;
    uint64_t size; /* in bytes */
    uint32_t crypt_method;
    uint32_t l1_size; /* XXX: save number of clusters instead ? */
    uint64_t l1_table_offset;
    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;
    uint32_t nb_snapshots;
    uint64_t snapshots_offset;

    /* The following fields are only valid for version >= 3 */
    uint64_t incompatible_features;
    uint64_t compatible_features;
    uint64_t autoclear_features;

    uint32_t refcount_order;
    uint32_t header_length;
} QCowHeader;

typedef struct QCowSnapshot {
    uint64_t l1_table_offset;
    uint32_t l1_size;
    char *id_str;
    char *name;
    uint64_t disk_size;
    uint64_t vm_state_size;
    uint32_t date_sec;
    uint32_t date_nsec;
    uint64_t vm_clock_nsec;
} QCowSnapshot;

struct Qcow2Cache;
typedef struct Qcow2Cache Qcow2Cache;

typedef struct Qcow2UnknownHeaderExtension {
    uint32_t magic;
    uint32_t len;
    QLIST_ENTRY(Qcow2UnknownHeaderExtension) next;
    uint8_t data[];
} Qcow2UnknownHeaderExtension;

enum {
    QCOW2_FEAT_TYPE_INCOMPATIBLE    = 0,
    QCOW2_FEAT_TYPE_COMPATIBLE      = 1,
    QCOW2_FEAT_TYPE_AUTOCLEAR       = 2,
};

/* Incompatible feature bits */
enum {
    QCOW2_INCOMPAT_DIRTY_BITNR   = 0,
    QCOW2_INCOMPAT_DIRTY         = 1 << QCOW2_INCOMPAT_DIRTY_BITNR,

    QCOW2_INCOMPAT_MASK          = QCOW2_INCOMPAT_DIRTY,
};

/* Compatible feature bits */
enum {
    QCOW2_COMPAT_LAZY_REFCOUNTS_BITNR = 0,
    QCOW2_COMPAT_LAZY_REFCOUNTS       = 1 << QCOW2_COMPAT_LAZY_REFCOUNTS_BITNR,

    QCOW2_COMPAT_FEAT_MASK            = QCOW2_COMPAT_LAZY_REFCOUNTS,
};

typedef struct Qcow2Feature {
    uint8_t type;
    uint8_t bit;
    char    name[46];
} QEMU_PACKED Qcow2Feature;

typedef struct BDRVQcowState {
    int cluster_bits;
    int cluster_size;
    int cluster_sectors;
    int l2_bits;
    int l2_size;
    int l1_size;
    int l1_vm_state_index;
    int csize_shift;
    int csize_mask;
    uint64_t cluster_offset_mask;
    uint64_t l1_table_offset;
    uint64_t *l1_table;

    Qcow2Cache* l2_table_cache;
    Qcow2Cache* refcount_block_cache;

    uint8_t *cluster_cache;
    uint8_t *cluster_data;
    uint64_t cluster_cache_offset;
    QLIST_HEAD(QCowClusterAlloc, QCowL2Meta) cluster_allocs;

    uint64_t *refcount_table;
    uint64_t refcount_table_offset;
    uint32_t refcount_table_size;
    int64_t free_cluster_index;
    int64_t free_byte_offset;

    CoMutex lock;

    uint32_t crypt_method; /* current crypt method, 0 if no key yet */
    uint32_t crypt_method_header;
    AES_KEY aes_encrypt_key;
    AES_KEY aes_decrypt_key;
    uint64_t snapshots_offset;
    int snapshots_size;
    int nb_snapshots;
    QCowSnapshot *snapshots;

    int flags;
    int qcow_version;

    uint64_t incompatible_features;
    uint64_t compatible_features;
    uint64_t autoclear_features;

    size_t unknown_header_fields_size;
    void* unknown_header_fields;
    QLIST_HEAD(, Qcow2UnknownHeaderExtension) unknown_header_ext;
} BDRVQcowState;

/* XXX: use std qcow open function ? */
typedef struct QCowCreateState {
    int cluster_size;
    int cluster_bits;
    uint16_t *refcount_block;
    uint64_t *refcount_table;
    int64_t l1_table_offset;
    int64_t refcount_table_offset;
    int64_t refcount_block_offset;
} QCowCreateState;

struct QCowAIOCB;

typedef struct Qcow2COWRegion {
    /**
     * Offset of the COW region in bytes from the start of the first cluster
     * touched by the request.
     */
    uint64_t    offset;

    /** Number of sectors to copy */
    int         nb_sectors;
} Qcow2COWRegion;

/**
 * Describes an in-flight (part of a) write request that writes to clusters
 * that are not referenced in their L2 table yet.
 */
typedef struct QCowL2Meta
{
    /** Guest offset of the first newly allocated cluster */
    uint64_t offset;

    /** Host offset of the first newly allocated cluster */
    uint64_t alloc_offset;

    /**
     * Number of sectors from the start of the first allocated cluster to
     * the end of the (possibly shortened) request
     */
    int nb_available;

    /** Number of newly allocated clusters */
    int nb_clusters;

    /**
     * true if the guest data (but not necessarily the related COW) has been
     * written to disk, so that read requests can (and after having completed
     * this request actually _must_) read the new data instead of reading the
     * old data that the L2 table still refers to.
     */
    bool is_written;

    /**
     * Requests that overlap with this allocation and wait to be restarted
     * when the allocating request has completed.
     */
    CoQueue dependent_requests;

    /**
     * The COW Region between the start of the first allocated cluster and the
     * area the guest actually writes to.
     */
    Qcow2COWRegion cow_start;

    /**
     * The COW Region between the area the guest actually writes to and the
     * end of the last allocated cluster.
     */
    Qcow2COWRegion cow_end;

    QLIST_ENTRY(QCowL2Meta) next_in_flight;
} QCowL2Meta;

enum {
    QCOW2_CLUSTER_UNALLOCATED,
    QCOW2_CLUSTER_NORMAL,
    QCOW2_CLUSTER_COMPRESSED,
    QCOW2_CLUSTER_ZERO
};

#define L1E_OFFSET_MASK 0x00ffffffffffff00ULL
#define L2E_OFFSET_MASK 0x00ffffffffffff00ULL
#define L2E_COMPRESSED_OFFSET_SIZE_MASK 0x3fffffffffffffffULL

#define REFT_OFFSET_MASK 0xffffffffffffff00ULL

static inline int size_to_clusters(BDRVQcowState *s, int64_t size)
{
    return (size + (s->cluster_size - 1)) >> s->cluster_bits;
}

static inline int size_to_l1(BDRVQcowState *s, int64_t size)
{
    int shift = s->cluster_bits + s->l2_bits;
    return (size + (1ULL << shift) - 1) >> shift;
}

static inline int64_t align_offset(int64_t offset, int n)
{
    offset = (offset + n - 1) & ~(n - 1);
    return offset;
}

static inline int qcow2_get_cluster_type(uint64_t l2_entry)
{
    if (l2_entry & QCOW_OFLAG_COMPRESSED) {
        return QCOW2_CLUSTER_COMPRESSED;
    } else if (l2_entry & QCOW_OFLAG_ZERO) {
        return QCOW2_CLUSTER_ZERO;
    } else if (!(l2_entry & L2E_OFFSET_MASK)) {
        return QCOW2_CLUSTER_UNALLOCATED;
    } else {
        return QCOW2_CLUSTER_NORMAL;
    }
}

/* Check whether refcounts are eager or lazy */
static inline bool qcow2_need_accurate_refcounts(BDRVQcowState *s)
{
    return !(s->incompatible_features & QCOW2_INCOMPAT_DIRTY);
}

static inline uint64_t l2meta_req_start(QCowL2Meta *m)
{
    return (m->offset + m->cow_start.offset)
        + (m->cow_start.nb_sectors << BDRV_SECTOR_BITS);
}

static inline uint64_t l2meta_req_end(QCowL2Meta *m)
{
    return m->offset + (m->nb_available << BDRV_SECTOR_BITS);
}

// FIXME Need qcow2_ prefix to global functions

/* qcow2.c functions */
int qcow2_backing_read1(BlockDriverState *bs, QEMUIOVector *qiov,
                  int64_t sector_num, int nb_sectors);

int qcow2_mark_dirty(BlockDriverState *bs);
int qcow2_update_header(BlockDriverState *bs);

/* qcow2-refcount.c functions */
int qcow2_refcount_init(BlockDriverState *bs);
void qcow2_refcount_close(BlockDriverState *bs);

int64_t qcow2_alloc_clusters(BlockDriverState *bs, int64_t size);
int qcow2_alloc_clusters_at(BlockDriverState *bs, uint64_t offset,
    int nb_clusters);
int64_t qcow2_alloc_bytes(BlockDriverState *bs, int size);
void qcow2_free_clusters(BlockDriverState *bs,
    int64_t offset, int64_t size);
void qcow2_free_any_clusters(BlockDriverState *bs,
    uint64_t cluster_offset, int nb_clusters);

int qcow2_update_snapshot_refcount(BlockDriverState *bs,
    int64_t l1_table_offset, int l1_size, int addend);

int qcow2_check_refcounts(BlockDriverState *bs, BdrvCheckResult *res,
                          BdrvCheckMode fix);

/* qcow2-cluster.c functions */
int qcow2_grow_l1_table(BlockDriverState *bs, int min_size, bool exact_size);
void qcow2_l2_cache_reset(BlockDriverState *bs);
int qcow2_decompress_cluster(BlockDriverState *bs, uint64_t cluster_offset);
void qcow2_encrypt_sectors(BDRVQcowState *s, int64_t sector_num,
                     uint8_t *out_buf, const uint8_t *in_buf,
                     int nb_sectors, int enc,
                     const AES_KEY *key);

int qcow2_get_cluster_offset(BlockDriverState *bs, uint64_t offset,
    int *num, uint64_t *cluster_offset);
int qcow2_alloc_cluster_offset(BlockDriverState *bs, uint64_t offset,
    int n_start, int n_end, int *num, uint64_t *host_offset, QCowL2Meta **m);
uint64_t qcow2_alloc_compressed_cluster_offset(BlockDriverState *bs,
                                         uint64_t offset,
                                         int compressed_size);

int qcow2_alloc_cluster_link_l2(BlockDriverState *bs, QCowL2Meta *m);
int qcow2_discard_clusters(BlockDriverState *bs, uint64_t offset,
    int nb_sectors);
int qcow2_zero_clusters(BlockDriverState *bs, uint64_t offset, int nb_sectors);

/* qcow2-snapshot.c functions */
int qcow2_snapshot_create(BlockDriverState *bs, QEMUSnapshotInfo *sn_info);
int qcow2_snapshot_goto(BlockDriverState *bs, const char *snapshot_id);
int qcow2_snapshot_delete(BlockDriverState *bs, const char *snapshot_id);
int qcow2_snapshot_list(BlockDriverState *bs, QEMUSnapshotInfo **psn_tab);
int qcow2_snapshot_load_tmp(BlockDriverState *bs, const char *snapshot_name);

void qcow2_free_snapshots(BlockDriverState *bs);
int qcow2_read_snapshots(BlockDriverState *bs);

/* qcow2-cache.c functions */
Qcow2Cache *qcow2_cache_create(BlockDriverState *bs, int num_tables);
int qcow2_cache_destroy(BlockDriverState* bs, Qcow2Cache *c);

void qcow2_cache_entry_mark_dirty(Qcow2Cache *c, void *table);
int qcow2_cache_flush(BlockDriverState *bs, Qcow2Cache *c);
int qcow2_cache_set_dependency(BlockDriverState *bs, Qcow2Cache *c,
    Qcow2Cache *dependency);
void qcow2_cache_depends_on_flush(Qcow2Cache *c);

int qcow2_cache_get(BlockDriverState *bs, Qcow2Cache *c, uint64_t offset,
    void **table);
int qcow2_cache_get_empty(BlockDriverState *bs, Qcow2Cache *c, uint64_t offset,
    void **table);
int qcow2_cache_put(BlockDriverState *bs, Qcow2Cache *c, void **table);

#endif
