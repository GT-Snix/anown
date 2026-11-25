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

/* ---- palettes ------------------------------------------------------------ */

typedef struct an_palette {
    const char *name;
    an_color paper, ink, a, b, c; /* a, b, c: accents */
    an_color shadow;              /* default hard-shadow color: distinct from the ink */
} an_palette;

/* Placeholders (docs/layers.md), all on cream paper with charcoal ink:
 *   riot:   electric cyan #00C2FF, hot magenta #FF2E88, acid lime #C6FF00
 *   hazard: safety orange #FF6A13, electric cyan #00C2FF, acid lime #C6FF00
 *   acid:   acid lime #C6FF00, hot magenta #FF2E88, safety orange #FF6A13
 * Shadow color for all three: riso medium blue #3255A4. */
extern const an_palette AN_PALETTE_RIOT, AN_PALETTE_HAZARD, AN_PALETTE_ACID;
/* "riot", "hazard" or "acid"; NULL if unknown. */
const an_palette *an_palette_named(const char *name);

/* ---- style --------------------------------------------------------------- */

/* Per-object override of a scene default. AN_AUTO (the zero value) follows
 * the scene / the rule; AN_ON and AN_OFF force it. */
typedef enum an_toggle { AN_AUTO = 0, AN_ON, AN_OFF } an_toggle;

/* Paper grain over time (docs/progress.md, "Grain modes"). */
typedef enum an_grain_mode {
    AN_GRAIN_AUTO = 0,  /* static in AN_EXPORT_SHARE exports (it compresses);
                           animated in master exports, PNGs and the preview */
    AN_GRAIN_STATIC,    /* one fixed paper texture, the same every frame */
    AN_GRAIN_ANIMATED,  /* re-rolled at 12 Hz (or every frame: grain_every_frame) */
} an_grain_mode;

typedef enum an_background {
    AN_BG_PAPER = 0, /* paper color, grain and vignette (default) */
    AN_BG_FLAT,      /* paper color only */
} an_background;

/* The look (docs/layers.md). Every magnitude here is scaled by effective chaos,
 * so chaos 0 renders the clean look. Zero fields take the defaults shown. */
typedef struct an_style {
    an_vec2 riso_offset;       /* fill offset, scene px; default {4, -3} */
    bool riso_off;             /* no riso offset anywhere */
    bool shadow;               /* objects cast hard shadows by default */
    an_vec2 shadow_offset;     /* scene px beyond the ink's outer edge; default {8, 8} */
    an_color shadow_color;     /* alpha 0 means the palette's shadow color */
    float boil;                /* line boil amplitude, scene px; default 1.5 */
    float boil_spacing;        /* arc length between boil samples, local px; default 15 */
    bool boil_off;
    float grain;               /* paper grain strength; default 0.06 */
    bool grain_off;
    bool grain_every_frame;    /* animated grain: every frame instead of 12 Hz */
    an_grain_mode grain_mode;
    float vignette;            /* default 0.25 */
    bool vignette_off;
    an_background background;
    const an_palette *palette; /* default &AN_PALETTE_RIOT */
} an_style;

typedef struct an_scene_desc {
    int width;           /* default 1920 */
    int height;          /* default 1080 */
    int fps;             /* default 60 */
    uint64_t seed;       /* seeds the scene's RNG and grain; 0 is a valid seed */
    an_color background; /* paper color; alpha 0 (the zero value) means the palette's paper */
    an_style style;
    float max_zoom;      /* largest scale on screen (camera zoom x object scales) the
                            curves are flattened for; default 4. Past it, curves may
                            show facets (one warning). docs/camera.md */
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
    AN_KIND_ARROW,
    AN_KIND_STROKE,
    AN_KIND_GLYPH, /* one letter of a text (an_text_glyph) */
    AN_KIND_CODE,  /* a code block (an_code): a group of lines */
} an_kind;

typedef struct an_obj an_obj;

/* Fill pattern (rect and circle fills), anchored in object space: it moves,
 * rotates and scales with the object (docs/layers.md). */
typedef enum an_pattern_kind {
    AN_PATTERN_SOLID = 0,
    AN_PATTERN_HALFTONE, /* dots on a square grid; dot area = density x cell area */
    AN_PATTERN_HATCH,    /* parallel lines; line width = density x spacing */
} an_pattern_kind;

typedef struct an_pattern {
    an_pattern_kind kind;
    float size;    /* cell size / line spacing, object px; default 12 */
    float angle;   /* radians, rotation of the grid or lines in object space */
    float density; /* ink coverage 0..1; default 0.5 */
} an_pattern;

/* How a stroke's width varies along each subpath (each "pen stroke").
 * Width at a point = stroke_width * taper * pressure. Zero = constant width. */
typedef struct an_brush {
    float taper_in;        /* arc length (px, local) over which width grows from 0 */
    float taper_out;       /* arc length over which width shrinks to 0 at the end */
    const float *pressure; /* optional multipliers, evenly spaced along the
                              subpath and linearly interpolated; copied */
    int pressure_count;
    float overshoot;       /* corner overshoot, in stroke widths (try 0.6); the
                              line runs a little past sharp corners, like a pen */
} an_brush;

/* Fields shared by every object's options struct. */
#define AN_OBJ_FIELDS                                                         \
    const char *name;   /* optional, unique in the scene; pins the RNG stream */ \
    bool hidden;        /* start at opacity 0 (fade in later) */             \
    bool undrawn;       /* start with draw progress 0 (see an_draw) */        \
    bool no_fill;       /* rect/circle: outline only */                       \
    an_obj *parent;     /* NULL: top level. Must belong to the same scene. */ \
    an_vec2 pos;        /* in the parent's space */                           \
    an_vec2 scale;      /* {0,0} means {1,1} */                               \
    float rotation;     /* radians, clockwise */                              \
    int z;              /* order among siblings; ties draw in creation order */ \
    an_color fill;      /* alpha 0 means AN_BLACK */                          \
    an_color stroke;    /* alpha 0 means AN_BLACK. Rect/circle are outlined   \
                           only if stroke or stroke_width is given. */       \
    float stroke_width; /* pixels; 0 means 4 */                               \
    an_brush brush;     /* width profile and corner overshoot of the stroke */ \
    an_toggle riso;     /* fill offset: AUTO = only if the object has ink */ \
    an_toggle shadow;   /* hard shadow: AUTO = the scene default (style) */ \
    an_pattern pattern; /* fill pattern; zero = solid */                    \
    bool paper_under;   /* fill the silhouette with the scene background    \
                           color first, so pattern gaps and the riso sliver \
                           hide what's beneath (docs/layers.md) */

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

/* ---- fonts and text (docs/text.md) ---------------------------------------- */

/* A font belongs to a scene (its arena). Glyph outlines are flattened once per
 * (font, glyph) and drawn like any other path: fill, ink, boil, riso, shadow. */
typedef struct an_font an_font;
/* Built in (embedded, OFL 1.1, see assets/fonts): Archivo Black for display
 * text, JetBrains Mono for code. Loaded once per scene. */
an_font *an_font_display(an_scene *s);
an_font *an_font_mono(an_scene *s);
/* A TrueType/OpenType file. Static fonts only: variation axes aren't applied.
 * NULL (with a message) if it can't be read. */
an_font *an_font_load(an_scene *s, const char *path);

typedef enum an_align { AN_ALIGN_LEFT = 0, AN_ALIGN_CENTER, AN_ALIGN_RIGHT } an_align;
typedef enum an_anchor {
    AN_ANCHOR_BASELINE = 0, /* pos.y is the first line's baseline */
    AN_ANCHOR_TOP,          /* ... the first line's ascent */
    AN_ANCHOR_MIDDLE,       /* ... the middle of the block (first ascent to last descent) */
    AN_ANCHOR_BOTTOM,       /* ... the last line's descent */
} an_anchor;

/* Text at pos: a node whose children are its letters (AN_KIND_GLYPH objects,
 * one per visible glyph; spaces have none). Each letter is an object of its
 * own: move, rotate, scale, fade or draw it with the usual anims (an_stagger
 * for per-letter effects). Rotation and scale pivot on the middle of the
 * letter's advance at half the x-height.
 * Letters take fill, stroke, stroke_width, pattern, riso, shadow and
 * paper_under from these opts; the outline is drawn only if stroke or
 * stroke_width is given (as for rects). Setting or animating the text's fill,
 * stroke or stroke width changes every letter that hasn't been given its own
 * (by a setter or an anim on the letter); opacity multiplies down the tree. A text draws as one object: all its
 * shadows, then all fills, then all outlines.
 * an_draw(text) draws the outlines on letter by letter, each letter's fill
 * appearing when its outline closes; without an outline it reveals whole
 * letters in order (a typewriter). */
typedef struct an_text_opts {
    AN_OBJ_FIELDS
    const char *text;    /* UTF-8, copied. "\n" breaks lines; tabs are 4 spaces */
    float size;          /* em size in pixels; default 48 */
    an_font *font;       /* NULL: an_font_display(s) */
    an_align align;      /* per line, relative to pos.x */
    an_anchor anchor;
    float line_spacing;  /* multiplier on the font's line height; default 1 */
    an_toggle kerning;   /* AUTO/ON: the font's kerning; OFF: none */
    an_toggle size_chaos; /* AUTO/ON: chaos scales with the stem width on screen,
                            so small text stays calm (docs/text.md); OFF: none */
    bool letters_hidden;  /* every letter starts at opacity 0 */
    bool letters_undrawn; /* every letter starts with draw progress 0 */
} an_text_opts;

/* Letters of a text, in string order (spaces and line breaks excluded). */
int an_text_length(const an_obj *text);
an_obj *an_text_glyph(const an_obj *text, int i); /* NULL if out of range */

/* A code block (docs/text.md): monospace text on a grid, optional line
 * numbers, a highlight bar per line, and a box. pos is the box's top-left.
 * The code is calm (its size factor puts chaos at 0 at code sizes) while the
 * box and highlight bars boil at the block's chaos. */
typedef struct an_code_opts {
    AN_OBJ_FIELDS          /* fill: code color (alpha 0: the palette's ink);
                              stroke, stroke_width: the box outline (default ink, 4);
                              riso, shadow: for the code text (AUTO = off) */
    const char *code;      /* UTF-8; one line per "\n" */
    float size;            /* em size; default 30 (an 18 px advance in the mono) */
    an_font *font;         /* NULL: an_font_mono(s) */
    bool line_numbers;
    int first_line;        /* number of the first line; default 1 */
    an_color number_color; /* alpha 0: the ink, 45% toward the paper */
    an_color highlight;    /* bar color; alpha 0: the palette's accent c */
    an_color box_fill;     /* alpha 0: the paper */
    bool no_box;
} an_code_opts;

an_obj *an_code(an_scene *s, const an_code_opts *opts);
/* Line n's highlight bar (n counts from first_line), a rect behind the line,
 * hidden at first: fade it in to highlight. NULL if out of range. */
an_obj *an_code_line(const an_obj *code, int n);
/* Line n's text object (its letters via an_text_glyph). */
an_obj *an_code_text(const an_obj *code, int n);

/* An empty node that transforms its children. */
typedef struct an_group_opts {
    AN_OBJ_FIELDS
} an_group_opts;

/* Arrow from `from` to `to` (relative to pos): a shaft, then a two-sided head
 * drawn as one pen stroke through the tip. */
typedef struct an_arrow_opts {
    AN_OBJ_FIELDS
    an_vec2 from;
    an_vec2 to;
    float head;    /* head length in pixels; default 4 * stroke width, min 16 */
} an_arrow_opts;

/* Path segments for an_stroke. Coordinates are relative to pos. */
typedef enum an_seg_kind {
    AN_SEG_MOVE,  /* start a new subpath at p[0] */
    AN_SEG_LINE,  /* line to p[0] */
    AN_SEG_QUAD,  /* quadratic bezier: control p[0], end p[1] */
    AN_SEG_CUBIC, /* cubic bezier: controls p[0], p[1], end p[2] */
    AN_SEG_CLOSE, /* close the current subpath */
} an_seg_kind;

typedef struct an_seg {
    an_seg_kind kind;
    an_vec2 p[3];
} an_seg;

#define AN_MOVE(x, y)  ((an_seg){ AN_SEG_MOVE, { { (x), (y) } } })
#define AN_LINE(x, y)  ((an_seg){ AN_SEG_LINE, { { (x), (y) } } })
#define AN_QUAD(cx, cy, x, y) ((an_seg){ AN_SEG_QUAD, { { (cx), (cy) }, { (x), (y) } } })
#define AN_CUBIC(c1x, c1y, c2x, c2y, x, y) \
    ((an_seg){ AN_SEG_CUBIC, { { (c1x), (c1y) }, { (c2x), (c2y) }, { (x), (y) } } })
#define AN_CLOSE       ((an_seg){ AN_SEG_CLOSE, { { 0, 0 } } })

/* A free-form stroke. Curves are flattened once, when the object is created
 * (adaptive subdivision); rendering only reads the result. */
typedef struct an_stroke_opts {
    AN_OBJ_FIELDS
    const an_seg *segs; /* copied; a path without a leading MOVE starts at (0,0) */
    int seg_count;
} an_stroke_opts;

/* Objects are created at the scene's cursor and aren't drawn before it.
 * Each returns NULL on allocation failure, if opts->parent is invalid, or if
 * opts->name is already used in the scene (with a message on stderr). */
an_obj *an_rect(an_scene *s, const an_rect_opts *opts);
an_obj *an_circle(an_scene *s, const an_circle_opts *opts);
an_obj *an_line(an_scene *s, const an_line_opts *opts);
an_obj *an_text(an_scene *s, const an_text_opts *opts);
an_obj *an_group(an_scene *s, const an_group_opts *opts);
an_obj *an_arrow(an_scene *s, const an_arrow_opts *opts);
an_obj *an_stroke(an_scene *s, const an_stroke_opts *opts);

/* Setters record an instant change at the cursor: from then on the value is
 * the one set, cutting off any animation running on that property. Values are
 * taken literally (scale {0,0} is zero); only opts apply defaults. */
void an_obj_set_pos(an_obj *o, an_vec2 pos);
void an_obj_set_scale(an_obj *o, an_vec2 scale);
void an_obj_set_rotation(an_obj *o, float radians);
void an_obj_set_opacity(an_obj *o, float opacity);
void an_obj_set_z(an_obj *o, int z);
void an_obj_set_fill(an_obj *o, an_color fill);
void an_obj_set_stroke(an_obj *o, an_color stroke);
void an_obj_set_stroke_width(an_obj *o, float width);
void an_obj_set_draw(an_obj *o, float progress);

/* Chaos (docs/layers.md): the scene's dial and a per-object multiplier (default
 * 1, applied down the tree). Both are timeline properties; these setters are
 * instant changes at the cursor. an_set_chaos takes either a scene or an object. */
void an_scene_set_chaos(an_scene *s, float chaos);
void an_obj_set_chaos(an_obj *o, float chaos);
#define an_set_chaos(target, value) \
    _Generic((target), an_scene *: an_scene_set_chaos, an_obj *: an_obj_set_chaos)((target), (value))
/* Move o under parent (NULL: top level), as its last child. Returns false and
 * changes nothing if that would create a cycle or parent is in another scene. */
bool an_obj_set_parent(an_obj *o, an_obj *parent);

an_kind an_obj_kind(const an_obj *o);
an_obj *an_obj_parent(const an_obj *o); /* NULL for top-level objects */

/* ---- randomness --------------------------------------------------------- */

/* PCG32 (pcg32_random_r, XSH-RR). All randomness comes from streams owned by a
 * scene and derived from an_scene_desc.seed. Draw only while building the scene;
 * rendering never does. Copying an an_rng forks the stream. */
typedef struct an_rng {
    uint64_t state;
    uint64_t inc; /* stream selector, always odd */
} an_rng;

/* The scene's own stream. */
an_rng *an_scene_rng(an_scene *s);
/* The object's stream: keyed by a hash of its name if it has one, else by its
 * creation id. Drawing from one object's stream never affects another's. */
an_rng *an_obj_rng(an_obj *o);

/* A stream from (seed, key). Same inputs, same sequence. */
an_rng an_rng_make(uint64_t seed, uint64_t key);
uint32_t an_rng_u32(an_rng *r);
/* Uniform in [0, bound), without modulo bias. bound 0 returns 0. */
uint32_t an_rng_below(an_rng *r, uint32_t bound);
/* Uniform in [lo, hi] inclusive. */
int an_rng_int(an_rng *r, int lo, int hi);
/* Uniform in [0, 1), 24-bit resolution (every value exactly representable). */
float an_rng_float(an_rng *r);
/* Uniform in [lo, hi). */
float an_rng_range(an_rng *r, float lo, float hi);

/* Stateless hash for render-time noise (grain, jitter): the result depends
 * only on the inputs, never on call order. Use e.g. (seed, object id, frame). */
uint32_t an_hash(uint64_t seed, uint64_t stream_id, uint64_t counter);
/* an_hash mapped to [0, 1), 24-bit resolution. */
float an_hash_float(uint64_t seed, uint64_t stream_id, uint64_t counter);

/* ---- timeline ----------------------------------------------------------- */

/* See docs/timeline.md. The script runs once and records tracks at a cursor;
 * rendering samples them at frame N. Durations are seconds; the cursor is
 * kept in integer microseconds and converted to frames only when sampling. */

typedef enum an_prop {
    AN_PROP_POS,
    AN_PROP_SCALE,
    AN_PROP_ROTATION,
    AN_PROP_OPACITY,
    AN_PROP_FILL,
    AN_PROP_STROKE,
    AN_PROP_Z,
    AN_PROP_STROKE_WIDTH, /* pixels */
    AN_PROP_DRAW,         /* draw progress 0..1 along the path's arc length */
    AN_PROP_CHAOS,        /* 0..1. Scene target: the chaos dial. Objects: a multiplier
                             (default 1) applied down the tree. See docs/layers.md. */
    /* scene target only: the camera (docs/camera.md) */
    AN_PROP_CAMERA_POS,      /* the world point at the frame's center; default the scene's center */
    AN_PROP_CAMERA_ZOOM,     /* screen px per world px; default 1; interpolated in log space */
    AN_PROP_CAMERA_ROTATION, /* radians; turning the camera clockwise turns the world counter-clockwise */
    AN_PROP_CAMERA_SHAKE,    /* shake events (an_shake); not a sampled value */
    AN_PROP_COUNT
} an_prop;

typedef enum an_ease_kind {
    AN_EASE_KIND_IN_OUT, /* zero value: cubic ease-in-out */
    AN_EASE_KIND_LINEAR,
    AN_EASE_KIND_SPRING,
} an_ease_kind;

typedef struct an_ease {
    an_ease_kind kind;
    float damping;  /* springs: damping ratio in (0, 1]; 1 = critically damped */
    float response; /* springs: undamped period in seconds */
} an_ease;

#define AN_EASE_IN_OUT ((an_ease){ .kind = AN_EASE_KIND_IN_OUT })
#define AN_EASE_LINEAR ((an_ease){ .kind = AN_EASE_KIND_LINEAR })
an_ease an_spring(float damping_ratio, float response_seconds);

typedef struct an_anim_opts {
    float duration; /* seconds; 0 = 1 s, or for springs the settle time */
    an_ease ease;   /* zero value = AN_EASE_IN_OUT */
} an_anim_opts;

/* A description of an animation; nothing happens until it's played. */
typedef struct an_anim {
    an_obj *obj;
    an_scene *scene;  /* for scene-target anims (obj is NULL) */
    an_prop prop;
    bool relative;   /* value is a delta from the value at the start */
    bool has_from;   /* start from `from` instead of the current value */
    float value[4];
    float from[4];
    an_anim_opts opts;
} an_anim;

an_anim an_move_to(an_obj *o, an_vec2 to, const an_anim_opts *opts);
an_anim an_move_by(an_obj *o, an_vec2 delta, const an_anim_opts *opts);
an_anim an_scale_to(an_obj *o, an_vec2 to, const an_anim_opts *opts);
an_anim an_rotate_to(an_obj *o, float radians, const an_anim_opts *opts);
an_anim an_rotate_by(an_obj *o, float radians, const an_anim_opts *opts);
an_anim an_fade_to(an_obj *o, float opacity, const an_anim_opts *opts);
an_anim an_fill_to(an_obj *o, an_color to, const an_anim_opts *opts);
an_anim an_stroke_to(an_obj *o, an_color to, const an_anim_opts *opts);
an_anim an_stroke_width_to(an_obj *o, float width, const an_anim_opts *opts);
/* Draw the path on: draw progress from 0 to 1, revealed by arc length. Always
 * starts from 0; create the object with .undrawn so it isn't visible before.
 * Defaults like every anim: 1 s, ease in-out. Pass AN_EASE_LINEAR for a steady pen. */
an_anim an_draw(an_obj *o, const an_anim_opts *opts);
/* Draw progress to a value, continuing from wherever it is (take-over). */
an_anim an_draw_to(an_obj *o, float progress, const an_anim_opts *opts);
an_anim an_chaos_to(an_obj *o, float chaos, const an_anim_opts *opts);
an_anim an_scene_chaos_to(an_scene *s, float chaos, const an_anim_opts *opts);

/* The camera (docs/camera.md): the scene's view of its world. The paper,
 * grain and vignette stay fixed to the frame. */
an_anim an_camera_move_to(an_scene *s, an_vec2 pos, const an_anim_opts *opts);
an_anim an_camera_zoom_to(an_scene *s, float zoom, const an_anim_opts *opts);
an_anim an_camera_rotate_to(an_scene *s, float radians, const an_anim_opts *opts);
void an_camera_set_pos(an_scene *s, an_vec2 pos);
void an_camera_set_zoom(an_scene *s, float zoom);
void an_camera_set_rotation(an_scene *s, float radians);

typedef struct an_camera_state {
    an_vec2 pos;    /* the world point at the frame's center (without shake) */
    float zoom, rotation;
    an_vec2 shake;  /* the shake's offset, screen px: exactly {0, 0} at rest */
} an_camera_state;
/* The camera at the frame last passed to an_seek. */
an_camera_state an_camera_get(const an_scene *s);

/* A camera shake starting when played: translation only, intensity in screen
 * px at full trauma, decaying as trauma^2 (trauma 1 -> 0 linearly over the
 * duration; default 0.5 s), smooth 15 Hz noise. A pure function of the frame;
 * the camera is exactly at rest from the end of the decay (docs/camera.md). */
an_anim an_shake(an_scene *s, float intensity, const an_anim_opts *opts);

/* Start at the cursor and move the cursor to the animation's end. */
void an_play(an_scene *s, an_anim a);
/* Start at the cursor; the cursor stays. */
void an_play_async(an_scene *s, an_anim a);
/* Advance the cursor. */
void an_wait(an_scene *s, float seconds);
/* Start all at the cursor; the cursor moves to the latest end. */
void an_par(an_scene *s, int n, const an_anim *anims);
/* Start anim i at cursor + i * gap; the cursor moves to the latest end. */
void an_stagger(an_scene *s, int n, const an_anim *anims, float gap_seconds);

/* The cursor, in seconds. */
double an_scene_time(const an_scene *s);
/* Last frame index L: frames 0..L (L + 1 frames) cover the whole timeline. */
int64_t an_scene_length(const an_scene *s);

/* Choose the frame that an_obj_get() reports. Sampling is a pure function of
 * the frame, so seeking in any order gives the same results. */
void an_seek(an_scene *s, int64_t frame);

typedef struct an_obj_state {
    bool exists; /* false before the object's creation time */
    an_vec2 pos, scale;
    float rotation, opacity;
    an_color fill, stroke;
    int z;
    float stroke_width, draw;
} an_obj_state;

/* The pen tip of a stroke being drawn on, at the frame last passed to an_seek.
 * A pure function of (path, frame): position is the point at arc length
 * draw * length; speed comes from the analytic derivative of the draw track's
 * easing. Object motion (moving parents) is not included in the speed. */
typedef struct an_nozzle {
    bool active;   /* exists, and the draw progress is strictly between 0 and 1 */
    an_vec2 pos;   /* scene pixels */
    an_vec2 dir;   /* unit tangent, scene space, direction of drawing */
    float speed;   /* scene pixels per second along the path */
    float width;   /* stroke width at the nozzle, scene pixels */
} an_nozzle;

an_nozzle an_obj_nozzle(const an_obj *o);

/* Chaos at the frame last passed to an_seek: the scene's dial, and an object's
 * effective chaos (scene dial x the multipliers from the top level down to it). */
float an_scene_chaos(const an_scene *s);
float an_obj_chaos(const an_obj *o);

/* The object's own (parent-relative) state at the frame last passed to an_seek. */
an_obj_state an_obj_get(const an_obj *o);

/* ---- output ------------------------------------------------------------- */

/* Open a window playing the scene in a loop, letterboxed to its aspect ratio.
 * Space pauses, left/right step a frame while paused. The wall clock only
 * picks which frame to show. On macOS this does not return. */
void an_scene_preview(an_scene *s);

/* Export quality (docs/progress.md, "Bitrate report"). Both are H.264 in mp4,
 * BT.709 limited range, color-tagged, with +faststart. */
typedef enum an_export_quality {
    AN_EXPORT_SHARE = 0, /* 4:2:0, High profile: plays everywhere (default) */
    AN_EXPORT_MASTER,    /* 4:4:4, High 4:4:4: full color resolution, near
                            lossless at its default CRF; for re-editing */
} an_export_quality;
#define AN_EXPORT_SHARE_CRF 18
#define AN_EXPORT_MASTER_CRF 12

typedef struct an_export_desc {
    const char *path; /* default "out.mp4" */
    int64_t frames;   /* default: an_scene_length(s) + 1, the whole timeline */
    int crf;          /* x264 quality; 0 = the quality's default (above) */
    an_export_quality quality;
} an_export_desc;

/* Render frames 0..frames-1 offscreen and encode H.264 mp4 through ffmpeg
 * (must be on PATH). By default that's 0..L inclusive, so the last frame shows
 * the final state. Returns the number of frames written, or -1 on failure
 * (with a message on stderr). */
int64_t an_scene_export(an_scene *s, const an_export_desc *desc);

/* Render one frame offscreen and write it as a PNG. Returns false on failure. */
bool an_scene_save_png(an_scene *s, int64_t frame, const char *path);

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
