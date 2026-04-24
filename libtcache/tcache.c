#include "tcache.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_LINE_SIZE 64

#define L1I_LINE_COUNT (HW11_L1_SIZE / CACHE_LINE_SIZE)
#define L1D_LINE_COUNT (HW11_L1_SIZE / CACHE_LINE_SIZE)
#define L2_LINE_COUNT  (HW11_L2_SIZE / CACHE_LINE_SIZE)

typedef struct {
    cache_line_t *entries;
    uint64_t *recent;
    size_t set_count;
    size_t ways;
    cache_stats_t stats;
} cache_store_t;

static replacement_policy_e active_policy;
static uint64_t lru_clock;

static cache_line_t l1i_entries[L1I_LINE_COUNT];
static cache_line_t l1d_entries[L1D_LINE_COUNT];
static cache_line_t l2_entries[L2_LINE_COUNT];

static uint64_t l1i_recent[L1I_LINE_COUNT];
static uint64_t l1d_recent[L1D_LINE_COUNT];
static uint64_t l2_recent[L2_LINE_COUNT];

static cache_store_t l1i;
static cache_store_t l1d;
static cache_store_t l2;

static size_t calc_set_count(size_t cache_size, size_t assoc) {
    return cache_size / (CACHE_LINE_SIZE * assoc);
}

static void setup_cache(cache_store_t *c, cache_line_t *entries, uint64_t *recent,
                        size_t cache_size, size_t assoc) {
    c->entries = entries;
    c->recent = recent;
    c->set_count = calc_set_count(cache_size, assoc);
    c->ways = assoc;
    c->stats.accesses = 0;
    c->stats.misses = 0;
}

static uint64_t block_number(uint64_t addr) {
    return addr >> 6;
}

static uint64_t block_start(uint64_t addr) {
    return addr & ~((uint64_t)CACHE_LINE_SIZE - 1);
}

static size_t block_offset(uint64_t addr) {
    return (size_t)(addr & (CACHE_LINE_SIZE - 1));
}

static size_t set_index(const cache_store_t *c, uint64_t addr) {
    return block_number(addr) % c->set_count;
}

static uint64_t tag_for_addr(const cache_store_t *c, uint64_t addr) {
    return block_number(addr) / c->set_count;
}

static cache_line_t *set_begin(cache_store_t *c, size_t idx) {
    return &c->entries[idx * c->ways];
}

static uint64_t *set_recent_begin(cache_store_t *c, size_t idx) {
    return &c->recent[idx * c->ways];
}

static cache_line_t *lookup_line(cache_store_t *c, uint64_t addr) {
    size_t idx = set_index(c, addr);
    uint64_t tag = tag_for_addr(c, addr);
    cache_line_t *bucket = set_begin(c, idx);

    for (size_t i = 0; i < c->ways; i++) {
        if (bucket[i].valid && bucket[i].tag == tag) {
            return &bucket[i];
        }
    }

    return NULL;
}

static void mark_used(cache_store_t *c, cache_line_t *entry) {
    size_t pos = (size_t)(entry - c->entries);
    c->recent[pos] = ++lru_clock;
}

static uint64_t entry_base_addr(cache_store_t *c, cache_line_t *entry) {
    size_t pos = (size_t)(entry - c->entries);
    size_t idx = pos / c->ways;
    uint64_t blk = (entry->tag * c->set_count) + idx;
    return blk * CACHE_LINE_SIZE;
}

static void clear_entry(cache_store_t *c, cache_line_t *entry) {
    size_t pos = (size_t)(entry - c->entries);
    memset(entry, 0, sizeof(*entry));
    c->recent[pos] = 0;
}

static void store_block_to_mem(uint64_t base, const uint8_t data[CACHE_LINE_SIZE]) {
    for (size_t i = 0; i < CACHE_LINE_SIZE; i++) {
        write_memory(base + i, data[i]);
    }
}

static void load_block_from_mem(uint64_t base, uint8_t data[CACHE_LINE_SIZE]) {
    for (size_t i = 0; i < CACHE_LINE_SIZE; i++) {
        data[i] = read_memory(base + i);
    }
}

static cache_line_t *choose_victim(cache_store_t *c, uint64_t addr) {
    size_t idx = set_index(c, addr);
    cache_line_t *bucket = set_begin(c, idx);
    uint64_t *recent = set_recent_begin(c, idx);

    for (size_t i = 0; i < c->ways; i++) {
        if (!bucket[i].valid) {
            return &bucket[i];
        }
    }

    if (active_policy == RANDOM) {
        return &bucket[rand() % c->ways];
    }

    size_t victim = 0;
    for (size_t i = 1; i < c->ways; i++) {
        if (recent[i] < recent[victim]) {
            victim = i;
        }
    }

    return &bucket[victim];
}

static void evict_line(cache_store_t *c, cache_line_t *entry);

static cache_line_t *reserve_line(cache_store_t *c, uint64_t addr) {
    cache_line_t *victim = choose_victim(c, addr);

    if (victim->valid) {
        evict_line(c, victim);
    }

    return victim;
}

static cache_line_t *place_block(cache_store_t *c, uint64_t addr,
                                 const uint8_t data[CACHE_LINE_SIZE],
                                 uint8_t dirty) {
    cache_line_t *entry = reserve_line(c, addr);

    memcpy(entry->data, data, CACHE_LINE_SIZE);
    entry->tag = tag_for_addr(c, addr);
    entry->valid = 1;
    entry->modified = dirty;
    mark_used(c, entry);

    return entry;
}

static void push_block_to_l2(uint64_t addr, const uint8_t data[CACHE_LINE_SIZE],
                             uint8_t track_stats) {
    cache_line_t *entry = lookup_line(&l2, addr);

    if (track_stats) {
        l2.stats.accesses++;
    }

    if (entry == NULL) {
        if (track_stats) {
            l2.stats.misses++;
        }
        entry = reserve_line(&l2, addr);
    }

    memcpy(entry->data, data, CACHE_LINE_SIZE);
    entry->tag = tag_for_addr(&l2, addr);
    entry->valid = 1;
    entry->modified = 1;
    mark_used(&l2, entry);
}

static cache_line_t *find_dirty_l1_line(uint64_t addr) {
    cache_line_t *entry = lookup_line(&l1i, addr);

    if (entry != NULL && entry->modified) {
        return entry;
    }

    entry = lookup_line(&l1d, addr);
    if (entry != NULL && entry->modified) {
        return entry;
    }

    return NULL;
}

static void discard_l1_copy(cache_store_t *c, uint64_t addr) {
    cache_line_t *entry = lookup_line(c, addr);

    if (entry == NULL) {
        return;
    }

    clear_entry(c, entry);
}

static uint8_t flush_l1_dirty_to_mem(cache_store_t *c, uint64_t addr,
                                     uint64_t base) {
    cache_line_t *entry = lookup_line(c, addr);

    if (entry == NULL) {
        return 0;
    }

    if (entry->modified) {
        store_block_to_mem(base, entry->data);
        clear_entry(c, entry);
        return 1;
    }

    clear_entry(c, entry);
    return 0;
}

static void evict_line(cache_store_t *c, cache_line_t *entry) {
    uint64_t base;
    uint8_t used_l1_newer_data = 0;
    cache_line_t *dirty_l1;

    if (!entry->valid) {
        return;
    }

    base = entry_base_addr(c, entry);

    if (c == &l2) {
        dirty_l1 = find_dirty_l1_line(base);

        if (dirty_l1 != NULL) {
            l2.stats.accesses++;
            store_block_to_mem(base, dirty_l1->data);
            dirty_l1->modified = 0;
            used_l1_newer_data = 1;
        }

        used_l1_newer_data |= flush_l1_dirty_to_mem(&l1i, base, base);
        used_l1_newer_data |= flush_l1_dirty_to_mem(&l1d, base, base);

        if (!used_l1_newer_data && entry->modified) {
            store_block_to_mem(base, entry->data);
        }
    } else if (entry->modified) {
        push_block_to_l2(base, entry->data, 1);
    }

    clear_entry(c, entry);
}

static cache_line_t *load_l2_block(uint64_t addr, uint8_t track_stats) {
    cache_line_t *entry;
    uint8_t block[CACHE_LINE_SIZE];

    if (track_stats) {
        l2.stats.accesses++;
    }

    entry = lookup_line(&l2, addr);

    if (entry != NULL) {
        mark_used(&l2, entry);
        return entry;
    }

    if (track_stats) {
        l2.stats.misses++;
    }

    load_block_from_mem(block_start(addr), block);
    return place_block(&l2, addr, block, 0);
}

static cache_store_t *other_l1(cache_store_t *c) {
    if (c == &l1i) {
        return &l1d;
    }

    return &l1i;
}

static void write_dirty_peer_to_l2(cache_store_t *requester, uint64_t addr) {
    cache_store_t *peer = other_l1(requester);
    cache_line_t *peer_entry = lookup_line(peer, addr);

    if (peer_entry == NULL || !peer_entry->modified) {
        return;
    }

    push_block_to_l2(block_start(addr), peer_entry->data, 1);
    peer_entry->modified = 0;
    discard_l1_copy(requester, addr);
}

static void invalidate_peer_copy(cache_store_t *owner, uint64_t addr) {
    cache_store_t *peer = other_l1(owner);
    cache_line_t *peer_entry = lookup_line(peer, addr);

    if (peer_entry == NULL) {
        return;
    }

    discard_l1_copy(peer, addr);
}

void init_cache(replacement_policy_e policy) {
    active_policy = policy;
    lru_clock = 0;
    srand(0);

    memset(l1i_entries, 0, sizeof(l1i_entries));
    memset(l1d_entries, 0, sizeof(l1d_entries));
    memset(l2_entries, 0, sizeof(l2_entries));

    memset(l1i_recent, 0, sizeof(l1i_recent));
    memset(l1d_recent, 0, sizeof(l1d_recent));
    memset(l2_recent, 0, sizeof(l2_recent));

    setup_cache(&l1i, l1i_entries, l1i_recent,
                HW11_L1_SIZE, HW11_L1_INSTR_ASSOC);

    setup_cache(&l1d, l1d_entries, l1d_recent,
                HW11_L1_SIZE, HW11_L1_DATA_ASSOC);

    setup_cache(&l2, l2_entries, l2_recent,
                HW11_L2_SIZE, HW11_L2_ASSOC);
}

uint8_t read_cache(uint64_t mem_addr, mem_type_t type) {
    cache_store_t *l1 = (type == DATA) ? &l1d : &l1i;
    cache_line_t *entry;
    cache_line_t *l2_entry;
    size_t offset = block_offset(mem_addr);

    write_dirty_peer_to_l2(l1, mem_addr);

    l1->stats.accesses++;
    entry = lookup_line(l1, mem_addr);

    if (entry != NULL) {
        mark_used(l1, entry);
        return entry->data[offset];
    }

    l1->stats.misses++;
    l2_entry = load_l2_block(mem_addr, 1);
    entry = place_block(l1, mem_addr, l2_entry->data, 0);

    return entry->data[offset];
}

void write_cache(uint64_t mem_addr, uint8_t value, mem_type_t type) {
    cache_store_t *l1 = (type == DATA) ? &l1d : &l1i;
    cache_line_t *entry;
    cache_line_t *l2_entry;
    size_t offset = block_offset(mem_addr);

    write_dirty_peer_to_l2(l1, mem_addr);

    l1->stats.accesses++;
    entry = lookup_line(l1, mem_addr);

    if (entry == NULL) {
        l1->stats.misses++;
        l2_entry = load_l2_block(mem_addr, 1);
        entry = place_block(l1, mem_addr, l2_entry->data, 0);
    }

    entry->data[offset] = value;
    entry->modified = 1;
    mark_used(l1, entry);

    invalidate_peer_copy(l1, mem_addr);
}

cache_stats_t get_l1_instr_stats() {
    return l1i.stats;
}

cache_stats_t get_l1_data_stats() {
    return l1d.stats;
}

cache_stats_t get_l2_stats() {
    return l2.stats;
}

cache_line_t* get_l1_instr_cache_line(uint64_t mem_addr) {
    return lookup_line(&l1i, mem_addr);
}

cache_line_t* get_l1_data_cache_line(uint64_t mem_addr) {
    return lookup_line(&l1d, mem_addr);
}

cache_line_t* get_l2_cache_line(uint64_t mem_addr) {
    return lookup_line(&l2, mem_addr);
}