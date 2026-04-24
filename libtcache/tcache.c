#include "tcache.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE 64U
#define OFFSET_BITS 6U

#define L1_INSTR_SETS (HW11_L1_SIZE / (LINE_SIZE * HW11_L1_INSTR_ASSOC))
#define L1_DATA_SETS (HW11_L1_SIZE / (LINE_SIZE * HW11_L1_DATA_ASSOC))
#define L2_SETS (HW11_L2_SIZE / (LINE_SIZE * HW11_L2_ASSOC))

typedef struct {
    cache_line_t *lines;
    uint64_t *lru;
    size_t sets;
    size_t assoc;
    cache_stats_t *stats;
    uint64_t *clock;
} cache_desc_t;

static cache_line_t l1_instr_lines[L1_INSTR_SETS][HW11_L1_INSTR_ASSOC];
static cache_line_t l1_data_lines[L1_DATA_SETS][HW11_L1_DATA_ASSOC];
static cache_line_t l2_lines[L2_SETS][HW11_L2_ASSOC];

static uint64_t l1_instr_lru[L1_INSTR_SETS][HW11_L1_INSTR_ASSOC];
static uint64_t l1_data_lru[L1_DATA_SETS][HW11_L1_DATA_ASSOC];
static uint64_t l2_lru[L2_SETS][HW11_L2_ASSOC];

static cache_stats_t l1_instr_stats;
static cache_stats_t l1_data_stats;
static cache_stats_t l2_stats;

static uint64_t l1_instr_clock;
static uint64_t l1_data_clock;
static uint64_t l2_clock;

static replacement_policy_e active_policy;
static uint32_t random_state;

static cache_desc_t l1_instr_cache = {
    &l1_instr_lines[0][0],
    &l1_instr_lru[0][0],
    L1_INSTR_SETS,
    HW11_L1_INSTR_ASSOC,
    &l1_instr_stats,
    &l1_instr_clock
};

static cache_desc_t l1_data_cache = {
    &l1_data_lines[0][0],
    &l1_data_lru[0][0],
    L1_DATA_SETS,
    HW11_L1_DATA_ASSOC,
    &l1_data_stats,
    &l1_data_clock
};

static cache_desc_t l2_cache = {
    &l2_lines[0][0],
    &l2_lru[0][0],
    L2_SETS,
    HW11_L2_ASSOC,
    &l2_stats,
    &l2_clock
};

static unsigned index_bits(size_t sets) {
    unsigned bits = 0;

    while (sets > 1) {
        ++bits;
        sets >>= 1;
    }

    return bits;
}

static uint64_t line_base(uint64_t mem_addr) {
    return mem_addr & ~(uint64_t)(LINE_SIZE - 1U);
}

static size_t cache_index(uint64_t mem_addr, const cache_desc_t *cache) {
    return (size_t)((mem_addr >> OFFSET_BITS) & (cache->sets - 1U));
}

static uint64_t cache_tag(uint64_t mem_addr, const cache_desc_t *cache) {
    return mem_addr >> (OFFSET_BITS + index_bits(cache->sets));
}

static uint64_t compose_line_base(uint64_t tag, size_t index, const cache_desc_t *cache) {
    return ((tag << index_bits(cache->sets)) | (uint64_t)index) << OFFSET_BITS;
}

static cache_line_t *line_at(const cache_desc_t *cache, size_t index, size_t way) {
    return &cache->lines[index * cache->assoc + way];
}

static uint64_t *lru_at(const cache_desc_t *cache, size_t index, size_t way) {
    return &cache->lru[index * cache->assoc + way];
}

static cache_line_t *find_line_in_cache(const cache_desc_t *cache, uint64_t mem_addr) {
    size_t index = cache_index(mem_addr, cache);
    uint64_t tag = cache_tag(mem_addr, cache);
    size_t way;

    for (way = 0; way < cache->assoc; ++way) {
        cache_line_t *line = line_at(cache, index, way);

        if (line->valid && line->tag == tag) {
            return line;
        }
    }

    return NULL;
}

static void touch_way(const cache_desc_t *cache, size_t index, size_t way) {
    *lru_at(cache, index, way) = ++(*cache->clock);
}

static size_t way_for_line(const cache_desc_t *cache, size_t index, const cache_line_t *line) {
    size_t way;

    for (way = 0; way < cache->assoc; ++way) {
        if (line_at(cache, index, way) == line) {
            return way;
        }
    }

    return 0;
}

static uint32_t next_random(void) {
    random_state = random_state * 1103515245U + 12345U;
    return random_state;
}

static size_t choose_victim_way(const cache_desc_t *cache, size_t index) {
    size_t way;
    size_t victim_way = 0;
    uint64_t oldest_lru = UINT64_MAX;

    for (way = 0; way < cache->assoc; ++way) {
        if (!line_at(cache, index, way)->valid) {
            return way;
        }
    }

    if (active_policy == RANDOM && cache->assoc > 1U) {
        return (size_t)(next_random() % cache->assoc);
    }

    for (way = 0; way < cache->assoc; ++way) {
        uint64_t lru = *lru_at(cache, index, way);

        if (lru < oldest_lru) {
            oldest_lru = lru;
            victim_way = way;
        }
    }

    return victim_way;
}

static void read_line_from_memory(uint64_t base_addr, uint8_t data[LINE_SIZE]) {
    size_t byte;

    for (byte = 0; byte < LINE_SIZE; ++byte) {
        data[byte] = read_memory(base_addr + byte);
    }
}

static void write_line_to_memory(uint64_t base_addr, const uint8_t data[LINE_SIZE]) {
    size_t byte;

    for (byte = 0; byte < LINE_SIZE; ++byte) {
        write_memory(base_addr + byte, data[byte]);
    }
}

static void l2_write_line(uint64_t mem_addr, const uint8_t data[LINE_SIZE]);

static void invalidate_l1_line(const cache_desc_t *cache, uint64_t mem_addr) {
    cache_line_t *line = find_line_in_cache(cache, mem_addr);

    if (line != NULL) {
        line->valid = 0;
        line->modified = 0;
    }
}

static void merge_dirty_l1_line_into_l2_victim(const cache_desc_t *l1_cache,
                                               uint64_t mem_addr,
                                               cache_line_t *l2_victim) {
    cache_line_t *line = find_line_in_cache(l1_cache, mem_addr);

    if (line != NULL && line->modified) {
        ++l2_stats.accesses;
        memcpy(l2_victim->data, line->data, LINE_SIZE);
        l2_victim->modified = 1;
        line->modified = 0;
    }
}

static void evict_l2_line_if_needed(cache_line_t *line, size_t index) {
    uint64_t victim_base;

    if (!line->valid) {
        return;
    }

    victim_base = compose_line_base(line->tag, index, &l2_cache);
    merge_dirty_l1_line_into_l2_victim(&l1_instr_cache, victim_base, line);
    merge_dirty_l1_line_into_l2_victim(&l1_data_cache, victim_base, line);
    invalidate_l1_line(&l1_instr_cache, victim_base);
    invalidate_l1_line(&l1_data_cache, victim_base);

    if (line->modified) {
        write_line_to_memory(victim_base, line->data);
    }
}

static cache_line_t *allocate_l2_line(uint64_t mem_addr, int fill_from_memory) {
    size_t index = cache_index(mem_addr, &l2_cache);
    size_t way = choose_victim_way(&l2_cache, index);
    cache_line_t *line = line_at(&l2_cache, index, way);

    evict_l2_line_if_needed(line, index);

    line->valid = 1;
    line->modified = 0;
    line->tag = cache_tag(mem_addr, &l2_cache);

    if (fill_from_memory) {
        read_line_from_memory(line_base(mem_addr), line->data);
    }

    touch_way(&l2_cache, index, way);
    return line;
}

static void l2_read_line(uint64_t mem_addr, uint8_t data[LINE_SIZE]) {
    size_t index = cache_index(mem_addr, &l2_cache);
    cache_line_t *line;
    size_t way;

    ++l2_stats.accesses;
    line = find_line_in_cache(&l2_cache, mem_addr);

    if (line == NULL) {
        ++l2_stats.misses;
        line = allocate_l2_line(mem_addr, 1);
    } else {
        way = way_for_line(&l2_cache, index, line);
        touch_way(&l2_cache, index, way);
    }

    memcpy(data, line->data, LINE_SIZE);
}

static void l2_write_line(uint64_t mem_addr, const uint8_t data[LINE_SIZE]) {
    size_t index = cache_index(mem_addr, &l2_cache);
    cache_line_t *line;
    size_t way;

    ++l2_stats.accesses;
    line = find_line_in_cache(&l2_cache, mem_addr);

    if (line == NULL) {
        ++l2_stats.misses;
        line = allocate_l2_line(mem_addr, 0);
    } else {
        way = way_for_line(&l2_cache, index, line);
        touch_way(&l2_cache, index, way);
    }

    memcpy(line->data, data, LINE_SIZE);
    line->modified = 1;
}

static void write_back_dirty_other_l1(uint64_t mem_addr, mem_type_t requester) {
    cache_desc_t *other_cache = requester == INSTR ? &l1_data_cache : &l1_instr_cache;
    cache_line_t *other_line = find_line_in_cache(other_cache, mem_addr);

    if (other_line != NULL && other_line->modified) {
        l2_write_line(line_base(mem_addr), other_line->data);
        other_line->modified = 0;
    }
}

static void invalidate_other_l1_copy(uint64_t mem_addr, mem_type_t writer) {
    cache_desc_t *other_cache = writer == INSTR ? &l1_data_cache : &l1_instr_cache;

    invalidate_l1_line(other_cache, mem_addr);
}

static void write_back_dirty_l1_victim(const cache_desc_t *cache,
                                       cache_line_t *line,
                                       size_t index) {
    uint64_t victim_base;

    if (!line->valid || !line->modified) {
        return;
    }

    victim_base = compose_line_base(line->tag, index, cache);
    l2_write_line(victim_base, line->data);
    line->modified = 0;
}

static cache_line_t *fill_l1_line(cache_desc_t *cache, uint64_t mem_addr) {
    uint8_t data[LINE_SIZE];
    size_t index = cache_index(mem_addr, cache);
    size_t way = choose_victim_way(cache, index);
    cache_line_t *line = line_at(cache, index, way);

    write_back_dirty_l1_victim(cache, line, index);
    l2_read_line(mem_addr, data);

    line->valid = 1;
    line->modified = 0;
    line->tag = cache_tag(mem_addr, cache);
    memcpy(line->data, data, LINE_SIZE);
    touch_way(cache, index, way);

    return line;
}

static cache_desc_t *cache_for_type(mem_type_t type) {
    return type == INSTR ? &l1_instr_cache : &l1_data_cache;
}

// STUDENT TODO: initialize cache with replacement policy
void init_cache(replacement_policy_e policy) {
    active_policy = policy;
    random_state = 1U;

    memset(l1_instr_lines, 0, sizeof(l1_instr_lines));
    memset(l1_data_lines, 0, sizeof(l1_data_lines));
    memset(l2_lines, 0, sizeof(l2_lines));
    memset(l1_instr_lru, 0, sizeof(l1_instr_lru));
    memset(l1_data_lru, 0, sizeof(l1_data_lru));
    memset(l2_lru, 0, sizeof(l2_lru));

    l1_instr_stats.accesses = 0;
    l1_instr_stats.misses = 0;
    l1_data_stats.accesses = 0;
    l1_data_stats.misses = 0;
    l2_stats.accesses = 0;
    l2_stats.misses = 0;

    l1_instr_clock = 0;
    l1_data_clock = 0;
    l2_clock = 0;
}

// STUDENT TODO: implement read cache, using the l1 and l2 structure
uint8_t read_cache(uint64_t mem_addr, mem_type_t type) {
    cache_desc_t *cache = cache_for_type(type);
    cache_line_t *line;
    size_t index;
    size_t way;

    ++cache->stats->accesses;
    line = find_line_in_cache(cache, mem_addr);

    if (line == NULL) {
        ++cache->stats->misses;
        write_back_dirty_other_l1(mem_addr, type);
        line = fill_l1_line(cache, mem_addr);
    } else {
        index = cache_index(mem_addr, cache);
        way = way_for_line(cache, index, line);
        touch_way(cache, index, way);
    }

    return line->data[mem_addr & (LINE_SIZE - 1U)];
}

// STUDENT TODO: implement write cache, using the l1 and l2 structure
void write_cache(uint64_t mem_addr, uint8_t value, mem_type_t type) {
    cache_desc_t *cache = cache_for_type(type);
    cache_line_t *line;
    size_t index;
    size_t way;

    ++cache->stats->accesses;
    line = find_line_in_cache(cache, mem_addr);

    if (line == NULL) {
        ++cache->stats->misses;
        write_back_dirty_other_l1(mem_addr, type);
        line = fill_l1_line(cache, mem_addr);
    } else {
        index = cache_index(mem_addr, cache);
        way = way_for_line(cache, index, line);
        touch_way(cache, index, way);
    }

    line->data[mem_addr & (LINE_SIZE - 1U)] = value;
    line->modified = 1;
    invalidate_other_l1_copy(mem_addr, type);
}

// STUDENT TODO: implement functions to get cache stats
cache_stats_t get_l1_instr_stats() {
    return l1_instr_stats;
}

cache_stats_t get_l1_data_stats() {
    return l1_data_stats;
}

cache_stats_t get_l2_stats() {
    return l2_stats;
}

// STUDENT TODO: implement a function returning a pointer to a specific cache line for an address
//               or null if the line is not present in the cache
cache_line_t* get_l1_instr_cache_line(uint64_t mem_addr) {
    return find_line_in_cache(&l1_instr_cache, mem_addr);
}

cache_line_t* get_l1_data_cache_line(uint64_t mem_addr) {
    return find_line_in_cache(&l1_data_cache, mem_addr);
}

cache_line_t* get_l2_cache_line(uint64_t mem_addr) {
    return find_line_in_cache(&l2_cache, mem_addr);
}
