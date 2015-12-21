/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

#include <db.h>

#include "portability/toku_pthread.h"
#include "portability/toku_stdint.h"
#include "portability/toku_stdlib.h"
#include "ft/serialize/rbtree_mhs.h"
#include "ft/serialize/block_allocator.h"
//Tree Block allocator.
//
// A tree block allocator manages the allocation of variable-sized blocks.
// The translation of block numbers to addresses is handled elsewhere.
// The allocation of block numbers is handled elsewhere.
//
// When creating a block allocator we also specify a certain-sized
// block at the beginning that is preallocated (and cannot be allocated or freed)
//
// We can allocate blocks of a particular size at a particular location.
// We can free blocks.
// We can determine the size of a block.
#define MAX_BYTE 0xffffffffffffffff
class tree_block_allocator: public block_allocator {
public:

    // Effect: Create a tree block allocator, in which the first RESERVE_AT_BEGINNING bytes are not put into a block.
    // Tree block allocation only uses first fit for now
    //  All blocks be start on a multiple of ALIGNMENT.
    //  Aborts if we run out of memory.
    // Parameters
    //  reserve_at_beginning (IN)        Size of reserved block at beginning.  This size does not have to be aligned.
    //  alignment (IN)                   Block alignment.
    void create(uint64_t reserve_at_beginning, uint64_t alignment);

    // Effect: Create a tree block allocator, in which the first RESERVE_AT_BEGINNING bytes are not put into a block.
    //         The allocation strategy is first fit (BA_STRATEGY_FIRST_FIT)
    //         The allocator is initialized to contain `n_blocks' of blockpairs, taken from `pairs'
    //  All blocks be start on a multiple of ALIGNMENT.
    //  Aborts if we run out of memory.
    // Parameters
    //  pairs,                           unowned array of pairs to copy
    //  n_blocks,                        Size of pairs array
    //  reserve_at_beginning (IN)        Size of reserved block at beginning.  This size does not have to be aligned.
    //  alignment (IN)                   Block alignment.
    void create_from_blockpairs(uint64_t reserve_at_beginning, uint64_t alignment,
                                struct blockpair *pairs, uint64_t n_blocks);

    // Effect: Destroy the tree block allocator
    void destroy();


    // Effect: Allocate a block of the specified size at an address chosen by the allocator.
    //  Aborts if anything goes wrong.
    //  The block address will be a multiple of the alignment.
    // Parameters:
    //  size (IN):    The size of the block.  (The size does not have to be aligned.)
    //  offset (OUT): The location of the block.
    //  heat (IN):    A higher heat means we should be prepared to free this block soon (perhaps in the next checkpoint)
    //                Heat values are lexiographically ordered (like integers), but their specific values are arbitrary
    void alloc_block(uint64_t size, uint64_t heat, uint64_t *offset);

    // Effect: Free the block at offset.
    // Requires: There must be a block currently allocated at that offset.
    // Parameters:
    //  offset (IN): The offset of the block.
    // size(IN) : The size of the block
    void free_block(uint64_t offset, uint64_t size);

    // Effect: Check to see if the block allocator is OK.  This may take a long time.
    // Usage Hints: Probably only use this for unit tests.
    // TODO: Private?
    void validate() const;

    // Effect: Return the unallocated block address of "infinite" size.
    //  That is, return the smallest address that is above all the allocated blocks.
    uint64_t allocated_limit() const;

    // Effect: Consider the blocks in sorted order.  The reserved block at the beginning is number 0.  The next one is number 1 and so forth.
    //  Return the offset and size of the block with that number.
    //  Return 0 if there is a block that big, return nonzero if b is too big.
    // Rationale: This is probably useful only for tests.
    int get_nth_block_in_layout_order(uint64_t b, uint64_t *offset, uint64_t *size);

    // Effect:  Fill in report to indicate how the file is used.
    // Requires: 
    //  report->file_size_bytes is filled in
    //  report->data_bytes is filled in
    //  report->checkpoint_bytes_additional is filled in
    void get_unused_statistics(TOKU_DB_FRAGMENTATION report);

    // Effect: Fill in report->data_bytes with the number of bytes in use
    //         Fill in report->data_blocks with the number of blockpairs in use
    //         Fill in unused statistics using this->get_unused_statistics()
    // Requires:
    //  report->file_size is ignored on return
    //  report->checkpoint_bytes_additional is ignored on return
    void get_statistics(TOKU_DB_FRAGMENTATION report);

    void set_strategy(enum allocation_strategy strategy) ;
private:
    void _create_internal(uint64_t reserve_at_beginning, uint64_t alignment);

    // Tracing
    toku_mutex_t _trace_lock;
    void _trace_create(void);
    void _trace_create_from_blockpairs(void);
    void _trace_destroy(void);
    void _trace_alloc(uint64_t size, uint64_t heat, uint64_t offset);
    void _trace_free(uint64_t offset);

    // How much to reserve at the beginning
    uint64_t _reserve_at_beginning;
    // Block alignment
    uint64_t _alignment;
    // How many blocks
    uint64_t _n_blocks;
    uint64_t _n_bytes_in_use;

    // These blocks are sorted by address.
//    struct blockpair *_blocks_array;
    rbtree_mhs * _tree;
};