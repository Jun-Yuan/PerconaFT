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

#include <algorithm>

#include <string.h>

#include "portability/memory.h"
#include "portability/toku_assert.h"
#include "portability/toku_stdint.h"
#include "portability/toku_stdlib.h"

#include "ft/serialize/block_allocator.h"
#include "ft/serialize/array_block_allocator.h"
#include "ft/serialize/block_allocator_strategy.h"

#if TOKU_DEBUG_PARANOID
#define VALIDATE() validate()
#else
#define VALIDATE()
#endif

extern FILE *ba_trace_file;

void array_block_allocator::_create_internal(uint64_t reserve_at_beginning, uint64_t alignment) {
    // the alignment must be at least 512 and aligned with 512 to work with direct I/O
    assert(alignment >= 512 && (alignment % 512) == 0);

    _reserve_at_beginning = reserve_at_beginning;
    _alignment = alignment;
    _n_blocks = 0;
    _blocks_array_size = 1;
    XMALLOC_N(_blocks_array_size, _blocks_array);
    _n_bytes_in_use = reserve_at_beginning;
    set_strategy(BA_STRATEGY_FIRST_FIT);

    memset(&_trace_lock, 0, sizeof(toku_mutex_t));
    toku_mutex_init(&_trace_lock, nullptr);

    VALIDATE();
}

void array_block_allocator::create(uint64_t reserve_at_beginning, uint64_t alignment) {
    _create_internal(reserve_at_beginning, alignment);
    _trace_create();
}

void array_block_allocator::destroy() {
    toku_free(_blocks_array);
    _trace_destroy();
    toku_mutex_destroy(&_trace_lock);
}


void array_block_allocator::grow_blocks_array_by(uint64_t n_to_add) {
    if (_n_blocks + n_to_add > _blocks_array_size) {
        uint64_t new_size = _n_blocks + n_to_add;
        uint64_t at_least = _blocks_array_size * 2;
        if (at_least > new_size) {
            new_size = at_least;
        }
        _blocks_array_size = new_size;
        XREALLOC_N(_blocks_array_size, _blocks_array);
    }
}

void array_block_allocator::grow_blocks_array() {
    grow_blocks_array_by(1);
}

void array_block_allocator::create_from_blockpairs(uint64_t reserve_at_beginning, uint64_t alignment,
                                             struct blockpair *pairs, uint64_t n_blocks) {
    _create_internal(reserve_at_beginning, alignment);

    _n_blocks = n_blocks;
    grow_blocks_array_by(_n_blocks);
    memcpy(_blocks_array, pairs, _n_blocks * sizeof(struct blockpair));
    std::sort(_blocks_array, _blocks_array + _n_blocks);
    for (uint64_t i = 0; i < _n_blocks; i++) {
        // Allocator does not support size 0 blocks. See block_allocator_free_block.
        invariant(_blocks_array[i].size > 0);
        invariant(_blocks_array[i].offset >= _reserve_at_beginning);
        invariant(_blocks_array[i].offset % _alignment == 0);

        _n_bytes_in_use += _blocks_array[i].size;
    }

    VALIDATE();

    _trace_create_from_blockpairs();
}

// Effect: align a value by rounding up.
static inline uint64_t align(uint64_t value, uint64_t ba_alignment) {
    return ((value + ba_alignment - 1) / ba_alignment) * ba_alignment;
}

struct block_allocator::blockpair *
array_block_allocator::choose_block_to_alloc_after(size_t size, uint64_t heat) {
    switch (get_strategy()) {
    case BA_STRATEGY_FIRST_FIT:
        return block_allocator_strategy::first_fit(_blocks_array, _n_blocks, size, _alignment);
    case BA_STRATEGY_BEST_FIT:
        return block_allocator_strategy::best_fit(_blocks_array, _n_blocks, size, _alignment);
    case BA_STRATEGY_HEAT_ZONE:
        return block_allocator_strategy::heat_zone(_blocks_array, _n_blocks, size, _alignment, heat);
    case BA_STRATEGY_PADDED_FIT:
        return block_allocator_strategy::padded_fit(_blocks_array, _n_blocks, size, _alignment);
    default:
        abort();
    }
}

// Effect: Allocate a block. The resulting block must be aligned on the ba->alignment (which to make direct_io happy must be a positive multiple of 512).
void array_block_allocator::alloc_block(uint64_t size, uint64_t heat, uint64_t *offset) {
    struct blockpair *bp;

    // Allocator does not support size 0 blocks. See block_allocator_free_block.
    invariant(size > 0);

    grow_blocks_array();
    _n_bytes_in_use += size;

    uint64_t end_of_reserve = align(_reserve_at_beginning, _alignment);

    if (_n_blocks == 0) {
        // First and only block
        assert(_n_bytes_in_use == _reserve_at_beginning + size); // we know exactly how many are in use
        _blocks_array[0].offset = align(_reserve_at_beginning, _alignment);
        _blocks_array[0].size = size;
        *offset = _blocks_array[0].offset;
        goto done;
    } else if (end_of_reserve + size <= _blocks_array[0].offset ) {
        // Check to see if the space immediately after the reserve is big enough to hold the new block.
        bp = &_blocks_array[0];
        memmove(bp + 1, bp, _n_blocks * sizeof(*bp));
        bp[0].offset = end_of_reserve;
        bp[0].size = size;
        *offset = end_of_reserve;
        goto done;
    }

    bp = choose_block_to_alloc_after(size, heat);
    if (bp != nullptr) {
        // our allocation strategy chose the space after `bp' to fit the new block
        uint64_t answer_offset = align(bp->offset + bp->size, _alignment);
        uint64_t blocknum = bp - _blocks_array;
        invariant(&_blocks_array[blocknum] == bp);
        invariant(blocknum < _n_blocks);
        memmove(bp + 2, bp + 1, (_n_blocks - blocknum - 1) * sizeof(*bp));
        bp[1].offset = answer_offset;
        bp[1].size = size;
        *offset = answer_offset;
    } else {
        // It didn't fit anywhere, so fit it on the end.
        assert(_n_blocks < _blocks_array_size);
        bp = &_blocks_array[_n_blocks];
        uint64_t answer_offset = align(bp[-1].offset + bp[-1].size, _alignment);
        bp->offset = answer_offset;
        bp->size = size;
        *offset = answer_offset;
    }

done:
    _n_blocks++;
    VALIDATE();

    _trace_alloc(size, heat, *offset);
}

// Find the index in the blocks array that has a particular offset.  Requires that the block exist.
// Use binary search so it runs fast.
int64_t array_block_allocator::find_block(uint64_t offset) {
    VALIDATE();
    if (_n_blocks == 1) {
        assert(_blocks_array[0].offset == offset);
        return 0;
    }

    uint64_t lo = 0;
    uint64_t hi = _n_blocks;
    while (1) {
        assert(lo < hi); // otherwise no such block exists.
        uint64_t mid = (lo + hi) / 2;
        uint64_t thisoff = _blocks_array[mid].offset;
        if (thisoff < offset) {
            lo = mid + 1;
        } else if (thisoff > offset) {
            hi = mid;
        } else {
            return mid;
        }
    }
}

// To support 0-sized blocks, we need to include size as an input to this function.
// All 0-sized blocks at the same offset can be considered identical, but
// a 0-sized block can share offset with a non-zero sized block.
// The non-zero sized block is not exchangable with a zero sized block (or vice versa),
// so inserting 0-sized blocks can cause corruption here.
void array_block_allocator::free_block(uint64_t offset, uint64_t UU(size)) {
    VALIDATE();
    int64_t bn = find_block(offset);
    assert(bn >= 0); // we require that there is a block with that offset.
    _n_bytes_in_use -= _blocks_array[bn].size;
    memmove(&_blocks_array[bn], &_blocks_array[bn + 1],
            (_n_blocks - bn - 1) * sizeof(struct blockpair));
    _n_blocks--;
    VALIDATE();
    
    _trace_free(offset);
}

uint64_t array_block_allocator::block_size(uint64_t offset) {
    int64_t bn = find_block(offset);
    assert(bn >=0); // we require that there is a block with that offset.
    return _blocks_array[bn].size;
}

uint64_t array_block_allocator::allocated_limit() const {
    if (_n_blocks == 0) {
        return _reserve_at_beginning;
    } else {
        struct blockpair *last = &_blocks_array[_n_blocks - 1];
        return last->offset + last->size;
    }
}

// Effect: Consider the blocks in sorted order.  The reserved block at the beginning is number 0.  The next one is number 1 and so forth.
// Return the offset and size of the block with that number.
// Return 0 if there is a block that big, return nonzero if b is too big.
int array_block_allocator::get_nth_block_in_layout_order(uint64_t b, uint64_t *offset, uint64_t *size) {
    if (b ==0 ) {
        *offset = 0;
        *size = _reserve_at_beginning;
        return  0;
    } else if (b > _n_blocks) {
        return -1;
    } else {
        *offset =_blocks_array[b - 1].offset;
        *size =_blocks_array[b - 1].size;
        return 0;
    }
}

// Requires: report->file_size_bytes is filled in
// Requires: report->data_bytes is filled in
// Requires: report->checkpoint_bytes_additional is filled in
void array_block_allocator::get_unused_statistics(TOKU_DB_FRAGMENTATION report) {
    assert(_n_bytes_in_use == report->data_bytes + report->checkpoint_bytes_additional);

    report->unused_bytes = 0;
    report->unused_blocks = 0;
    report->largest_unused_block = 0;
    if (_n_blocks > 0) {
        //Deal with space before block 0 and after reserve:
        {
            struct blockpair *bp = &_blocks_array[0];
            assert(bp->offset >= align(_reserve_at_beginning, _alignment));
            uint64_t free_space = bp->offset - align(_reserve_at_beginning, _alignment);
            if (free_space > 0) {
                report->unused_bytes += free_space;
                report->unused_blocks++;
                if (free_space > report->largest_unused_block) {
                    report->largest_unused_block = free_space;
                }
            }
        }

        //Deal with space between blocks:
        for (uint64_t blocknum = 0; blocknum +1 < _n_blocks; blocknum ++) {
            // Consider the space after blocknum
            struct blockpair *bp = &_blocks_array[blocknum];
            uint64_t this_offset = bp[0].offset;
            uint64_t this_size   = bp[0].size;
            uint64_t end_of_this_block = align(this_offset+this_size, _alignment);
            uint64_t next_offset = bp[1].offset;
            uint64_t free_space  = next_offset - end_of_this_block;
            if (free_space > 0) {
                report->unused_bytes += free_space;
                report->unused_blocks++;
                if (free_space > report->largest_unused_block) {
                    report->largest_unused_block = free_space;
                }
            }
        }

        //Deal with space after last block
        {
            struct blockpair *bp = &_blocks_array[_n_blocks-1];
            uint64_t this_offset = bp[0].offset;
            uint64_t this_size   = bp[0].size;
            uint64_t end_of_this_block = align(this_offset+this_size, _alignment);
            if (end_of_this_block < report->file_size_bytes) {
                uint64_t free_space  = report->file_size_bytes - end_of_this_block;
                assert(free_space > 0);
                report->unused_bytes += free_space;
                report->unused_blocks++;
                if (free_space > report->largest_unused_block) {
                    report->largest_unused_block = free_space;
                }
            }
        }
    } else {
        // No blocks.  Just the reserve.
        uint64_t end_of_this_block = align(_reserve_at_beginning, _alignment);
        if (end_of_this_block < report->file_size_bytes) {
            uint64_t free_space  = report->file_size_bytes - end_of_this_block;
            assert(free_space > 0);
            report->unused_bytes += free_space;
            report->unused_blocks++;
            if (free_space > report->largest_unused_block) {
                report->largest_unused_block = free_space;
            }
        }
    }
}

void array_block_allocator::get_statistics(TOKU_DB_FRAGMENTATION report) {
    report->data_bytes = _n_bytes_in_use; 
    report->data_blocks = _n_blocks; 
    report->file_size_bytes = 0;
    report->checkpoint_bytes_additional = 0;
    get_unused_statistics(report);
}

void array_block_allocator::validate() const {
    uint64_t n_bytes_in_use = _reserve_at_beginning;
    for (uint64_t i = 0; i < _n_blocks; i++) {
        n_bytes_in_use += _blocks_array[i].size;
        if (i > 0) {
            assert(_blocks_array[i].offset >  _blocks_array[i - 1].offset);
            assert(_blocks_array[i].offset >= _blocks_array[i - 1].offset + _blocks_array[i - 1].size );
        }
    }
    assert(n_bytes_in_use == _n_bytes_in_use);
}

// Tracing

void array_block_allocator::_trace_create(void) {
    if (ba_trace_file != nullptr) {
        toku_mutex_lock(&_trace_lock);
        fprintf(ba_trace_file, "ba_trace_create %p %" PRIu64 " %" PRIu64 "\n",
                this, _reserve_at_beginning, _alignment);
        toku_mutex_unlock(&_trace_lock);

        fflush(ba_trace_file);
    }
}

void array_block_allocator::_trace_create_from_blockpairs(void) {
    if (ba_trace_file != nullptr) {
        toku_mutex_lock(&_trace_lock);
        fprintf(ba_trace_file, "ba_trace_create_from_blockpairs %p %" PRIu64 " %" PRIu64 " ",
                this, _reserve_at_beginning, _alignment);
        for (uint64_t i = 0; i < _n_blocks; i++) {
            fprintf(ba_trace_file, "[%" PRIu64 " %" PRIu64 "] ",
                    _blocks_array[i].offset, _blocks_array[i].size);
        }
        fprintf(ba_trace_file, "\n");
        toku_mutex_unlock(&_trace_lock);

        fflush(ba_trace_file);
    }
}

void array_block_allocator::_trace_destroy(void) {
    if (ba_trace_file != nullptr) {
        toku_mutex_lock(&_trace_lock);
        fprintf(ba_trace_file, "ba_trace_destroy %p\n", this);
        toku_mutex_unlock(&_trace_lock);

        fflush(ba_trace_file);
    }
}

void array_block_allocator::_trace_alloc(uint64_t size, uint64_t heat, uint64_t offset) {
    if (ba_trace_file != nullptr) {
        toku_mutex_lock(&_trace_lock);
        fprintf(ba_trace_file, "ba_trace_alloc %p %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                this, size, heat, offset);
        toku_mutex_unlock(&_trace_lock);

        fflush(ba_trace_file);
    }
}

void array_block_allocator::_trace_free(uint64_t offset) {
    if (ba_trace_file != nullptr) {
        toku_mutex_lock(&_trace_lock);
        fprintf(ba_trace_file, "ba_trace_free %p %" PRIu64 "\n", this, offset);
        toku_mutex_unlock(&_trace_lock);

        fflush(ba_trace_file);
    }
}