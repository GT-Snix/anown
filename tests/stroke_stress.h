// The stroke stress set: hard cases for the triangulator, laid out in a grid.
// Public API only, so both tests/test_stroke.c (geometry checks) and
// examples/04_stroke_stress (PNG for eyeballing joins) build the same strokes.
#ifndef ANOWN_STROKE_STRESS_H
#define ANOWN_STROKE_STRESS_H

#include "anown.h"

#include <math.h>

#define STRESS_COLS 5
#define STRESS_ROWS 4
#define STRESS_CELL_W 384.0f
#define STRESS_CELL_H 270.0f
#define STRESS_MAX 24

typedef struct stress_case {
    const char *name;
    an_seg segs[24];
    int nseg;
    float width;
    an_brush brush;
} stress_case;

static const float STRESS_PRESSURE[5] = { 0.3f, 1.0f, 0.0f, 1.0f, 0.3f };

// Fills `out` (STRESS_MAX entries); returns how many.
static int stress_cases(stress_case *out) {
    const float d1 = 1.0f * 3.14159265f / 180.0f; // 1 degree
    int n = 0;
    // 1. exact 180-degree reversal, constant width
    out[n++] = (stress_case){ "reversal", { AN_MOVE(-140, 0), AN_LINE(140, 0), AN_LINE(-60, 0) }, 3, 24, { 0 } };
    // 2. reversal, tapered, with overshoot
    out[n++] = (stress_case){ "reversal taper+overshoot", { AN_MOVE(-140, 30), AN_LINE(140, 30), AN_LINE(-60, 30) }, 3, 24,
                              { .taper_in = 60, .taper_out = 60, .overshoot = 0.6f } };
    // 3. 1-degree acute angle (turn of 179 degrees)
    out[n++] = (stress_case){ "1deg acute", { AN_MOVE(-150, -40), AN_LINE(150, -40),
                                              AN_LINE(150 - 300 * cosf(d1), -40 + 300 * sinf(d1)) }, 3, 18, { 0 } };
    // 4. 1-degree turn (nearly straight), several in a row
    out[n++] = (stress_case){ "1deg turns", { AN_MOVE(-150, 0), AN_LINE(-75, 0),
                                              AN_LINE(0, 75 * sinf(d1)), AN_LINE(75, 0), AN_LINE(150, 75 * sinf(d1)) }, 5, 30, { 0 } };
    // 5. zero-length segments in the middle and at both ends
    out[n++] = (stress_case){ "zero-length segs", { AN_MOVE(-140, -60), AN_LINE(-140, -60), AN_LINE(0, -60), AN_LINE(0, -60),
                                                    AN_LINE(0, -60), AN_LINE(140, 60), AN_LINE(140, 60) }, 7, 20, { 0 } };
    // 6. duplicate points around a sharp corner, with overshoot
    out[n++] = (stress_case){ "duplicates at corner", { AN_MOVE(-140, 60), AN_LINE(0, -80), AN_LINE(0, -80),
                                                        AN_LINE(0.0001f, -80), AN_LINE(140, 60) }, 5, 22, { .overshoot = 0.8f } };
    // 7. a single point (dot), and a path that is only duplicates
    out[n++] = (stress_case){ "dot", { AN_MOVE(-60, 0), AN_LINE(-60, 0), AN_MOVE(60, 0), AN_LINE(60, 0), AN_LINE(60, 0) }, 5, 40, { 0 } };
    // 8. closed square, constant width (seamless loop)
    out[n++] = (stress_case){ "closed square", { AN_MOVE(-90, -90), AN_LINE(90, -90), AN_LINE(90, 90), AN_LINE(-90, 90), AN_CLOSE }, 5, 16, { 0 } };
    // 9. closed triangle, tapered, overshoot (open at the seam)
    out[n++] = (stress_case){ "closed tri taper", { AN_MOVE(0, -100), AN_LINE(110, 90), AN_LINE(-110, 90), AN_CLOSE }, 4, 18,
                              { .taper_in = 40, .taper_out = 40, .overshoot = 0.6f } };
    // 10. closed with an explicit repeated end point and a 1-degree spike
    out[n++] = (stress_case){ "closed spike", { AN_MOVE(-120, 80), AN_LINE(120, 80), AN_LINE(-120, 80 - 240 * sinf(d1)),
                                                AN_LINE(-120, 80), AN_CLOSE }, 5, 14, { 0 } };
    // 11. very tight curve: radius smaller than the stroke width
    out[n++] = (stress_case){ "tight curve", { AN_MOVE(-150, 40), AN_LINE(-10, 40), AN_CUBIC(8, 40, 8, 20, -2, 20),
                                               AN_CUBIC(-12, 20, -12, 40, 10, 40), AN_LINE(150, 40) }, 5, 26, { 0 } };
    // 12. cubic with a cusp
    out[n++] = (stress_case){ "cusp", { AN_MOVE(-140, 60), AN_CUBIC(140, -100, -140, -100, 140, 60) }, 2, 20, { 0 } };
    // 13. cubic self-loop
    out[n++] = (stress_case){ "loop", { AN_MOVE(-150, 60), AN_CUBIC(250, -150, -250, -150, 150, 60) }, 2, 18,
                              { .taper_in = 30, .taper_out = 80 } };
    // 14. tight spiral
    {
        stress_case c = { "spiral", { AN_MOVE(0, 0) }, 1, 10, { .taper_in = 20 } };
        for (int i = 1; i < 23; i++) {
            const float a = (float)i * 0.9f, r = 6.0f + 5.0f * (float)i;
            c.segs[c.nseg++] = AN_QUAD(r * cosf(a - 0.45f) * 1.1f, r * sinf(a - 0.45f) * 1.1f, r * cosf(a), r * sinf(a));
        }
        out[n++] = c;
    }
    // 15. pressure curve with a zero in the middle
    out[n++] = (stress_case){ "pressure zero", { AN_MOVE(-150, 0), AN_CUBIC(-50, -120, 50, 120, 150, 0) }, 2, 34,
                              { .pressure = STRESS_PRESSURE, .pressure_count = 5 } };
    // 16. zig-zag with overshoot, tapered
    out[n++] = (stress_case){ "zigzag overshoot", { AN_MOVE(-150, 60), AN_LINE(-90, -60), AN_LINE(-30, 60), AN_LINE(30, -60),
                                                    AN_LINE(90, 60), AN_LINE(150, -60) }, 6, 14,
                              { .taper_in = 40, .taper_out = 40, .overshoot = 0.7f } };
    // 17. very wide stroke on very short segments
    out[n++] = (stress_case){ "wide short segs", { AN_MOVE(-20, 0), AN_LINE(-15, 3), AN_LINE(-10, -3), AN_LINE(-5, 3), AN_LINE(0, -3),
                                                   AN_LINE(5, 3), AN_LINE(10, -3), AN_LINE(15, 3), AN_LINE(20, 0) }, 9, 70, { 0 } };
    // 18. taper longer than the path
    out[n++] = (stress_case){ "taper > length", { AN_MOVE(-40, 0), AN_LINE(40, 0) }, 2, 30, { .taper_in = 200, .taper_out = 200 } };
    // 19. 90-degree corners in both directions, constant width
    out[n++] = (stress_case){ "right angles", { AN_MOVE(-150, 60), AN_LINE(-75, 60), AN_LINE(-75, -60), AN_LINE(0, -60),
                                                AN_LINE(0, 60), AN_LINE(75, 60), AN_LINE(75, -60), AN_LINE(150, -60) }, 8, 20, { 0 } };
    // 20. back-and-forth reversals in a row (scribble)
    out[n++] = (stress_case){ "scribble", { AN_MOVE(-140, 0), AN_LINE(140, 2), AN_LINE(-130, 4), AN_LINE(130, 6),
                                            AN_LINE(-120, 8), AN_LINE(120, 10) }, 6, 12, { .overshoot = 0.5f } };
    return n;
}

// Build the stress set into `s` (1920x1080), one stroke per grid cell, charcoal.
// Fills objs (STRESS_MAX entries) and returns the count.
static int stress_build(an_scene *s, an_obj **objs, stress_case *cases) {
    const int n = stress_cases(cases);
    for (int i = 0; i < n; i++) {
        const int col = i % STRESS_COLS, row = i / STRESS_COLS;
        objs[i] = an_stroke(s, &(an_stroke_opts){
            .pos = { STRESS_CELL_W * ((float)col + 0.5f), STRESS_CELL_H * ((float)row + 0.5f) },
            .segs = cases[i].segs, .seg_count = cases[i].nseg,
            .stroke = AN_HEX(0x16161A), .stroke_width = cases[i].width, .brush = cases[i].brush,
        });
    }
    return n;
}

#endif
