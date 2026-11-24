// Internal: scene and object layout.
#ifndef ANOWN_SCENE_INTERNAL_H
#define ANOWN_SCENE_INTERNAL_H

#include "anown.h"
#include "arena_internal.h"

struct an_obj {
    an_kind kind;
    uint32_t id; // creation order within the scene, 1-based; root is 0
    an_scene *scene;

    // transform, relative to parent
    an_vec2 pos;
    an_vec2 scale;
    float rotation;
    int z;

    // style (only fill, and stroke for lines, are drawn so far)
    an_color fill;
    an_color stroke;
    float stroke_width;

    // tree: doubly linked sibling list under parent
    an_obj *parent; // never NULL except for the scene root
    an_obj *first_child, *last_child;
    an_obj *prev_sibling, *next_sibling;

    union {
        struct { an_vec2 size; } rect;
        struct { float radius; } circle;
        struct { an_vec2 from, to; } line;
        struct { const char *str; float size; } text;
    } u;
};

struct an_scene {
    int width, height, fps;
    uint64_t seed;
    an_color background;
    ani_arena arena;
    an_obj root;     // hidden group; top-level objects are its children
    uint32_t count;  // objects created, not counting root
};

#endif
