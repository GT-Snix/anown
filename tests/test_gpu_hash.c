// The GPU port of an_hash (shaders/hash.glsl) against the CPU an_hash
// (src/rng.c), on real Metal output: a 64x64 target where pixel i writes
// an_hash(inputs[i]) as 4 bytes, read back.
//
// Inputs: for each argument (seed, stream, counter) the edge values 0,
// all-ones, every power of two 2^0..2^63, and values straddling 2^32, alone and
// combined; the pinned golden inputs from test_rng; the rest random.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "hash_test.glsl.h"

#include <stdlib.h>
#include <string.h>

enum { W = 64, H = 64, N = W * H };

typedef struct hash_in {
    uint32_t seed_lo, seed_hi, stream_lo, stream_hi, counter_lo, counter_hi, pad0, pad1;
} hash_in;

static hash_in mk(uint64_t seed, uint64_t stream, uint64_t counter) {
    return (hash_in){ (uint32_t)seed, (uint32_t)(seed >> 32), (uint32_t)stream, (uint32_t)(stream >> 32),
                      (uint32_t)counter, (uint32_t)(counter >> 32), 0, 0 };
}

int main(void) {
    // edge values
    uint64_t edge[80];
    int ne = 0;
    edge[ne++] = 0;
    edge[ne++] = UINT64_MAX;
    for (int k = 0; k < 64; k++) edge[ne++] = 1ull << k;
    const uint64_t straddle[] = { 0xFFFFFFFFull, 0x100000000ull, 0x100000001ull, 0xFFFFFFFF00000000ull,
                                  0x00000000FFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFEull, 0x1FFFFFFFFull,
                                  0xFFFFFFFFFFFFFFFEull, 0x8000000000000001ull };
    for (size_t i = 0; i < sizeof straddle / sizeof straddle[0]; i++) edge[ne++] = straddle[i];

    hash_in *in = calloc(N, sizeof *in);
    int n = 0;
    // pinned golden inputs (values from test_rng)
    in[n++] = mk(0, 0, 0);
    in[n++] = mk(1, 2, 3);
    in[n++] = mk(3, 2, 1);
    in[n++] = mk(42, 7, 1000);
    // each edge value in each argument position, and all three at once
    for (int i = 0; i < ne; i++) {
        in[n++] = mk(edge[i], 0, 0);
        in[n++] = mk(0, edge[i], 0);
        in[n++] = mk(0, 0, edge[i]);
        in[n++] = mk(edge[i], edge[i], edge[i]);
        in[n++] = mk(edge[i], ~edge[i], edge[i] ^ 0x5555555555555555ull);
    }
    // pairs of edge values
    for (int i = 0; i < ne && n < N - 2048; i += 3) {
        for (int j = 0; j < ne && n < N - 2048; j += 5) in[n++] = mk(edge[i], edge[j], edge[(i + j) % ne]);
    }
    const int n_edge = n;
    // the rest: random 64-bit values
    an_rng rng = an_rng_make(2026, 11);
    while (n < N) {
        uint64_t v[3];
        for (int k = 0; k < 3; k++) v[k] = (uint64_t)an_rng_u32(&rng) << 32 | an_rng_u32(&rng);
        in[n++] = mk(v[0], v[1], v[2]);
    }

    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_gpu_hash: headless Metal setup failed\n");
        return 1;
    }
    ani_offscreen off;
    CHECK(ani_offscreen_make(&off, W, H, 1));
    sg_buffer buf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .storage_buffer = true, .vertex_buffer = false },
        .data = { .ptr = in, .size = sizeof(hash_in) * N },
    });
    sg_view view = sg_make_view(&(sg_view_desc){ .storage_buffer.buffer = buf });
    sg_pipeline pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(hash_test_shader_desc(sg_query_backend())),
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
    });
    CHECK(sg_query_view_state(view) == SG_RESOURCESTATE_VALID);
    CHECK(sg_query_pipeline_state(pip) == SG_RESOURCESTATE_VALID);

    unsigned char *px = malloc(ani_offscreen_frame_bytes(&off));
    void *pool = ani_pool_push();
    ani_offscreen_begin(&off, AN_BLACK);
    sg_apply_pipeline(pip);
    sg_apply_bindings(&(sg_bindings){ .views[VIEW_hash_inputs] = view });
    const hash_test_params_t params = { .width = W };
    sg_apply_uniforms(UB_hash_test_params, &SG_RANGE(params));
    sg_draw(0, 6, 1);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&off, px);
    ani_pool_pop(pool);

    int bad = 0, bad_edge = 0;
    for (int i = 0; i < N; i++) {
        const hash_in v = in[i];
        const uint64_t seed = (uint64_t)v.seed_hi << 32 | v.seed_lo;
        const uint64_t stream = (uint64_t)v.stream_hi << 32 | v.stream_lo;
        const uint64_t counter = (uint64_t)v.counter_hi << 32 | v.counter_lo;
        const uint32_t cpu = an_hash(seed, stream, counter);
        const unsigned char *p = px + (size_t)i * 4;
        const uint32_t gpu = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        if (gpu != cpu) {
            if (bad < 5) fprintf(stderr, "  input %d (%016llx, %016llx, %016llx): gpu %08x cpu %08x\n", i,
                                 (unsigned long long)seed, (unsigned long long)stream, (unsigned long long)counter, gpu, cpu);
            bad++;
            if (i < n_edge) bad_edge++;
        }
    }
    // pinned golden values, read from the GPU output directly
    const uint32_t golden[4] = { 0x4fee4699u, 0x56460618u, 0x57b17c99u, 0x00fb9e45u };
    for (int i = 0; i < 4; i++) {
        const unsigned char *p = px + (size_t)i * 4;
        const uint32_t gpu = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        CHECK(gpu == golden[i]);
    }
    printf("  %d inputs (%d edge-value vectors from %d edge values, %d random): %d mismatches\n", N, n_edge, ne,
           N - n_edge, bad);
    CHECK(bad == 0 && bad_edge == 0);

    free(px);
    free(in);
    sg_destroy_buffer(buf);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    return check_done("test_gpu_hash");
}
