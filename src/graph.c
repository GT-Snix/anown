// Graphs (docs/graphs.md): nodes, edges (endpoints resolved each frame by the
// tessellator), highlight states and paths.
#include "scene_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static an_color mix(an_color a, an_color b, float t) {
    return (an_color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f };
}

// A rounded rect centered on the origin: four quarter arcs (cubic, k = 0.5523)
static ani_path *rounded_path(ani_arena *arena, an_vec2 size, float r, float tol) {
    const float hw = size.x * 0.5f, hh = size.y * 0.5f, k = 0.5522847f * r;
    ani_path_builder b;
    ani_pb_init(&b);
    bool ok = ani_pb_move(&b, (an_vec2){ -hw + r, -hh });
    ok = ok && ani_pb_line(&b, (an_vec2){ hw - r, -hh });
    ok = ok && ani_pb_cubic(&b, (an_vec2){ hw - r + k, -hh }, (an_vec2){ hw, -hh + r - k }, (an_vec2){ hw, -hh + r }, tol);
    ok = ok && ani_pb_line(&b, (an_vec2){ hw, hh - r });
    ok = ok && ani_pb_cubic(&b, (an_vec2){ hw, hh - r + k }, (an_vec2){ hw - r + k, hh }, (an_vec2){ hw - r, hh }, tol);
    ok = ok && ani_pb_line(&b, (an_vec2){ -hw + r, hh });
    ok = ok && ani_pb_cubic(&b, (an_vec2){ -hw + r - k, hh }, (an_vec2){ -hw, hh - r + k }, (an_vec2){ -hw, hh - r }, tol);
    ok = ok && ani_pb_line(&b, (an_vec2){ -hw, -hh + r });
    ok = ok && ani_pb_cubic(&b, (an_vec2){ -hw, -hh + r - k }, (an_vec2){ -hw + r - k, -hh }, (an_vec2){ -hw + r, -hh }, tol);
    ani_pb_close(&b);
    ani_path *p = ok ? ani_pb_finish(&b, arena) : NULL;
    ani_pb_free(&b);
    return p;
}

an_obj *an_node(an_scene *s, const an_node_opts *opts) {
    const an_node_opts o = opts ? *opts : (an_node_opts){ 0 };
    if (!s) return NULL;
    const an_palette *pal = s->style.palette;
    an_obj *n = ani_obj_make(s, AN_KIND_NODE, &(ani_common_opts){
        .name = o.name, .hidden = o.hidden, .undrawn = o.undrawn, .parent = o.parent, .pos = o.pos, .scale = o.scale,
        .rotation = o.rotation, .z = o.z });
    if (!n) return NULL;
    const an_color fill = o.fill.a > 0.0f ? o.fill : pal->a;
    const an_toggle pu = o.paper_under != AN_AUTO ? o.paper_under
                         : (o.pattern.kind != AN_PATTERN_SOLID ? AN_ON : AN_OFF);
    const an_color stroke = o.stroke.a > 0.0f ? o.stroke : pal->ink;
    const float sw = o.stroke_width > 0.0f ? o.stroke_width : 5.0f;
    an_obj *shape;
    float label_size;
    n->u.node.kind = o.shape;
    if (o.shape == AN_NODE_ROUNDED) {
        const an_vec2 size = (o.size.x > 0 && o.size.y > 0) ? o.size : (an_vec2){ 120, 70 };
        const float r = 0.25f * fminf(size.x, size.y);
        ani_path *path = rounded_path(&s->arena, size, r, s->flatness);
        if (!path) return NULL;
        shape = ani_obj_with_path(ani_obj_make(s, AN_KIND_POLYGON, &(ani_common_opts){
                                      .parent = n, .fill = fill, .stroke = stroke, .stroke_width = sw,
                                      .pattern = o.pattern, .paper_under = pu, .riso = o.riso, .shadow = o.shadow,
                                      .brush = o.brush, .undrawn = o.undrawn }),
                                  path, true);
        n->u.node.size = size;
        label_size = size.y * 0.4f;
    } else {
        const float r = o.radius > 0.0f ? o.radius : 40.0f;
        shape = an_circle(s, &(an_circle_opts){ .parent = n, .radius = r, .fill = fill, .stroke = stroke,
                                                .stroke_width = sw, .pattern = o.pattern, .paper_under = pu,
                                                .riso = o.riso, .shadow = o.shadow, .brush = o.brush,
                                                .undrawn = o.undrawn });
        n->u.node.radius = r;
        label_size = r * 0.75f;
    }
    if (!shape) return NULL;
    ani_obj_part(shape, "shape", 0);
    n->u.node.shape = shape;
    if (o.label) {
        an_obj *t = an_text(s, &(an_text_opts){ .parent = n, .text = o.label, .font = o.font ? o.font : an_font_mono(s),
                                                 .size = label_size, .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                                 .fill = pal->ink });
        if (!t) return NULL;
        ani_obj_part(t, "label", 0);
        n->u.node.label = t;
    }
    const an_color defaults[4] = { fill, pal->b, mix(s->background, pal->ink, 0.12f), pal->c };
    for (int k = 0; k < 4; k++) n->u.node.states[k] = o.state_colors[k].a > 0.0f ? o.state_colors[k] : defaults[k];
    return n;
}

// ---- states -----------------------------------------------------------------------------

static an_anim state_anim(an_obj *o, an_verb_kind kind, an_state st, const an_anim_opts *opts) {
    an_anim x = { .obj = o, .verb = kind, .swap_j = (int)st };
    if (opts) x.opts = *opts;
    if (!(x.opts.duration > 0.0f)) x.opts.duration = 0.3f;
    return x;
}

an_anim an_node_set_state(an_obj *n, an_state st, const an_anim_opts *opts) {
    return state_anim(n, AN_VERB_NODE_STATE, st, opts);
}

an_anim an_edge_set_state(an_obj *e, an_state st, const an_anim_opts *opts) {
    return state_anim(e, AN_VERB_EDGE_STATE, st, opts);
}

an_state an_edge_state_at_cursor(const an_obj *e) {
    if (!e || e->kind != AN_KIND_EDGE) return AN_STATE_BASE;
    const ani_value v = ani_sample(ani_obj_prop((an_obj *)e, AN_PROP_STATE), ani_tick_time(e->scene->cursor));
    return (an_state)(int)v.v[0];
}

an_state an_node_state_at_cursor(const an_obj *n) {
    if (!n || n->kind != AN_KIND_NODE) return AN_STATE_BASE;
    const ani_value v = ani_sample(ani_obj_prop(n->u.node.shape, AN_PROP_STATE), ani_tick_time(n->scene->cursor));
    return (an_state)(int)v.v[0];
}

// Record one object's state (node or edge) at `start`; returns its end.
static int64_t record_state(an_scene *s, an_obj *o, int st, const an_anim_opts *opts, int64_t start) {
    if (st < 0 || st > 3 || !o) return start;
    if (o->kind == AN_KIND_NODE) {
        an_obj *shape = o->u.node.shape;
        ani_record(s, ani_obj_prop(shape, AN_PROP_STATE), start, 0, AN_EASE_LINEAR, (ani_value){ { (double)st } },
                   false, NULL);
        const an_anim f = an_fill_to(shape, o->u.node.states[st], opts);
        return ani_record_anim(s, &f, start);
    }
    if (o->kind == AN_KIND_EDGE) {
        ani_record(s, ani_obj_prop(o, AN_PROP_STATE), start, 0, AN_EASE_LINEAR, (ani_value){ { (double)st } },
                   false, NULL);
        const an_anim c = an_stroke_to(o, o->u.edge.states[st], opts);
        const float wmul = (st == AN_STATE_ACTIVE || st == AN_STATE_DONE) ? 1.5f : 1.0f;
        const an_anim w = an_stroke_width_to(o, o->u.edge.width * wmul, opts);
        const int64_t e1 = ani_record_anim(s, &c, start), e2 = ani_record_anim(s, &w, start);
        if (o->u.edge.head) { // the arrowhead in the same color
            const an_anim h = an_fill_to(o->u.edge.head, o->u.edge.states[st], opts);
            ani_record_anim(s, &h, start);
        }
        return e1 > e2 ? e1 : e2;
    }
    return start;
}

int64_t ani_graph_record_state(an_scene *s, const an_anim *a, int64_t start) {
    return record_state(s, a->obj, a->swap_j, &a->opts, start);
}

an_anim an_highlight_path(an_obj *const *nodes, int n, an_state st, const an_path_opts *opts) {
    const an_path_opts po = opts ? *opts : (an_path_opts){ 0 };
    an_anim x = { .verb = AN_VERB_PATH, .swap_j = (int)st, .gap = po.together ? 0.0f : po.gap > 0.0f ? po.gap : 0.25f };
    x.opts.duration = po.duration > 0.0f ? po.duration : 0.3f;
    x.opts.ease = po.ease;
    if (!nodes || n <= 0 || !nodes[0]) return x;
    const an_obj *g = nodes[0]->parent;
    if (!g || g->kind != AN_KIND_GRAPH) {
        fprintf(stderr, "anown: an_highlight_path: the nodes must belong to a graph (an_graph_node)\n");
        return x;
    }
    an_scene *s = nodes[0]->scene;
    // node, edge, node, ...: at most 2n - 1 objects
    x.objs = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)(2 * n - 1), alignof(an_obj *));
    if (!x.objs) return x;
    int k = 0;
    bool reported = false;
    for (int i = 0; i < n; i++) {
        if (!nodes[i] || nodes[i]->parent != g) {
            fprintf(stderr, "anown: an_highlight_path: node %d isn't in the first node's graph; skipped\n", i);
            continue;
        }
        x.objs[k++] = nodes[i];
        if (i + 1 >= n || !nodes[i + 1]) continue;
        an_obj *e = an_graph_edge_between(g, nodes[i], nodes[i + 1]);
        if (e) x.objs[k++] = e;
        else if (!reported) {
            fprintf(stderr, "anown: an_highlight_path: no edge between nodes %d and %d; skipped\n", i, i + 1);
            reported = true;
        }
    }
    x.swap_i = k;
    x.obj = nodes[0]; // the scene comes from it
    return x;
}

int64_t ani_graph_record_path(an_scene *s, const an_anim *a, int64_t start) {
    int64_t end = start;
    const int64_t gap = ani_ticks(a->gap);
    for (int i = 0; a->objs && i < a->swap_i; i++) {
        const int64_t e = record_state(s, a->objs[i], a->swap_j, &a->opts, start + (int64_t)i * gap);
        end = e > end ? e : end;
    }
    return end;
}

// ---- edges -------------------------------------------------------------------------------

static bool inside_edge(const an_obj *o) {
    for (const an_obj *c = o; c; c = c->parent)
        if (c->kind == AN_KIND_EDGE) return true;
    return false;
}

an_obj *an_edge(an_scene *s, const an_edge_opts *opts) {
    const an_edge_opts o = opts ? *opts : (an_edge_opts){ 0 };
    if (!s) return NULL;
    if (!o.from || !o.to || o.from == o.to || o.from->scene != s || o.to->scene != s) {
        fprintf(stderr, "anown: an_edge needs two different nodes of this scene\n");
        return NULL;
    }
    if (inside_edge(o.from) || inside_edge(o.to)) { // its endpoints would depend on an edge: a cycle
        fprintf(stderr, "anown: an_edge: an endpoint is an edge or inside one (cycles aren't allowed)\n");
        return NULL;
    }
    const an_palette *pal = s->style.palette;
    ani_common_opts c = *ANI_COMMON(&o);
    c.stroke = o.stroke.a > 0.0f ? o.stroke : pal->ink;
    c.stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 5.0f;
    an_obj *e = ani_obj_make(s, AN_KIND_EDGE, &c);
    if (!e) return NULL;
    e->has_stroke = true;
    e->u.edge.from = o.from;
    e->u.edge.to = o.to;
    e->u.edge.directed = o.directed;
    e->u.edge.bend = o.bend < -1.0f ? -1.0f : o.bend > 1.0f ? 1.0f : o.bend;
    e->u.edge.gap = o.gap > 0.0f ? o.gap : 6.0f;
    e->u.edge.width = c.stroke_width;
    const an_color states[4] = { c.stroke, pal->b, mix(s->background, pal->ink, 0.3f), pal->c };
    for (int k = 0; k < 4; k++) e->u.edge.states[k] = o.state_colors[k].a > 0.0f ? o.state_colors[k] : states[k];
    if (o.directed) {
        // the head: a triangle with its tip at the origin pointing along +x; the
        // tessellator visits it with the resolved tip frame as its parent transform
        an_obj *h = an_polygon(s, &(an_polygon_opts){
            .parent = e, .fill = c.stroke, .undrawn = o.undrawn, .shadow = o.shadow,
            .points = (an_vec2[]){ { 0, 0 }, { -18, -8 }, { -18, 8 } }, .count = 3 });
        if (!h) return NULL;
        ani_obj_part(h, "head", 0);
        e->u.edge.head = h;
    }
    return e;
}

static ani_affine inverse(ani_affine m) {
    const float det = m.a * m.d - m.b * m.c;
    if (!(fabsf(det) > 0.0f)) return (ani_affine){ 1, 0, 0, 1, 0, 0 };
    const float ia = m.d / det, ib = -m.b / det, ic = -m.c / det, id = m.a / det;
    return (ani_affine){ ia, ib, ic, id, -(ia * m.tx + ic * m.ty), -(ib * m.tx + id * m.ty) };
}

// The point on node n's outline in direction d (parent-local), gap further on,
// and the node's center: L maps n's local space to the edge's parent space.
static an_vec2 outline_point(const an_obj *n, ani_affine L, an_vec2 d, float gap) {
    const an_vec2 c = { L.tx, L.ty };
    const float dl = sqrtf(d.x * d.x + d.y * d.y);
    if (!(dl > 0.0f)) return c;
    // the direction in the node's own space
    const ani_affine Li = inverse((ani_affine){ L.a, L.b, L.c, L.d, 0, 0 });
    const an_vec2 ld = { Li.a * d.x + Li.c * d.y, Li.b * d.x + Li.d * d.y };
    float s = 0.0f;
    const an_obj *shape = n->kind == AN_KIND_NODE ? n->u.node.shape : n;
    if (n->kind == AN_KIND_NODE && n->u.node.kind == AN_NODE_CIRCLE) s = n->u.node.radius / sqrtf(ld.x * ld.x + ld.y * ld.y);
    else if (shape->kind == AN_KIND_CIRCLE) s = shape->u.circle.radius / sqrtf(ld.x * ld.x + ld.y * ld.y);
    else {
        an_vec2 half = { 0, 0 };
        if (n->kind == AN_KIND_NODE) half = (an_vec2){ n->u.node.size.x * 0.5f, n->u.node.size.y * 0.5f };
        else if (n->kind == AN_KIND_RECT) half = (an_vec2){ n->u.rect.size.x * 0.5f, n->u.rect.size.y * 0.5f };
        const float sx = fabsf(ld.x) > 0.0f ? half.x / fabsf(ld.x) : INFINITY, sy = fabsf(ld.y) > 0.0f ? half.y / fabsf(ld.y) : INFINITY;
        s = fminf(sx, sy);
        if (!isfinite(s)) s = 0.0f;
    }
    const an_vec2 on = ani_affine_apply(L, (an_vec2){ ld.x * s, ld.y * s });
    return (an_vec2){ on.x + d.x / dl * gap, on.y + d.y / dl * gap };
}

bool ani_edge_resolve(const an_obj *e, ani_time at, an_vec2 *p0, an_vec2 *p1, an_vec2 *ctrl, an_vec2 *dir) {
    an_obj *a = e->u.edge.from, *b = e->u.edge.to;
    if (!ani_obj_exists(a, at) || !ani_obj_exists(b, at)) return false;
    const ani_affine Pi = e->parent && e->parent->parent ? inverse(ani_obj_world_at(e->parent, at))
                                                         : (ani_affine){ 1, 0, 0, 1, 0, 0 };
    const ani_affine La = ani_affine_mul(Pi, ani_obj_world_at(a, at)), Lb = ani_affine_mul(Pi, ani_obj_world_at(b, at));
    const an_vec2 ca = { La.tx, La.ty }, cb = { Lb.tx, Lb.ty };
    const an_vec2 d = { cb.x - ca.x, cb.y - ca.y };
    *p0 = outline_point(a, La, d, e->u.edge.gap);
    *p1 = outline_point(b, Lb, (an_vec2){ -d.x, -d.y }, e->u.edge.gap);
    // the bend: the chord's middle pushed along its up-screen normal by bend x half the chord
    const float dx = p1->x - p0->x, dy = p1->y - p0->y, len = sqrtf(dx * dx + dy * dy);
    *ctrl = (an_vec2){ (p0->x + p1->x) * 0.5f, (p0->y + p1->y) * 0.5f };
    if (len > 0.0f && e->u.edge.bend != 0.0f) {
        float nx = dy / len, ny = -dx / len;
        if (ny > 0.0f || (ny == 0.0f && nx > 0.0f)) {
            nx = -nx;
            ny = -ny;
        }
        ctrl->x += nx * e->u.edge.bend * len * 0.5f;
        ctrl->y += ny * e->u.edge.bend * len * 0.5f;
    }
    const float tx = p1->x - ctrl->x, ty = p1->y - ctrl->y, tl = sqrtf(tx * tx + ty * ty);
    *dir = tl > 0.0f ? (an_vec2){ tx / tl, ty / tl } : (an_vec2){ 1, 0 };
    return true;
}

// ---- graphs ------------------------------------------------------------------------------

an_obj *an_graph(an_scene *s, const an_graph_opts *opts) {
    const an_graph_opts o = opts ? *opts : (an_graph_opts){ 0 };
    if (!s) return NULL;
    an_obj *g = ani_obj_make(s, AN_KIND_GRAPH, &(ani_common_opts){
        .name = o.name, .hidden = o.hidden, .parent = o.parent, .pos = o.pos, .scale = o.scale, .rotation = o.rotation,
        .z = o.z });
    if (!g) return NULL;
    g->u.graph.directed = o.directed;
    memcpy(g->u.graph.states, o.state_colors, sizeof g->u.graph.states);
    return g;
}

// grow an arena array by copying (the old block stays in the arena until the scene goes)
static void *grow(ani_arena *a, void *old, int n, int *cap, size_t elem) {
    if (n < *cap) return old;
    const int nc = *cap ? *cap * 2 : 16;
    void *p = ani_arena_alloc(a, elem * (size_t)nc, 16);
    if (!p) return NULL;
    if (old && n) memcpy(p, old, elem * (size_t)n);
    *cap = nc;
    return p;
}

static bool owns(const an_obj *g, const an_obj *n) {
    for (int i = 0; i < g->u.graph.nn; i++)
        if (g->u.graph.nodes[i] == n) return true;
    return false;
}

an_obj *an_graph_node(an_obj *g, const an_node_opts *opts) {
    if (!g || g->kind != AN_KIND_GRAPH) return NULL;
    an_node_opts o = opts ? *opts : (an_node_opts){ 0 };
    o.parent = g;
    for (int k = 1; k < 4; k++)
        if (!(o.state_colors[k].a > 0.0f)) o.state_colors[k] = g->u.graph.states[k];
    an_obj *n = an_node(g->scene, &o);
    if (!n) return NULL;
    an_obj **nodes = grow(&g->scene->arena, g->u.graph.nodes, g->u.graph.nn, &g->u.graph.ncap, sizeof(an_obj *));
    if (!nodes) return NULL;
    g->u.graph.nodes = nodes;
    nodes[g->u.graph.nn++] = n;
    return n;
}

an_obj *an_graph_edge(an_obj *g, an_obj *a, an_obj *b, const an_edge_opts *opts) {
    if (!g || g->kind != AN_KIND_GRAPH) return NULL;
    if (!a || !b || !owns(g, a) || !owns(g, b)) {
        fprintf(stderr, "anown: an_graph_edge: both ends must be nodes of this graph (an_graph_node)\n");
        return NULL;
    }
    for (int i = 0; i < g->u.graph.ne; i++)
        if (g->u.graph.edges[i].a == a && g->u.graph.edges[i].b == b) {
            fprintf(stderr, "anown: an_graph_edge: that edge already exists; returning it\n");
            return g->u.graph.edges[i].edge;
        }
    an_edge_opts o = opts ? *opts : (an_edge_opts){ 0 };
    o.parent = g;
    o.from = a;
    o.to = b;
    o.directed = o.directed || g->u.graph.directed;
    if (o.z == 0) o.z = -1; // under the nodes
    for (int k = 1; k < 4; k++)
        if (!(o.state_colors[k].a > 0.0f)) o.state_colors[k] = g->u.graph.states[k];
    an_obj *e = an_edge(g->scene, &o);
    if (!e) return NULL;
    ani_graph_edge *edges = grow(&g->scene->arena, g->u.graph.edges, g->u.graph.ne, &g->u.graph.ecap, sizeof(ani_graph_edge));
    if (!edges) return NULL;
    g->u.graph.edges = edges;
    edges[g->u.graph.ne++] = (ani_graph_edge){ a, b, e };
    return e;
}

an_obj *an_graph_edge_between(const an_obj *g, const an_obj *a, const an_obj *b) {
    if (!g || g->kind != AN_KIND_GRAPH) return NULL;
    an_obj *back = NULL;
    for (int i = 0; i < g->u.graph.ne; i++) {
        const ani_graph_edge *e = &g->u.graph.edges[i];
        if (e->a == a && e->b == b) return e->edge;
        if (!back && e->a == b && e->b == a) back = e->edge;
    }
    return back;
}
