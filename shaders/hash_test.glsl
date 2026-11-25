// Test-only shader (tests/test_gpu_hash.c): pixel i writes an_hash of input i
// as 4 bytes (little-endian: R = bits 0..7, ..., A = bits 24..31).
@include hash.glsl

@vs hash_test_vs
void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
        vec2(1, -1), vec2(1, 1), vec2(-1, 1));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
@end

@fs hash_test_fs
@include_block an_hash
struct hash_input {
    uvec2 seed;
    uvec2 stream;
    uvec2 counter;
    uvec2 pad;
};
layout(binding=0) readonly buffer hash_inputs {
    hash_input inputs[];
};
layout(binding=0) uniform hash_test_params {
    int width;
};
out vec4 frag_color;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    hash_input v = inputs[p.y * width + p.x];
    uint h = an_hash(v.seed, v.stream, v.counter);
    frag_color = vec4(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u), float(h >> 24u)) / 255.0;
}
@end

@program hash_test hash_test_vs hash_test_fs
