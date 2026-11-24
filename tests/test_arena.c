// Arena allocator: alignment, zeroing, growth, oversized allocations, reset.
#include "arena_internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

static int aligned(const void *p, size_t a) { return ((uintptr_t)p & (a - 1)) == 0; }

static void test_alignment_and_zeroing(void) {
    ani_arena a;
    ani_arena_init(&a, 256);
    const size_t aligns[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++) {
            unsigned char *p = ani_arena_alloc(&a, 3 + i, aligns[i]); // odd sizes
            CHECK(p != NULL);
            CHECK(aligned(p, aligns[i]));
            int zero = 1;
            for (size_t k = 0; k < 3 + i; k++) zero &= p[k] == 0;
            CHECK(zero);
            memset(p, 0xAB, 3 + i); // dirty it so reuse after reset must re-zero
        }
    }
    CHECK(ani_arena_alloc(&a, 8, 3) == NULL); // non power of two
    ani_arena_release(&a);
}

static void test_growth_keeps_data(void) {
    ani_arena a;
    ani_arena_init(&a, 128); // tiny blocks force many new blocks
    enum { N = 1000 };
    uint32_t *ptrs[N];
    for (uint32_t i = 0; i < N; i++) {
        ptrs[i] = ANI_NEW(&a, uint32_t);
        CHECK(ptrs[i] != NULL);
        *ptrs[i] = i * 2654435761u;
    }
    int intact = 1;
    for (uint32_t i = 0; i < N; i++) intact &= *ptrs[i] == i * 2654435761u;
    CHECK(intact);
    CHECK(ani_arena_used(&a) >= N * sizeof(uint32_t));
    ani_arena_release(&a);
    CHECK(ani_arena_capacity(&a) == 0);
}

static void test_oversized(void) {
    ani_arena a;
    ani_arena_init(&a, 64);
    char *small = ani_arena_alloc(&a, 16, 1);
    char *big = ani_arena_alloc(&a, 10000, 64); // bigger than a block
    CHECK(small && big);
    CHECK(aligned(big, 64));
    memset(big, 1, 10000);
    char *after = ani_arena_alloc(&a, 16, 1); // still works after the big one
    CHECK(after != NULL);
    CHECK(small[0] == 0 && after[0] == 0);
    ani_arena_release(&a);
}

static void test_reset_reuses_blocks(void) {
    ani_arena a;
    ani_arena_init(&a, 1024);
    void *first = ani_arena_alloc(&a, 100, 16);
    for (int i = 0; i < 100; i++) ani_arena_alloc(&a, 100, 16); // ~10 blocks
    const size_t cap = ani_arena_capacity(&a);
    CHECK(cap >= 100 * 100);

    ani_arena_reset(&a);
    CHECK(ani_arena_used(&a) == 0);
    CHECK(ani_arena_capacity(&a) == cap); // blocks kept

    // Same allocation sequence lands at the same addresses and grows nothing.
    void *again = ani_arena_alloc(&a, 100, 16);
    CHECK(again == first);
    for (int i = 0; i < 100; i++) ani_arena_alloc(&a, 100, 16);
    CHECK(ani_arena_capacity(&a) == cap);

    // An allocation too big for any kept block still works after a reset.
    ani_arena_reset(&a);
    CHECK(ani_arena_alloc(&a, 5000, 8) != NULL);
    ani_arena_release(&a);
}

static void test_strdup(void) {
    ani_arena a;
    ani_arena_init(&a, 0);
    char src[] = "bubble sort";
    char *s = ani_arena_strdup(&a, src);
    src[0] = 'X'; // copy is independent
    CHECK(s && strcmp(s, "bubble sort") == 0);
    char *e = ani_arena_strdup(&a, NULL);
    CHECK(e && e[0] == 0);
    ani_arena_release(&a);
}

int main(void) {
    test_alignment_and_zeroing();
    test_growth_keeps_data();
    test_oversized();
    test_reset_reuses_blocks();
    test_strdup();
    return check_done("test_arena");
}
