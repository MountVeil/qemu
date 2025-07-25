// mtt_cache.c
#include "mtt_cache.h"
#include <stdio.h>
#include <string.h>

MTTCacheEntry mtt_cache[MTT_SET_COUNT][MTT_WAYS];
uint64_t mtt_lru_tick = 0;

uint64_t mtt_hits = 0;
uint64_t mtt_misses = 0;
uint64_t lookup_ns_total = 0;
uint64_t lookup_count = 0;
uint64_t cnt_read = 0;
uint64_t cnt_write = 0;
uint64_t cnt_fetch = 0;

static inline int mtt_hash_index(uint64_t tag) {
    return (tag >> 12) % MTT_SET_COUNT;  // 更合理的 hash
}

void mtt_cache_init(void) {
    memset(mtt_cache, 0, sizeof(mtt_cache));
    mtt_lru_tick = 0;
    mtt_hits = 0;
    mtt_misses = 0;
}

bool mtt_cache_lookup(uint64_t pa, uint8_t sdid, int access_type)
{
    uint64_t pa_page = pa & ~0xFFFULL;
    uint64_t tag = mtt_make_tag(pa_page);
    int set = (tag ^ (tag >> 3)) % MTT_SET_COUNT;   // address scrambling

    for (int i = 0; i < MTT_WAYS; i++) {
        MTTCacheEntry entry = mtt_cache[set][i];

        if (mtt_is_valid(entry) && mtt_extract_tag(entry) == tag) {
            uint8_t perms = mtt_extract_perms(entry);

            if ((access_type == ACCESS_LOAD  && (perms & PERM_R)) ||
                (access_type == ACCESS_STORE && (perms & PERM_W)) ||
                (access_type == ACCESS_FETCH && (perms & PERM_X))) {
                mtt_cache[set][i] = (entry & ~(MTT_CACHE_LRU_MASK << MTT_CACHE_LRU_SHIFT)) |
                (mtt_lru_tick++ << MTT_CACHE_LRU_SHIFT);

                mtt_hits++;
                return true;
            }

            mtt_misses++;
            return false;
        }
    }

    mtt_misses++;
    return false;
}

void mtt_cache_insert(uint64_t pa, uint8_t sdid, uint8_t perms)
{
    MTTCacheEntry entry;
    uint64_t pa_page = pa & ~0xFFFULL;
    uint64_t tag = mtt_make_tag(pa_page);
    int set = (tag ^ (tag >> 3)) % MTT_SET_COUNT;

    int victim = -1;
    uint64_t oldest = (uint64_t)-1;

    for (int i = 0; i < MTT_WAYS; i++) {
        entry = mtt_cache[set][i];

        if (mtt_is_valid(entry) && mtt_extract_tag(entry) == tag) {
            // Merge permissions if already exists
            uint8_t old_perms = mtt_extract_perms(entry);
            uint64_t new_entry = mtt_make_entry(tag, old_perms | perms, ++mtt_lru_tick);
            mtt_cache[set][i] = new_entry;
            return;
        }
    }

    for (int i = 0; i < MTT_WAYS; i++) {
        entry = mtt_cache[set][i];

        // Find victim: invalid or oldest
        uint64_t lru = (entry >> MTT_CACHE_LRU_SHIFT) & MTT_CACHE_LRU_MASK;
        if (!mtt_is_valid(entry)) {
            victim = i;
            break;
        }
        if (lru < oldest) {
            oldest = lru;
            victim = i;
        }
    }

    // Replace victim
    mtt_cache[set][victim] = mtt_make_entry(tag, perms, ++mtt_lru_tick);
}

void mtt_cache_flush(void)
{
    for (int i = 0; i < MTT_SET_COUNT; ++i) {
        for (int j = 0; j < MTT_WAYS; ++j) {
            mtt_cache[i][j] = 0;
        }
    }
}

void mtt_cache_invalidate(uint64_t pa_page, uint8_t sdid)
{
    uint64_t tag = mtt_make_tag(pa_page);

    for (int i = 0; i < MTT_SET_COUNT; ++i) {
        for (int j = 0; j < MTT_WAYS; ++j) {
            MTTCacheEntry entry = mtt_cache[i][j];
            if (mtt_is_valid(entry) && mtt_extract_tag(entry) == tag) {
                mtt_cache[i][j] = 0; // Invalidate
            }
        }
    }
}



void mtt_cache_dump_stats(void) {
    uint64_t total = mtt_hits + mtt_misses;
    double hit_rate = total ? ((double)mtt_hits / total * 100.0) : 0.0;
    printf("[MTT Cache] Hits: %lu, Misses: %lu, Hit Rate: %.2f%%\n",
           mtt_hits, mtt_misses, hit_rate);
}