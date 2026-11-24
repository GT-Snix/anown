#include "arena_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ani_arena_block {
    ani_arena_block *next;
    size_t cap;  // bytes in data[]
    size_t used; // bytes handed out from data[]
    unsigned char data[];
};

void ani_arena_init(ani_arena *a, size_t block_size) {
    a->first = NULL;
    a->current = NULL;
    a->block_size = block_size ? block_size : ANI_ARENA_DEFAULT_BLOCK;
}

void ani_arena_release(ani_arena *a) {
    ani_arena_block *b = a->first;
    while (b) {
        ani_arena_block *next = b->next;
        free(b);
        b = next;
    }
    a->first = NULL;
    a->current = NULL;
}

void ani_arena_reset(ani_arena *a) {
    for (ani_arena_block *b = a->first; b; b = b->next) b->used = 0;
    a->current = a->first;
}

// Offset into b->data where an allocation of `size` aligned to `align` would
// start, or SIZE_MAX if it doesn't fit.
static size_t fit(const ani_arena_block *b, size_t size, size_t align) {
    const uintptr_t base = (uintptr_t)b->data;
    const uintptr_t at = (base + b->used + (align - 1)) & ~(uintptr_t)(align - 1);
    const size_t off = (size_t)(at - base);
    if (off > b->cap || b->cap - off < size) return SIZE_MAX;
    return off;
}

void *ani_arena_alloc(ani_arena *a, size_t size, size_t align) {
    if (align == 0) align = 1;
    if ((align & (align - 1)) != 0) return NULL; // not a power of two
    if (size == 0) size = 1;

    ani_arena_block *b = a->current;
    size_t off = b ? fit(b, size, align) : SIZE_MAX;

    // Blocks after `current` are empty (after a reset); use the first that fits.
    while (off == SIZE_MAX && b && b->next) {
        b = b->next;
        off = fit(b, size, align);
    }

    if (off == SIZE_MAX) {
        if (size > SIZE_MAX - align - sizeof(ani_arena_block)) return NULL;
        size_t cap = size + align - 1;
        if (cap < a->block_size) cap = a->block_size;
        ani_arena_block *nb = malloc(sizeof(ani_arena_block) + cap);
        if (!nb) return NULL;
        nb->next = NULL;
        nb->cap = cap;
        nb->used = 0;
        // append at the end of the chain
        if (!a->first) {
            a->first = nb;
        } else {
            ani_arena_block *last = b ? b : a->first;
            while (last->next) last = last->next;
            last->next = nb;
        }
        b = nb;
        off = fit(b, size, align);
    }

    a->current = b;
    b->used = off + size;
    void *p = b->data + off;
    memset(p, 0, size);
    return p;
}

char *ani_arena_strdup(ani_arena *a, const char *str) {
    if (!str) str = "";
    const size_t n = strlen(str) + 1;
    char *p = ani_arena_alloc(a, n, 1);
    if (p) memcpy(p, str, n);
    return p;
}

size_t ani_arena_used(const ani_arena *a) {
    size_t n = 0;
    for (const ani_arena_block *b = a->first; b; b = b->next) n += b->used;
    return n;
}

size_t ani_arena_capacity(const ani_arena *a) {
    size_t n = 0;
    for (const ani_arena_block *b = a->first; b; b = b->next) n += b->cap;
    return n;
}
