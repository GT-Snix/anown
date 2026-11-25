// PCG32 (O'Neill, pcg-random.org): 64-bit LCG state, XSH-RR output, with the
// increment selecting the stream. Seeding follows pcg32_srandom_r so outputs
// match the reference implementation for the same (initstate, initseq).
//
// Scene/object streams: (seed, key) is mixed with SplitMix64 into initstate and
// initseq, so nearby keys (ids 1, 2, 3...) give unrelated streams.
#include "rng_internal.h"

static uint64_t splitmix64_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint32_t an_rng_u32(an_rng *r) {
    const uint64_t old = r->state;
    r->state = old * 6364136223846793005ull + r->inc;
    const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    const uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

an_rng ani_pcg32_seed(uint64_t initstate, uint64_t initseq) {
    an_rng r = { .state = 0, .inc = (initseq << 1u) | 1u };
    an_rng_u32(&r);
    r.state += initstate;
    an_rng_u32(&r);
    return r;
}

an_rng an_rng_make(uint64_t seed, uint64_t key) {
    const uint64_t k = splitmix64_mix(key + 0x9E3779B97F4A7C15ull);
    const uint64_t initstate = splitmix64_mix(seed ^ k);
    const uint64_t initseq = splitmix64_mix(initstate ^ 0xDA942042E4DD58B5ull);
    return ani_pcg32_seed(initstate, initseq);
}

uint64_t ani_rng_name_key(const char *name) {
    // FNV-1a 64; the top bit is set so a name key can't equal a small creation id
    uint64_t h = 0xCBF29CE484222325ull;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 0x100000001B3ull;
    }
    return h | (1ull << 63);
}

uint32_t an_rng_below(an_rng *r, uint32_t bound) {
    if (bound == 0) return 0;
    // reject the low (2^32 mod bound) values so every result is equally likely
    const uint32_t threshold = (uint32_t)(-bound) % bound;
    for (;;) {
        const uint32_t x = an_rng_u32(r);
        if (x >= threshold) return x % bound;
    }
}

int an_rng_int(an_rng *r, int lo, int hi) {
    if (hi < lo) {
        const int t = lo;
        lo = hi;
        hi = t;
    }
    const uint32_t span = (uint32_t)((int64_t)hi - (int64_t)lo) + 1u; // 0 means 2^32
    const uint32_t x = span ? an_rng_below(r, span) : an_rng_u32(r);
    return (int)((int64_t)lo + (int64_t)x);
}

float an_rng_float(an_rng *r) {
    return (float)(an_rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

float an_rng_range(an_rng *r, float lo, float hi) {
    return lo + (hi - lo) * an_rng_float(r);
}

// ---- stateless hash for render-time noise ---------------------------------------
// Each input is pre-mixed with its own constant and folded in through the
// SplitMix64 finalizer, so (seed, stream, counter) permutations don't collide.
uint32_t an_hash(uint64_t seed, uint64_t stream_id, uint64_t counter) {
    uint64_t h = splitmix64_mix(seed ^ 0x9E3779B97F4A7C15ull);
    h = splitmix64_mix(h ^ splitmix64_mix(stream_id + 0xD1B54A32D192ED03ull));
    h = splitmix64_mix(h ^ splitmix64_mix(counter + 0x8CB92BA72F3D8DD7ull));
    return (uint32_t)(h >> 32);
}

float an_hash_float(uint64_t seed, uint64_t stream_id, uint64_t counter) {
    return (float)(an_hash(seed, stream_id, counter) >> 8) * (1.0f / 16777216.0f);
}
