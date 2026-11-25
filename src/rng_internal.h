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

#endif
