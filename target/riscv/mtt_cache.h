// mtt_cache.h
#ifndef MTT_CACHE_H
#define MTT_CACHE_H

#include <stdint.h>
#include <stdbool.h>

// Access types
#define ACCESS_LOAD  0
#define ACCESS_STORE 1
#define ACCESS_FETCH 2

// Permission bits
#define PERM_R 0x4
#define PERM_W 0x2
#define PERM_X 0x1

// Entry encoding
#define MTT_CACHE_VALID_MASK     0x1ULL
#define MTT_CACHE_PERMS_MASK     0x7ULL
#define MTT_CACHE_PERMS_SHIFT    1
#define MTT_CACHE_TAG_SHIFT      4
#define MTT_CACHE_TAG_MASK       0xFFFFFFFFULL  // 32-bit PPN + SDID (combined tag)
#define MTT_CACHE_LRU_SHIFT      36
#define MTT_CACHE_LRU_MASK       0xFFFFFFFULL   // 28-bit LRU tick

#define MTT_CACHE_SIZE 4096
#define MTT_SET_COUNT 1024
#define MTT_WAYS      4

typedef uint64_t MTTCacheEntry;

extern MTTCacheEntry mtt_cache[MTT_SET_COUNT][MTT_WAYS];
extern uint64_t mtt_lru_tick;
extern uint64_t mtt_hits, mtt_misses;

// Encode/Decode helpers
static inline uint64_t mtt_make_tag(uint64_t pa_page) {
    return (pa_page & ~0xFFFULL);
}

static inline uint64_t mtt_extract_tag(MTTCacheEntry e) {
    return (e >> MTT_CACHE_TAG_SHIFT) & MTT_CACHE_TAG_MASK;
}

static inline uint8_t mtt_extract_perms(MTTCacheEntry e) {
    return (e >> MTT_CACHE_PERMS_SHIFT) & MTT_CACHE_PERMS_MASK;
}

static inline bool mtt_is_valid(MTTCacheEntry e) {
    return (e & MTT_CACHE_VALID_MASK);
}

static inline uint64_t mtt_make_entry(uint64_t tag, uint8_t perms, uint64_t lru_tick) {
    return (MTT_CACHE_VALID_MASK |
            ((uint64_t)perms << MTT_CACHE_PERMS_SHIFT) |
            ((uint64_t)tag << MTT_CACHE_TAG_SHIFT) |
            (lru_tick << MTT_CACHE_LRU_SHIFT));
}

// API
void mtt_cache_init(void);
bool mtt_cache_lookup(uint64_t pa, uint8_t sdid, int access_type);
void mtt_cache_insert(uint64_t pa, uint8_t sdid, uint8_t perms);
void mtt_cache_flush(void);
void mtt_cache_invalidate(uint64_t pa_page, uint8_t sdid);
void mtt_cache_dump_stats(void);

#endif // MTT_CACHE_H