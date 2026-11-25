// 04_stroke_stress: render the stroke stress set to a PNG for checking joins,
// caps and tapers by eye. The same strokes are checked numerically by
// tests/test_stroke.c.
//
//   ./build/examples/04_stroke_stress [out.png]      default stroke_stress.png
//
// Grid, left to right, top to bottom (see tests/stroke_stress.h):
//   reversal | reversal taper+overshoot | 1deg acute | 1deg turns | zero-length segs
//   duplicates at corner | dot | closed square | closed tri taper | closed spike
//   tight curve | cusp | loop | spiral | pressure zero
//   zigzag overshoot | wide short segs | taper > length | right angles | scribble
#include "anown.h"
#include "../../tests/stroke_stress.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *path = argc >= 2 ? argv[1] : "stroke_stress.png";
    an_scene *s = an_scene_create(NULL);
    if (!s) return 1;
    // faint cell grid
    for (int c = 1; c < STRESS_COLS; c++) {
        an_line(s, &(an_line_opts){ .from = { STRESS_CELL_W * (float)c, 0 }, .to = { STRESS_CELL_W * (float)c, 1080 },
                                    .stroke = AN_HEX(0xD8D0C2), .stroke_width = 2 });
    }
    for (int r = 1; r < STRESS_ROWS; r++) {
        an_line(s, &(an_line_opts){ .from = { 0, STRESS_CELL_H * (float)r }, .to = { 1920, STRESS_CELL_H * (float)r },
                                    .stroke = AN_HEX(0xD8D0C2), .stroke_width = 2 });
    }
    an_obj *objs[STRESS_MAX];
    stress_case cases[STRESS_MAX];
    const int n = stress_build(s, objs, cases);
    // centerlines in pink on top, thin, so joins can be compared with the path
    for (int i = 0; i < n; i++) {
        an_stroke(s, &(an_stroke_opts){ .pos = an_obj_get(objs[i]).pos, .segs = cases[i].segs,
                                        .seg_count = cases[i].nseg, .stroke = AN_HEX(0xFF48B0), .stroke_width = 1.2f });
    }
    const bool ok = an_scene_save_png(s, 0, path);
    if (ok) printf("wrote %s (%d stress strokes)\n", path, n);
    an_scene_destroy(s);
    return ok ? 0 : 1;
}
