// The shared renderer, through the offscreen path at 4x MSAA: shapes land where
// the scene says, interiors are exact colors, and two renders are byte-identical.
// Needs a Metal device. No window.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 180 };

static uint32_t px_at(const unsigned char *px, int x, int y) {
    const unsigned char *p = px + ((size_t)y * W + (size_t)x) * 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void render(an_scene *s, ani_offscreen *off, ani_renderer *r, unsigned char *out) {
    void *pool = ani_pool_push();
    ani_offscreen_begin(off, AN_BLACK);
    CHECK(ani_renderer_draw(r, s, W, H));
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(off, out);
    ani_pool_pop(pool);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_render: headless Metal setup failed\n");
        return 1;
    }
    ani_offscreen off;
    ani_renderer r;
    CHECK(ani_offscreen_make(&off, W, H, 4));
    CHECK(ani_renderer_init(&r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_NONE, 4));

    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H });
    // rect centered at (60,50), 40x20 -> covers x 40..79, y 40..59
    an_rect(s, &(an_rect_opts){ .pos = { 60, 50 }, .size = { 40, 20 }, .fill = AN_HEX(0xFF48B0) });
    // circle at (200,100) r 30, drawn above a rect it overlaps (higher z)
    an_circle(s, &(an_circle_opts){ .pos = { 200, 100 }, .radius = 30, .fill = AN_HEX(0x0078BF), .z = 1 });
    an_rect(s, &(an_rect_opts){ .pos = { 230, 100 }, .size = { 40, 40 }, .fill = AN_HEX(0xFFE800) });
    // group moves its child: rect at local (0,0) ends up at (290,160)
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 290, 160 } });
    an_rect(s, &(an_rect_opts){ .parent = g, .size = { 10, 10 }, .fill = AN_HEX(0x00A95C) });

    unsigned char *a = malloc(ani_offscreen_frame_bytes(&off));
    unsigned char *b = malloc(ani_offscreen_frame_bytes(&off));
    CHECK(a && b);
    render(s, &off, &r, a);

    CHECK(px_at(a, 5, 5) == 0xF3EDE2FF);      // background (cream)
    CHECK(px_at(a, 60, 50) == 0xFF48B0FF);    // rect interior
    CHECK(px_at(a, 40, 40) == 0xFF48B0FF);    // rect top-left pixel (edge on whole pixels)
    CHECK(px_at(a, 39, 50) == 0xF3EDE2FF);    // just left of it
    CHECK(px_at(a, 60, 60) == 0xF3EDE2FF);    // just below it (rows are top-down)
    CHECK(px_at(a, 200, 100) == 0x0078BFFF);  // circle center
    CHECK(px_at(a, 225, 100) == 0x0078BFFF);  // overlap: circle (z 1) wins over rect
    CHECK(px_at(a, 245, 100) == 0xFFE800FF);  // rect outside the circle
    CHECK(px_at(a, 200, 135) == 0xF3EDE2FF);  // below the circle
    CHECK(px_at(a, 290, 160) == 0x00A95CFF);  // grouped child, moved by the group

    render(s, &off, &r, b);
    CHECK(memcmp(a, b, ani_offscreen_frame_bytes(&off)) == 0); // deterministic

    free(a);
    free(b);
    an_scene_destroy(s);
    ani_renderer_shutdown(&r);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    return check_done("test_render");
}
