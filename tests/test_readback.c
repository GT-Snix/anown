// Offscreen render + Metal readback must return exact, top-down RGBA8 pixels.
//
// Renders known patterns headlessly and checks every byte, so a sokol upgrade
// (or our own change) that breaks readback fails here. What it catches:
//   - wrong channel order (RGBA vs BGRA): every color used has R != B
//   - vertical flip: rects are placed asymmetrically in y
//   - stale data / bad ordering against the frame's render pass: frame 2 uses
//     different colors and positions from frame 1 and is read back into the same
//     staging buffer
//   - off-by-one rasterization at integer pixel edges
// Needs a Metal device (any Apple Silicon Mac). No window.
#include "anown.h"
#include "offscreen_internal.h"
#include "solid_rect.glsl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { W = 64, H = 32 };

typedef struct rect { int x, y, w, h; } rect;

static sg_pipeline g_pip;

static void render(ani_offscreen *off, an_color bg, rect r, an_color ink) {
    void *pool = ani_pool_push();
    const vs_params_t vsp = {
        .rect = { (float)r.x, (float)r.y, (float)r.w, (float)r.h },
        .target = { (float)W, (float)H },
    };
    const fs_params_t fsp = { .color = { ink.r, ink.g, ink.b, ink.a } };
    ani_offscreen_begin(off, bg);
    sg_apply_pipeline(g_pip);
    sg_apply_uniforms(UB_vs_params, &SG_RANGE(vsp));
    sg_apply_uniforms(UB_fs_params, &SG_RANGE(fsp));
    sg_draw(0, 6, 1);
    sg_end_pass();
    sg_commit();
    ani_pool_pop(pool);
}

// Check every pixel: inside r must be `ink`, everything else `bg`, alpha 255.
static int check(const char *name, const uint8_t *px, uint32_t bg, rect r, uint32_t ink) {
    int bad = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const int inside = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            const uint32_t want = inside ? ink : bg;
            const uint8_t *p = px + ((size_t)y * W + (size_t)x) * 4;
            const uint8_t e[4] = { (uint8_t)(want >> 16), (uint8_t)(want >> 8), (uint8_t)want, 255 };
            if (p[0] != e[0] || p[1] != e[1] || p[2] != e[2] || p[3] != e[3]) {
                if (bad < 5) {
                    fprintf(stderr, "%s: pixel (%d,%d) = %02x%02x%02x%02x, want %02x%02x%02x%02x\n",
                            name, x, y, p[0], p[1], p[2], p[3], e[0], e[1], e[2], e[3]);
                }
                bad++;
            }
        }
    }
    if (bad) fprintf(stderr, "%s: %d of %d pixels wrong\n", name, bad, W * H);
    return bad;
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_readback: headless Metal setup failed\n");
        return 1;
    }
    int bad = 0;
    // Without MSAA, and with 4x MSAA (readback from the resolve image). Rect edges
    // are on whole pixels, so every sample in a pixel agrees and the resolve is exact.
    const int sample_counts[] = { 1, 4 };
    for (int i = 0; i < 2; i++) {
        const int sc = sample_counts[i];
        ani_offscreen off;
        if (!ani_offscreen_make(&off, W, H, sc)) {
            fprintf(stderr, "test_readback: offscreen target creation failed (%dx MSAA)\n", sc);
            return 1;
        }
        g_pip = sg_make_pipeline(&(sg_pipeline_desc){
            .shader = sg_make_shader(solid_rect_shader_desc(sg_query_backend())),
            .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
            .depth.pixel_format = SG_PIXELFORMAT_NONE,
            .sample_count = sc,
        });
        uint8_t *px = malloc(ani_offscreen_frame_bytes(&off));
        if (!px) return 1;
        char name[32];

        // Frame 1: cream with a pink rect near the top-left.
        const rect r1 = { 8, 4, 16, 8 };
        render(&off, AN_HEX(0xF3EDE2), r1, AN_HEX(0xFF48B0));
        ani_offscreen_readback(&off, px);
        snprintf(name, sizeof name, "frame 1 (%dx)", sc);
        bad += check(name, px, 0xF3EDE2, r1, 0xFF48B0);

        // Frame 2: different background, color and position, same staging buffer.
        const rect r2 = { 40, 19, 20, 10 };
        render(&off, AN_HEX(0x0078BF), r2, AN_HEX(0xFFE800));
        ani_offscreen_readback(&off, px);
        snprintf(name, sizeof name, "frame 2 (%dx)", sc);
        bad += check(name, px, 0x0078BF, r2, 0xFFE800);

        free(px);
        sg_destroy_pipeline(g_pip);
        ani_offscreen_destroy(&off);
    }
    ani_gfx_shutdown();

    if (bad) return 1;
    printf("test_readback: ok (2 frames x {1x, 4x MSAA}, %dx%d, every byte exact)\n", W, H);
    return 0;
}
