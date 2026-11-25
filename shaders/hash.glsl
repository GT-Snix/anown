// an_hash on the GPU, bit for bit equal to the CPU version in src/rng.c
// (docs/layers.md). 64-bit values are uvec2(lo, hi).
//
//   mix(z) = splitmix64 finalizer:
//     z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
//     z = (z ^ (z >> 27)) * 0x94D049BB133111EB
//     z ^ (z >> 31)
//   an_hash(seed, stream, counter):
//     h = mix(seed ^ 0x9E3779B97F4A7C15)
//     h = mix(h ^ mix(stream + 0xD1B54A32D192ED03))
//     h = mix(h ^ mix(counter + 0x8CB92BA72F3D8DD7))
//     return h >> 32
//
// Tested against the CPU on real Metal output: tests/test_gpu_hash.c.
@block an_hash
uvec2 u64_xor(uvec2 a, uvec2 b) { return a ^ b; }

// a >> n for 0 < n < 32
uvec2 u64_shr(uvec2 a, uint n) {
    return uvec2((a.x >> n) | (a.y << (32u - n)), a.y >> n);
}

uvec2 u64_add(uvec2 a, uvec2 b) {
    uint lo = a.x + b.x;
    uint carry = lo < a.x ? 1u : 0u;
    return uvec2(lo, a.y + b.y + carry);
}

// a * b mod 2^64: full 32x32->64 low product, plus the two cross terms << 32
uvec2 u64_mul(uvec2 a, uvec2 b) {
    uint hi, lo;
    umulExtended(a.x, b.x, hi, lo);
    hi += a.x * b.y + a.y * b.x;
    return uvec2(lo, hi);
}

uvec2 splitmix64_mix(uvec2 z) {
    z = u64_mul(u64_xor(z, u64_shr(z, 30u)), uvec2(0x1CE4E5B9u, 0xBF58476Du));
    z = u64_mul(u64_xor(z, u64_shr(z, 27u)), uvec2(0x133111EBu, 0x94D049BBu));
    return u64_xor(z, u64_shr(z, 31u));
}

uint an_hash(uvec2 seed, uvec2 stream, uvec2 counter) {
    uvec2 h = splitmix64_mix(u64_xor(seed, uvec2(0x7F4A7C15u, 0x9E3779B9u)));
    h = splitmix64_mix(u64_xor(h, splitmix64_mix(u64_add(stream, uvec2(0xD192ED03u, 0xD1B54A32u)))));
    h = splitmix64_mix(u64_xor(h, splitmix64_mix(u64_add(counter, uvec2(0x2F3D8DD7u, 0x8CB92BA7u)))));
    return h.y;
}

// an_hash mapped to [0, 1), 24-bit resolution (same as an_hash_float on the CPU)
float an_hash_float(uvec2 seed, uvec2 stream, uvec2 counter) {
    return float(an_hash(seed, stream, counter) >> 8u) * (1.0 / 16777216.0);
}
@end
