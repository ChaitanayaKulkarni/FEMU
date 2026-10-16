/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FEMU_CDL_BITMAP_H
#define FEMU_CDL_BITMAP_H

#include <assert.h>
#include <stdint.h>

/*
 * One 64-bit summary and 64 64-bit leaf words cover at most 4096 IDs. Bit zero
 * is reserved by CDL's callers. The caller records the exact number of valid
 * bits and allocates ceil(nbits / 64) leaf words once at setup.
 *
 * All operations require the scheduler lock (or a quiescent controller).
 */
#define CDL_BITMAP_WORD_BITS 64U
#define CDL_BITMAP_MAX_WORDS CDL_BITMAP_WORD_BITS
#define CDL_BITMAP_CAPACITY  (CDL_BITMAP_WORD_BITS * CDL_BITMAP_MAX_WORDS)

typedef struct CdlBitmap {
    uint64_t summary;
    uint64_t *words;
    unsigned int nbits;
} CdlBitmap;

static inline void cdl_bitmap_set(CdlBitmap *map, unsigned int bit)
{
    unsigned int word;

    assert(bit < map->nbits && map->nbits <= CDL_BITMAP_CAPACITY);
    word = bit / CDL_BITMAP_WORD_BITS;
    map->words[word] |= UINT64_C(1) << (bit % CDL_BITMAP_WORD_BITS);
    map->summary |= UINT64_C(1) << word;
}

static inline void cdl_bitmap_clear(CdlBitmap *map, unsigned int bit)
{
    unsigned int word;

    assert(bit < map->nbits && map->nbits <= CDL_BITMAP_CAPACITY);
    word = bit / CDL_BITMAP_WORD_BITS;
    map->words[word] &= ~(UINT64_C(1) << (bit % CDL_BITMAP_WORD_BITS));
    if (!map->words[word]) {
        map->summary &= ~(UINT64_C(1) << word);
    }
}

/* Return the first ID >= start, or zero. Never evaluate CTZ on zero and never
 * read a leaf unless its summary bit is set, including the final partial leaf.
 */
static inline unsigned int cdl_bitmap_find(const CdlBitmap *map,
                                            unsigned int start)
{
    unsigned int word;
    uint64_t summary, bits;

    assert(map->nbits <= CDL_BITMAP_CAPACITY);
    if (start >= map->nbits) {
        return 0;
    }
    word = start / CDL_BITMAP_WORD_BITS;
    if (!(map->summary >> word)) {
        return 0;
    }
    bits = (map->summary & (UINT64_C(1) << word)) ?
           map->words[word] &
           (UINT64_MAX << (start % CDL_BITMAP_WORD_BITS)) : 0;
    if (!bits) {
        summary = word + 1 < CDL_BITMAP_MAX_WORDS ?
                  map->summary & (UINT64_MAX << (word + 1)) : 0;
        if (!summary) {
            return 0;
        }
        word = __builtin_ctzll(summary);
        bits = map->words[word];
    }
    return word * CDL_BITMAP_WORD_BITS + __builtin_ctzll(bits);
}

static inline unsigned int cdl_bitmap_next(const CdlBitmap *map,
                                            unsigned int start)
{
    unsigned int bit = cdl_bitmap_find(map, start);

    return bit ? bit : cdl_bitmap_find(map, 1);
}

#endif
