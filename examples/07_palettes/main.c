// 07_palettes: a swatch PNG per palette. Every color (paper, ink, a, b, c, shadow) as a
// block on cream (top half) and on charcoal (bottom half). Flat: chaos 0, so no
// grain, vignette or riso; this is about the colors.
//
//   ./build/examples/07_palettes [dir]     writes <dir>/swatch_<name>.png (default .)
#include "anown.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *dir = argc >= 2 ? argv[1] : ".";
    const an_palette *pals[3] = { &AN_PALETTE_RIOT, &AN_PALETTE_HAZARD, &AN_PALETTE_ACID };
    int rc = 0;
    for (int p = 0; p < 3; p++) {
        const an_palette *pal = pals[p];
        an_scene *s = an_scene_create(&(an_scene_desc){ .width = 1200, .height = 600, .style = { .palette = pal } });
        an_set_chaos(s, 0);
        // bottom half: charcoal panel
        an_rect(s, &(an_rect_opts){ .pos = { 600, 450 }, .size = { 1200, 300 }, .fill = AN_HEX(0x16161A) });
        const an_color cols[6] = { pal->paper, pal->ink, pal->a, pal->b, pal->c, pal->shadow };
        for (int i = 0; i < 6; i++) {
            const float x = 115.0f + 194.0f * (float)i;
            // a thin outline in the opposite tone so paper-on-cream and ink-on-charcoal stay visible
            an_rect(s, &(an_rect_opts){ .pos = { x, 150 }, .size = { 160, 160 }, .fill = cols[i],
                                        .stroke = AN_HEX(0x16161A), .stroke_width = 2 });
            an_rect(s, &(an_rect_opts){ .pos = { x, 450 }, .size = { 160, 160 }, .fill = cols[i],
                                        .stroke = AN_HEX(0xF3EDE2), .stroke_width = 2 });
        }
        char path[512];
        snprintf(path, sizeof path, "%s/swatch_%s.png", dir, pal->name);
        if (an_scene_save_png(s, 0, path)) {
            printf("wrote %s (paper, ink, a, b, c, shadow on cream / on charcoal)\n", path);
        } else {
            rc = 1;
        }
        an_scene_destroy(s);
    }
    return rc;
}
