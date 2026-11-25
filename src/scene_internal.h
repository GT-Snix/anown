// Internal: scene and object layout.
#ifndef ANOWN_SCENE_INTERNAL_H
#define ANOWN_SCENE_INTERNAL_H

#include "anown.h"
#include "arena_internal.h"
#include "path_internal.h"

typedef struct ani_channel ani_channel; // timeline_internal.h
typedef struct ani_glyph ani_glyph;     // font_internal.h

struct an_obj {
    an_kind kind;
    uint32_t id; // creation order within the scene, 1-based; root is 0
    an_scene *scene;
    const char *name;   // NULL if unnamed; unique in the scene
    an_obj *next_named; // list of named objects, for duplicate checks
    an_rng rng;         // keyed by name hash if named, else by id
    uint64_t rng_key;   // that key (also keys the line boil noise)
    int64_t birth;      // cursor (ticks) when created; not drawn before

    // BASE state (what the script set at creation). Never changed by sampling;
    // the timeline layers tracks over it. See docs/timeline.md.
    an_vec2 pos;
    an_vec2 scale;
    float rotation;
    float opacity;
    int z;
    an_color fill;
    an_color stroke;
    float stroke_width;
    float draw;         // draw progress 0..1
    float chaos;        // chaos multiplier, default 1

    // geometry and stroke style (fixed at creation)
    ani_path *path;     // outline / centerline; NULL for groups and text
    ani_brush brush;
    bool has_stroke;    // draw the outline
    bool no_fill;       // rect/circle: skip the fill
    an_toggle riso;     // fill offset override
    an_toggle shadow;   // hard shadow override
    an_pattern pattern; // fill pattern (defaults applied)
    bool paper_under;   // fill the silhouette with the background color first
    // letters: properties set on the letter itself (base writes), by bit
    // (1 << an_prop). Those no longer follow the text (docs/text.md).
    uint32_t own_base;
    // the verb transform's BASE state (docs/camera.md)
    an_vec2 vscale, vpivot; // vpivot: the object's center, set at creation
    float vrot;
    int cell_state;     // array cells: an_cell_state (base of AN_PROP_CELL_STATE)

    // tracks per animatable property; NULL until the first track
    ani_channel *ch[AN_PROP_COUNT];

    // tree: doubly linked sibling list under parent
    an_obj *parent; // never NULL except for the scene root
    an_obj *first_child, *last_child;
    an_obj *prev_sibling, *next_sibling;

    union {
        struct { an_vec2 size; } rect;
        struct { float radius; } circle;
        struct { an_vec2 from, to; } line; // also arrows
        // AN_KIND_TEXT: the node that holds the letters (docs/text.md)
        struct {
            const char *str;
            float size;            // em px
            an_font *font;
            an_obj **glyphs;       // letters in string order
            int nglyphs;
            float total_arc;       // sum of the letters' outline lengths (draw-on)
            bool size_chaos;       // chaos x the stem-width factor
            bool inked;            // letters have outlines: draw-on by arc length
        } text;
        // AN_KIND_GLYPH: one letter. Its path is the glyph's cached outline scaled
        // to the size, with the pivot at the local origin; pos = pen + pivot.
        struct {
            const ani_glyph *glyph;
            an_obj *text;          // the text it was laid out in
            int index;             // in the text's letters
            an_vec2 pen;           // layout pen position (baseline), text space
            an_vec2 pivot;         // pivot relative to the pen
            float arc0;            // outline length of the letters before it
            float stem_px;         // stem width at scale 1, px
            float em_px;           // em size, px
        } glyph;
        // AN_KIND_CODE: a code block
        struct {
            an_obj **bars, **texts;
            int nlines, first_line;
            an_obj *marker;          // an_code_marker, or NULL
            float base0, pitch;      // line 1's baseline (block space), line pitch
            float asc, desc, code_x, code_w, adv;
            int current;             // script time: the last line an_code_goto recorded (0: none)
        } code;
        // AN_KIND_CALLOUT: its parts
        struct {
            an_obj *label, *arrow;
        } callout;
        // AN_KIND_ARRAY (docs/explainer.md)
        struct {
            int n;
            an_obj **elems;          // element groups, by element index
            an_obj **cells;          // their cell / bar rects
            int *perm;               // script time: slot -> element index (swaps played so far)
            int64_t *busy;           // script time: per slot, the end tick of the last swap on it
            float *slot_x;           // slot centers (array space)
            float *height;           // per element: cell height, or bar height
            an_vec2 cell;
            an_array_style style;
            an_color fill;           // the array's cell fill (unhighlight goes back to it)
            an_color states[4];      // colors of the named states
        } array;
    } u;
};

struct an_scene {
    int width, height, fps;
    uint64_t seed;
    an_color background;
    an_rng rng;      // the scene's own stream (key ANI_RNG_SCENE_KEY)
    float chaos;     // BASE chaos dial (scene target), default 1
    an_vec2 cam_pos; // BASE camera (docs/camera.md): position, zoom, rotation
    float cam_zoom, cam_rot;
    struct ani_shake *shakes; // camera shake events in recording order (arena)
    int nshakes, shake_cap;
    float max_zoom;  // curves are flattened for this scale on screen
    float flatness;  // path flattening tolerance, local px: 0.2 / max_zoom
    an_style style;  // resolved: defaults applied (docs/layers.md)
    ani_channel *ch[AN_PROP_COUNT]; // scene-target tracks (only AN_PROP_CHAOS so far)
    ani_arena arena;
    an_obj root;     // hidden group; top-level objects are its children
    uint32_t count;  // objects created, not counting root
    uint64_t serial; // unique per scene in the process: identity for render caches
    an_obj *named;   // head of the named-object list
    an_font *font_display, *font_mono; // built-in fonts, loaded on first use

    // timeline (docs/timeline.md)
    int64_t cursor;  // ticks (microseconds)
    int64_t end;     // latest track end, ticks
    uint32_t seq;    // track recording counter
    int64_t frame;   // frame reported by an_obj_get (an_seek)
};

// ---- object creation (scene.c), shared with text.c --------------------------------

// The fields every *_opts struct starts with (AN_OBJ_FIELDS).
typedef struct ani_common_opts {
    AN_OBJ_FIELDS
} ani_common_opts;

// Copy the shared leading fields of any *_opts struct (all begin with
// AN_OBJ_FIELDS, so they share a common initial sequence).
#define ANI_COMMON(o) (&(ani_common_opts){                                         \
    .name = (o)->name, .hidden = (o)->hidden, .parent = (o)->parent, .pos = (o)->pos, .scale = (o)->scale, \
    .rotation = (o)->rotation, .z = (o)->z, .fill = (o)->fill,                       \
    .stroke = (o)->stroke, .stroke_width = (o)->stroke_width, .undrawn = (o)->undrawn, \
    .no_fill = (o)->no_fill, .brush = (o)->brush,                                    \
    .riso = (o)->riso, .shadow = (o)->shadow, .pattern = (o)->pattern, .paper_under = (o)->paper_under })

// A new object of `kind` at the cursor, with defaults applied to c's fields.
// NULL on allocation failure, a bad parent, or a duplicate name.
an_obj *ani_obj_make(an_scene *s, an_kind kind, const ani_common_opts *c);
// Rect, circle and glyph outlines are opt-in: drawn when a stroke color or width is given.
bool ani_outline_requested(an_color stroke, float width);

#endif
