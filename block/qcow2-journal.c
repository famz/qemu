/*
 * qcow2 journalling functions
 *
 * Copyright (c) 2013 Kevin Wolf <kwolf@redhat.com>
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

#include "qemu-common.h"
#include "block/block_int.h"
#include "qcow2.h"

#define QCOW2_JOURNAL_MAGIC 0x716a6f75726e616cULL  /* "qjournal" */
#define QCOW2_JOURNAL_BLOCK_MAGIC 0x716a626b  /* "qjbk" */

typedef struct Qcow2JournalHeader {
    uint64_t    magic;
    uint32_t    journal_size;
    uint32_t    block_size;
    uint32_t    synced_index;
    uint32_t    synced_seq;
    uint32_t    committed_seq;
    uint32_t    checksum;
} QEMU_PACKED Qcow2JournalHeader;

/*
 * One big transaction per journal block. The transaction is committed either
 * time based or when a microtransaction (single set of operations that must be
 * performed atomically) doesn't fit in the same block any more.
 */
typedef struct Qcow2JournalBlock {
    uint32_t    magic;
    uint32_t    checksum;
    uint32_t    seq;
    uint32_t    desc_offset; /* Allow block header extensions */
    uint32_t    desc_bytes;
    uint32_t    nb_data_blocks;
} QEMU_PACKED Qcow2JournalBlock;

