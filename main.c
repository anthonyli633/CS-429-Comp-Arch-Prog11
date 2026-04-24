#include "tcache.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

static void check(const char *name, int condition) {
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++failures;
    }
}

static void prepare_byte(uint64_t addr, uint8_t value) {
    write_memory(addr, value);
}

static void test_basic_data_read_write(void) {
    uint64_t addr = 0x1234;
    cache_stats_t stats;

    init_cache(LRU);
    prepare_byte(addr, 0x5a);

    check("cold data read returns memory value", read_cache(addr, DATA) == 0x5a);

    stats = get_l1_data_stats();
    check("cold data read records one L1 data miss",
          stats.accesses == 1 && stats.misses == 1);

    stats = get_l2_stats();
    check("cold data read records one L2 miss",
          stats.accesses == 1 && stats.misses == 1);

    write_cache(addr, 0xa5, DATA);
    check("data write hit is visible on later read", read_cache(addr, DATA) == 0xa5);

    stats = get_l1_data_stats();
    check("write hit and read hit do not add L1 misses",
          stats.accesses == 3 && stats.misses == 1);

    stats = get_l2_stats();
    check("L1 hits do not access L2", stats.accesses == 1 && stats.misses == 1);
    check("written L1 data line is marked modified",
          get_l1_data_cache_line(addr) != NULL &&
          get_l1_data_cache_line(addr)->modified);
}

static void test_l1_data_lru(void) {
    const uint64_t stride = (HW11_L1_SIZE / HW11_L1_DATA_ASSOC);
    uint64_t a = 0x2000;
    uint64_t b = a + stride;
    uint64_t c = b + stride;
    cache_stats_t stats;

    init_cache(LRU);
    prepare_byte(a, 0x11);
    prepare_byte(b, 0x22);
    prepare_byte(c, 0x33);

    (void)read_cache(a, DATA);
    (void)read_cache(b, DATA);
    (void)read_cache(a, DATA);
    (void)read_cache(c, DATA);

    check("L1 data LRU keeps recently touched line",
          get_l1_data_cache_line(a) != NULL);
    check("L1 data LRU evicts least recently used line",
          get_l1_data_cache_line(b) == NULL &&
          get_l1_data_cache_line(c) != NULL);

    stats = get_l1_data_stats();
    check("L1 data LRU trace miss count", stats.accesses == 4 && stats.misses == 3);
}

static void test_l1_writeback_to_l2(void) {
    const uint64_t stride = (HW11_L1_SIZE / HW11_L1_DATA_ASSOC);
    uint64_t a = 0x10000;
    uint64_t b = a + stride;
    uint64_t c = b + stride;
    cache_line_t *l2_line;

    init_cache(LRU);

    write_cache(a + 7, 0xe1, DATA);
    write_cache(b + 7, 0xb2, DATA);
    write_cache(c + 7, 0xc3, DATA);

    l2_line = get_l2_cache_line(a);
    check("dirty L1 data eviction writes back to L2",
          get_l1_data_cache_line(a) == NULL &&
          l2_line != NULL &&
          l2_line->modified &&
          l2_line->data[7] == 0xe1);
}

static void test_split_l1_coherence(void) {
    uint64_t addr = 0x6123;
    cache_line_t *data_line;

    init_cache(LRU);
    prepare_byte(addr, 0x10);

    check("instruction cache initially reads memory",
          read_cache(addr, INSTR) == 0x10);

    write_cache(addr, 0xab, DATA);
    check("data write invalidates stale instruction copy",
          get_l1_instr_cache_line(addr) == NULL);

    check("instruction cache sees data write after L2 writeback",
          read_cache(addr, INSTR) == 0xab);

    data_line = get_l1_data_cache_line(addr);
    check("coherence writeback leaves data line clean",
          data_line != NULL && !data_line->modified);
}

static uint64_t run_policy_probe(replacement_policy_e policy) {
    const uint64_t l2_stride = HW11_L2_SIZE / HW11_L2_ASSOC;
    uint64_t addrs[5];
    int round;
    int i;

    init_cache(policy);

    for (i = 0; i < 5; ++i) {
        addrs[i] = 0x800 + (uint64_t)i * l2_stride;
        prepare_byte(addrs[i], (uint8_t)(0x40 + i));
    }

    for (round = 0; round < 4; ++round) {
        for (i = 0; i < 5; ++i) {
            (void)read_cache(addrs[i], DATA);
        }
    }

    return get_l2_stats().misses;
}

static void compare_replacement_policies(void) {
    uint64_t lru_misses = run_policy_probe(LRU);
    uint64_t random_misses = run_policy_probe(RANDOM);

    printf("\nReplacement policy comparison, 5-line cyclic L2 conflict trace:\n");
    printf("  LRU L2 misses:    %lu\n", (unsigned long)lru_misses);
    printf("  RANDOM L2 misses: %lu\n", (unsigned long)random_misses);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    test_basic_data_read_write();
    test_l1_data_lru();
    test_l1_writeback_to_l2();
    test_split_l1_coherence();
    compare_replacement_policies();

    if (failures != 0) {
        printf("\n%d test(s) failed.\n", failures);
        return EXIT_FAILURE;
    }

    printf("\nAll cache tests passed.\n");
    return EXIT_SUCCESS;
}
