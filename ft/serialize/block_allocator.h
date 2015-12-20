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

// Block allocator.
//
// A block allocator manages the allocation of variable-sized blocks.
// The translation of block numbers to addresses is handled elsewhere.
// The allocation of block numbers is handled elsewhere.
//
// When creating a block allocator we also specify a certain-sized
// block at the beginning that is preallocated (and cannot be allocated or freed)
//
// We can allocate blocks of a particular size at a particular location.
// We can allocate blocks of a particular size at a location chosen by the allocator.
// We can free blocks.
// We can determine the size of a block.

class block_allocator {
public:
    static const size_t BLOCK_ALLOCATOR_ALIGNMENT = 4096;

    // How much must be reserved at the beginning for the block?
    //  The actual header is 8+4+4+8+8_4+8+ the length of the db names + 1 pointer for each root.
    //  So 4096 should be enough.
    static const size_t BLOCK_ALLOCATOR_HEADER_RESERVE = 4096;
    
    static_assert(BLOCK_ALLOCATOR_HEADER_RESERVE % BLOCK_ALLOCATOR_ALIGNMENT == 0,
                  "block allocator header must have proper alignment");

    static const size_t BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE = BLOCK_ALLOCATOR_HEADER_RESERVE * 2;

    struct blockpair {
        uint64_t offset;
        uint64_t size;
        blockpair(uint64_t o, uint64_t s) :
            offset(o), size(s) {
        }
        int operator<(const struct blockpair &rhs) const {
            return offset < rhs.offset;
        }
        int operator<(const uint64_t &o) const {
            return offset < o;
        }
    };

    // Effect: Create a block allocator, in which the first RESERVE_AT_BEGINNING bytes are not put into a block.
    //         The default allocation strategy is first fit (BA_STRATEGY_FIRST_FIT)
    //  All blocks be start on a multiple of ALIGNMENT.
    //  Aborts if we run out of memory.
    // Parameters
    //  reserve_at_beginning (IN)        Size of reserved block at beginning.  This size does not have to be aligned.
    //  alignment (IN)                   Block alignment.
    virtual void create(uint64_t reserve_at_beginning, uint64_t alignment) = 0;

    // Effect: Create a block allocator, in which the first RESERVE_AT_BEGINNING bytes are not put into a block.
    //         The default allocation strategy is first fit (BA_STRATEGY_FIRST_FIT)
    //         The allocator is initialized to contain `n_blocks' of blockpairs, taken from `pairs'
    //  All blocks be start on a multiple of ALIGNMENT.
    //  Aborts if we run out of memory.
    // Parameters
    //  pairs,                           unowned array of pairs to copy
    //  n_blocks,                        Size of pairs array
    //  reserve_at_beginning (IN)        Size of reserved block at beginning.  This size does not have to be aligned.
    //  alignment (IN)                   Block alignment.
    virtual void create_from_blockpairs(uint64_t reserve_at_beginning, uint64_t alignment,
                                struct blockpair *pairs, uint64_t n_blocks) = 0;

    // Effect: Destroy this block allocator
    virtual void destroy() = 0;

    // Effect: Allocate a block of the specified size at an address chosen by the allocator.
    //  Aborts if anything goes wrong.
    //  The block address will be a multiple of the alignment.
    // Parameters:
    //  size (IN):    The size of the block.  (The size does not have to be aligned.)
    //  offset (OUT): The location of the block.
    //  heat (IN):    A higher heat means we should be prepared to free this block soon (perhaps in the next checkpoint)
    //                Heat values are lexiographically ordered (like integers), but their specific values are arbitrary
    virtual void alloc_block(uint64_t size, uint64_t heat, uint64_t *offset) = 0;

    // Effect: Free the block at offset.
    // Requires: There must be a block currently allocated at that offset.
    // Parameters:
    //  offset (IN): The offset of the block.
    virtual void free_block(uint64_t offset) = 0;

    // Effect: Return the size of the block that starts at offset.
    // Requires: There must be a block currently allocated at that offset.
    // Parameters:
    //  offset (IN): The offset of the block.
    virtual uint64_t block_size(uint64_t offset) = 0;

    // Effect: Check to see if the block allocator is OK.  This may take a long time.
    // Usage Hints: Probably only use this for unit tests.
    // TODO: Private?
    virtual void validate() const;

    // Effect: Return the unallocated block address of "infinite" size.
    //  That is, return the smallest address that is above all the allocated blocks.
    virtual uint64_t allocated_limit() const;

    // Effect: Consider the blocks in sorted order.  The reserved block at the beginning is number 0.  The next one is number 1 and so forth.
    //  Return the offset and size of the block with that number.
    //  Return 0 if there is a block that big, return nonzero if b is too big.
    // Rationale: This is probably useful only for tests.
    virtual int get_nth_block_in_layout_order(uint64_t b, uint64_t *offset,
                                              uint64_t *size) = 0;

    // Effect:  Fill in report to indicate how the file is used.
    // Requires: 
    //  report->file_size_bytes is filled in
    //  report->data_bytes is filled in
    //  report->checkpoint_bytes_additional is filled in
    virtual void get_unused_statistics(TOKU_DB_FRAGMENTATION report) = 0;

    // Effect: Fill in report->data_bytes with the number of bytes in use
    //         Fill in report->data_blocks with the number of blockpairs in use
    //         Fill in unused statistics using this->get_unused_statistics()
    // Requires:
    //  report->file_size is ignored on return
    //  report->checkpoint_bytes_additional is ignored on return
    virtual void get_statistics(TOKU_DB_FRAGMENTATION report) = 0;

  
};
