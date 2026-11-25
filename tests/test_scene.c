// Scene and object tree: defaults, parenting, reparenting, cycle rejection,
// draw order, world transforms, deterministic tessellation. CPU only, no GPU.
#include "anown.h"
#include "check.h"
#include "path_internal.h"
#include "tess_internal.h"

#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

static int child_count(const an_obj *p) {
    int n = 0;
    for (const an_obj *c = p->first_child; c; c = c->next_sibling) n++;
    return n;
}

static void test_defaults(void) {
    an_scene *s = an_scene_create(NULL);
    CHECK(s->width == 1920 && s->height == 1080 && s->fps == 60);
    CHECK(s->background.r == AN_CREAM.r && s->background.a == 1.0f);

    an_obj *r = an_rect(s, NULL);
    CHECK(r && an_obj_kind(r) == AN_KIND_RECT);
    CHECK(r->scale.x == 1.0f && r->scale.y == 1.0f);
    CHECK(r->fill.a == 1.0f && r->fill.r == 0.0f); // AN_BLACK
    CHECK(r->u.rect.size.x == 100.0f && r->u.rect.size.y == 100.0f);
    CHECK(an_obj_parent(r) == NULL);

    an_obj *c = an_circle(s, &(an_circle_opts){ .radius = 7, .fill = AN_CREAM });
    CHECK(c->u.circle.radius == 7.0f && c->fill.r == AN_CREAM.r);

    char buf[] = "hello";
    an_obj *t = an_text(s, &(an_text_opts){ .text = buf });
    buf[0] = 'J';
    CHECK(strcmp(t->u.text.str, "hello") == 0 && t->u.text.size == 48.0f);

    an_obj *l = an_line(s, &(an_line_opts){ .to = { 10, 0 } });
    CHECK(l->stroke_width == 4.0f);

    CHECK(s->count == 4 && r->id == 1 && l->id == 4);
    an_scene_destroy(s);

    an_scene *s2 = an_scene_create(&(an_scene_desc){ .width = 640, .height = 480, .fps = 30, .seed = 7 });
    CHECK(s2->width == 640 && s2->height == 480 && s2->fps == 30 && s2->seed == 7);
    an_scene_destroy(s2);
}

static void test_tree(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *g = an_group(s, NULL);
    an_obj *a = an_rect(s, &(an_rect_opts){ .parent = g });
    an_obj *b = an_rect(s, &(an_rect_opts){ .parent = g });
    an_obj *c = an_rect(s, &(an_rect_opts){ .parent = a });

    CHECK(an_obj_parent(a) == g && an_obj_parent(b) == g && an_obj_parent(c) == a);
    CHECK(child_count(g) == 2 && g->first_child == a && g->last_child == b);
    CHECK(child_count(&s->root) == 1);

    // reparent: c moves from a to the end of g's children
    CHECK(an_obj_set_parent(c, g));
    CHECK(child_count(a) == 0 && a->first_child == NULL && a->last_child == NULL);
    CHECK(child_count(g) == 3 && g->last_child == c && c->prev_sibling == b);

    // detach from the middle keeps the sibling links consistent
    CHECK(an_obj_set_parent(b, NULL));
    CHECK(a->next_sibling == c && c->prev_sibling == a && child_count(g) == 2);
    CHECK(an_obj_parent(b) == NULL && child_count(&s->root) == 2);

    // cycles are rejected and change nothing
    CHECK(!an_obj_set_parent(g, a)); // a is g's child
    CHECK(!an_obj_set_parent(g, g));
    CHECK(an_obj_parent(g) == NULL && an_obj_parent(a) == g);

    // objects from another scene are rejected
    an_scene *other = an_scene_create(NULL);
    an_obj *foreign = an_group(other, NULL);
    CHECK(!an_obj_set_parent(a, foreign));
    CHECK(an_rect(s, &(an_rect_opts){ .parent = foreign }) == NULL);
    CHECK(s->count == 4); // the rejected rect wasn't counted
    an_scene_destroy(other);
    an_scene_destroy(s);
}

static void test_world_transform(void) {
    an_scene *s = an_scene_create(NULL);
    // group at (100,100), rotated 90 deg clockwise and scaled 2x;
    // child at local (10,0) -> rotated to (0,10) -> scaled (0,20) -> (100,120)
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 100, 100 }, .rotation = PI_F / 2, .scale = { 2, 2 } });
    an_obj *c = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 10, 0 } });
    const an_vec2 p = ani_affine_apply(ani_obj_world(c), (an_vec2){ 0, 0 });
    CHECK_NEAR(p.x, 100.0, 1e-4);
    CHECK_NEAR(p.y, 120.0, 1e-4);

    // a point on the child's local +x axis ends up further along screen +y
    const an_vec2 q = ani_affine_apply(ani_obj_world(c), (an_vec2){ 1, 0 });
    CHECK_NEAR(q.x, 100.0, 1e-4);
    CHECK_NEAR(q.y, 122.0, 1e-4);
    an_scene_destroy(s);
}

// First vertex color of the triangles belonging to the i-th drawn shape is hard
// to address generically, so draw order is checked with single-color rects:
// each rect is 6 vertices, after the 6-vertex background.
static void test_draw_order(void) {
    an_scene *s = an_scene_create(NULL);
    an_set_chaos(s, 0); // structural counts: rects are plain quads (no boil)
    an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0x010000), .z = 1 });  // id 1
    an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0x020000) });          // id 2, z 0
    an_obj *g = an_group(s, &(an_group_opts){ .z = -1 });               // id 3
    an_rect(s, &(an_rect_opts){ .parent = g, .fill = AN_HEX(0x030000), .z = 5 });
    an_rect(s, &(an_rect_opts){ .parent = g, .fill = AN_HEX(0x040000) });
    an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0x050000) });          // id 6, z 0

    ani_mesh m;
    ani_mesh_init(&m);
    CHECK(ani_tess_scene(&m, s, 0));
    CHECK(m.count == 6 * 6);
    // expected: group (z -1) first with its children by z: 0x04, 0x03;
    // then z 0 in creation order: 0x02, 0x05; then z 1: 0x01
    const int want[5] = { 4, 3, 2, 5, 1 };
    for (int i = 0; i < 5; i++) {
        const int got = (int)(m.v[6 + i * 6].r * 255.0f + 0.5f);
        CHECK(got == want[i]);
    }
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

static void test_tessellation_deterministic(void) {
    ani_mesh m1, m2;
    ani_mesh_init(&m1);
    ani_mesh_init(&m2);
    for (int run = 0; run < 2; run++) {
        an_scene *s = an_scene_create(NULL);
        an_obj *g = an_group(s, &(an_group_opts){ .pos = { 960, 540 }, .rotation = 0.3f });
        for (int i = 0; i < 20; i++) {
            an_circle(s, &(an_circle_opts){ .parent = g, .pos = { (float)i * 40, 0 }, .radius = 5.0f + (float)i });
            an_line(s, &(an_line_opts){ .parent = g, .from = { 0, 0 }, .to = { (float)i * 40, 100 } });
        }
        CHECK(ani_tess_scene(run ? &m2 : &m1, s, 0));
        an_scene_destroy(s);
    }
    CHECK(m1.count == m2.count && m1.count > 0);
    CHECK(memcmp(m1.v, m2.v, (size_t)m1.count * sizeof(ani_vertex)) == 0);
    CHECK(ani_circle_segments(1) == 16 && ani_circle_segments(1e6f) == 2048); // clamped
    CHECK(ani_circle_segments(100) == 100); // ceil(pi / acos(1 - 0.05/100)) = ceil(99.34), via bc
    ani_mesh_free(&m1);
    ani_mesh_free(&m2);
}

static void test_many_objects(void) {
    an_scene *s = an_scene_create(NULL);
    an_set_chaos(s, 0); // structural counts: rects are plain quads (no boil)
    an_obj *parent = NULL;
    for (int i = 0; i < 10000; i++) {
        an_obj *o = an_rect(s, &(an_rect_opts){ .parent = (i % 100 == 0) ? NULL : parent });
        CHECK(o != NULL);
        if (i % 100 == 0) parent = o; // 100 top-level rects, each with 99 children
    }
    CHECK(s->count == 10000 && child_count(&s->root) == 100);
    ani_mesh m;
    ani_mesh_init(&m);
    CHECK(ani_tess_scene(&m, s, 0));
    CHECK(m.count == 6 + 10000 * 6);
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

int main(void) {
    test_defaults();
    test_tree();
    test_world_transform();
    test_draw_order();
    test_tessellation_deterministic();
    test_many_objects();
    return check_done("test_scene");
}
