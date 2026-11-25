// Internal: RNG seeding helpers.
#ifndef ANOWN_RNG_INTERNAL_H
#define ANOWN_RNG_INTERNAL_H

#include "anown.h"

// Exactly pcg32_srandom_r(initstate, initseq) from the reference implementation.
an_rng ani_pcg32_seed(uint64_t initstate, uint64_t initseq);

// Stream key for a named object. Top bit set, so it never collides with an id.
uint64_t ani_rng_name_key(const char *name);

// Key of the scene's own stream (no object has id 0; the hidden root is 0).
#define ANI_RNG_SCENE_KEY 0u
// The root of the object tree's keys (any constant other than the scene's).
#define ANI_RNG_ROOT_KEY 0x726F6F74ull

// An unnamed object's key: from its parent's key and its child index (the
// parent's running count of children created), not a global creation counter,
// so adding an object never changes another subtree's randomness.
uint64_t ani_rng_child_key(uint64_t parent, uint64_t index);
// A library helper's internal part: from its parent's key, a fixed part name
// and an index (docs/explainer.md, "Keys").
uint64_t ani_rng_part_key(uint64_t parent, const char *part, uint64_t index);

#endif
