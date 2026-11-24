/* anown: an animation library for explainer videos.
 * Public API. This is the only header users include.
 *
 * Conventions:
 *   - Options are structs passed as compound literals:
 *       an_rect(s, &(an_rect_opts){ .pos = {960, 540}, .size = {200, 100} });
 *     Every field has a sensible zero default, and NULL opts means all defaults.
 *   - Coordinates are scene pixels: origin top-left, x right, y down. A scene is
 *     authored at its own width/height; preview and export scale it to the target.
 *   - Rotation is in radians; positive turns clockwise on screen.
 *   - Objects live in the scene's arena. There is no per-object free; everything
 *     goes away with an_scene_destroy().
 */
#ifndef ANOWN_H
#define ANOWN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AN_VERSION_MAJOR 0
#define AN_VERSION_MINOR 1
#define AN_VERSION_PATCH 0

/* ---- basic types -------------------------------------------------------- */

/* RGBA in 0..1, sRGB-encoded (the same values you'd write in hex). */
typedef struct an_color {
    float r, g, b, a;
} an_color;

/* Build an opaque color from 0xRRGGBB. */
#define AN_HEX(rgb)                                         \
    ((an_color){ (float)(((rgb) >> 16) & 0xFF) / 255.0f,    \
                 (float)(((rgb) >> 8) & 0xFF) / 255.0f,     \
                 (float)((rgb) & 0xFF) / 255.0f, 1.0f })

#define AN_CREAM AN_HEX(0xF3EDE2)
#define AN_BLACK AN_HEX(0x000000)

/* Convert 0xRRGGBB to an_color (function form of AN_HEX). */
an_color an_hex(uint32_t rgb);

typedef struct an_vec2 {
    float x, y;
} an_vec2;

/* ---- scene -------------------------------------------------------------- */

typedef struct an_scene an_scene;

typedef struct an_scene_desc {
    int width;           /* default 1920 */
    int height;          /* default 1080 */
    int fps;             /* default 60 */
    uint64_t seed;       /* seeds the scene's RNG; 0 is a valid seed */
    an_color background; /* alpha 0 (the zero value) means AN_CREAM */
} an_scene_desc;

/* Returns NULL on allocation failure. */
an_scene *an_scene_create(const an_scene_desc *desc);
/* Frees the scene and every object in it. */
void an_scene_destroy(an_scene *s);

/* ---- objects ------------------------------------------------------------ */

typedef enum an_kind {
    AN_KIND_RECT,
    AN_KIND_CIRCLE,
    AN_KIND_LINE,
    AN_KIND_TEXT,
    AN_KIND_GROUP,
} an_kind;

typedef struct an_obj an_obj;

/* Fields shared by every object's options struct. */
#define AN_OBJ_FIELDS                                                         \
    an_obj *parent;     /* NULL: top level. Must belong to the same scene. */ \
    an_vec2 pos;        /* in the parent's space */                           \
    an_vec2 scale;      /* {0,0} means {1,1} */                               \
    float rotation;     /* radians, clockwise */                              \
    int z;              /* order among siblings; ties draw in creation order */ \
    an_color fill;      /* alpha 0 means AN_BLACK */                          \
    an_color stroke;    /* lines: the line color; alpha 0 means AN_BLACK */   \
    float stroke_width; /* lines: thickness in pixels; 0 means 4 */

/* Axis-aligned rectangle centered on pos. */
typedef struct an_rect_opts {
    AN_OBJ_FIELDS
    an_vec2 size; /* default {100, 100} */
} an_rect_opts;

/* Circle centered on pos. */
typedef struct an_circle_opts {
    AN_OBJ_FIELDS
    float radius; /* default 50 */
} an_circle_opts;

/* Straight line from `from` to `to`, both relative to pos. */
typedef struct an_line_opts {
    AN_OBJ_FIELDS
    an_vec2 from;
    an_vec2 to;
} an_line_opts;

/* Text anchored at pos. Not rendered yet (no font rendering in this version). */
typedef struct an_text_opts {
    AN_OBJ_FIELDS
    const char *text; /* copied into the scene */
    float size;       /* em size in pixels; default 48 */
} an_text_opts;

/* An empty node that transforms its children. */
typedef struct an_group_opts {
    AN_OBJ_FIELDS
} an_group_opts;

/* Each returns NULL on allocation failure or if opts->parent is invalid. */
an_obj *an_rect(an_scene *s, const an_rect_opts *opts);
an_obj *an_circle(an_scene *s, const an_circle_opts *opts);
an_obj *an_line(an_scene *s, const an_line_opts *opts);
an_obj *an_text(an_scene *s, const an_text_opts *opts);
an_obj *an_group(an_scene *s, const an_group_opts *opts);

/* Unlike the opts defaults, setters take values literally (scale {0,0} is zero). */
void an_obj_set_pos(an_obj *o, an_vec2 pos);
void an_obj_set_scale(an_obj *o, an_vec2 scale);
void an_obj_set_rotation(an_obj *o, float radians);
void an_obj_set_z(an_obj *o, int z);
void an_obj_set_fill(an_obj *o, an_color fill);
/* Move o under parent (NULL: top level), as its last child. Returns false and
 * changes nothing if that would create a cycle or parent is in another scene. */
bool an_obj_set_parent(an_obj *o, an_obj *parent);

an_kind an_obj_kind(const an_obj *o);
an_obj *an_obj_parent(const an_obj *o); /* NULL for top-level objects */

/* ---- output ------------------------------------------------------------- */

/* Open a window showing the scene, letterboxed to its aspect ratio.
 * On macOS this does not return: the process exits when the window closes. */
void an_scene_preview(an_scene *s);

typedef struct an_export_desc {
    const char *path; /* default "out.mp4" */
    int frames;       /* default: one second at the scene's fps */
    int crf;          /* x264 quality; default 18 */
} an_export_desc;

/* Render offscreen and encode H.264 mp4 through ffmpeg (must be on PATH).
 * Returns false on failure, with a message on stderr. */
bool an_scene_export(an_scene *s, const an_export_desc *desc);

/* ---- windows (no scene) ------------------------------------------------- */

/* Options for an_window_run. Zero fields get defaults:
 * title "anown", 1280x720, clear color AN_CREAM. */
typedef struct an_window_desc {
    const char *title;
    int width;
    int height;
    an_color clear; /* alpha 0 (the zero value) means AN_CREAM */
} an_window_desc;

/* Open a window and clear it every frame until it is closed. */
void an_window_run(const an_window_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* ANOWN_H */
