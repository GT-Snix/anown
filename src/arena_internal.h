// Internal: chunked arena allocator. Every allocation is zeroed and lives until
// ani_arena_reset() or ani_arena_release(). Reset keeps the blocks for reuse.
#ifndef ANOWN_ARENA_INTERNAL_H
#define ANOWN_ARENA_INTERNAL_H

#include <stdalign.h>
#include <stddef.h>

typedef struct ani_arena_block ani_arena_block;

typedef struct ani_arena {
    ani_arena_block *first;   // chain of blocks, in allocation order
    ani_arena_block *current; // block being filled; blocks after it are empty
    size_t block_size;        // minimum size of a new block
} ani_arena;

#define ANI_ARENA_DEFAULT_BLOCK (64 * 1024)

// block_size 0 means ANI_ARENA_DEFAULT_BLOCK. Allocates nothing up front.
void ani_arena_init(ani_arena *a, size_t block_size);
// Free all blocks. The arena can be used again afterwards.
void ani_arena_release(ani_arena *a);
// Make all memory available again, keeping the blocks.
void ani_arena_reset(ani_arena *a);

// Zeroed memory, aligned to `align` (a power of two). NULL on failure.
void *ani_arena_alloc(ani_arena *a, size_t size, size_t align);
// Copy of a NUL-terminated string (NULL -> "").
char *ani_arena_strdup(ani_arena *a, const char *str);

// Bytes handed out since the last reset, including alignment padding.
size_t ani_arena_used(const ani_arena *a);
// Bytes held in blocks.
size_t ani_arena_capacity(const ani_arena *a);

#define ANI_NEW(arena, T) ((T *)ani_arena_alloc((arena), sizeof(T), alignof(T)))

#endif
