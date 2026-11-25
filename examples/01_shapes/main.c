// 01_shapes: every object kind in one static scene.
//
//   ./build/examples/01_shapes                    live preview window
//   ./build/examples/01_shapes --export out.mp4   2 s of video through ffmpeg
//
// An array of boxes with an index pointer, a small graph, and a rotated,
// scaled group. Text objects are in the scene but not drawn yet.
#include "anown.h"

#include <stdio.h>
#include <string.h>

#define PINK   AN_HEX(0xFF48B0)
#define YELLOW AN_HEX(0xFFE800)
#define BLUE   AN_HEX(0x0078BF)
#define GREEN  AN_HEX(0x00A95C)
#define INK    AN_HEX(0x1A1A1A)

static void array_row(an_scene *s) {
    // a group holds the row so it can be moved as one
    an_obj *row = an_group(s, &(an_group_opts){ .pos = { 260, 260 } });
    for (int i = 0; i < 8; i++) {
        an_rect(s, &(an_rect_opts){
            .parent = row,
            .pos = { (float)i * 112, 0 },
            .size = { 100, 100 },
            .fill = (i == 3) ? PINK : YELLOW,
        });
    }
    // pointer under index 3: a line and a circle
    an_line(s, &(an_line_opts){
        .parent = row, .pos = { 3 * 112, 70 }, .from = { 0, 0 }, .to = { 0, 60 },
        .stroke = INK, .stroke_width = 8,
    });
    an_circle(s, &(an_circle_opts){ .parent = row, .pos = { 3 * 112, 150 }, .radius = 16, .fill = INK });
    an_text(s, &(an_text_opts){ .parent = row, .pos = { 0, -90 }, .text = "int a[8]" });
}

static void graph(an_scene *s) {
    const an_vec2 nodes[5] = { { 300, 760 }, { 520, 620 }, { 560, 900 }, { 800, 760 }, { 1000, 640 } };
    const int edges[6][2] = { { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 3, 4 }, { 1, 4 } };
    // edges first in creation order, but z keeps nodes on top regardless
    for (int i = 0; i < 6; i++) {
        an_line(s, &(an_line_opts){
            .from = nodes[edges[i][0]], .to = nodes[edges[i][1]],
            .stroke = INK, .stroke_width = 6,
        });
    }
    for (int i = 0; i < 5; i++) {
        an_circle(s, &(an_circle_opts){ .pos = nodes[i], .radius = 46, .fill = (i == 3) ? PINK : BLUE, .z = 1 });
    }
}

static void spinner(an_scene *s) {
    // rotation and scale are inherited by children
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 1500, 780 }, .rotation = 0.35f, .scale = { 1.2f, 1.2f } });
    an_rect(s, &(an_rect_opts){ .parent = g, .size = { 260, 260 }, .fill = GREEN });
    an_rect(s, &(an_rect_opts){ .parent = g, .size = { 140, 140 }, .fill = YELLOW, .rotation = 0.785f });
    an_circle(s, &(an_circle_opts){ .parent = g, .radius = 40, .fill = PINK });
}

int main(int argc, char **argv) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = 1920, .height = 1080, .fps = 60, .seed = 1 });
    if (!s) return 1;
    array_row(s);
    graph(s);
    spinner(s);

    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        const int64_t n = an_scene_export(s, &(an_export_desc){ .path = argv[2], .frames = 120 });
        an_scene_destroy(s);
        if (n > 0) printf("wrote %s (%lld frames)\n", argv[2], (long long)n);
        return n > 0 ? 0 : 1;
    }
    an_scene_preview(s); // does not return on macOS
    return 0;
}
