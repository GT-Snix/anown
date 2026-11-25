// 11_bst: inserting 12 random keys into a binary search tree, then a plot of
// the tree's height against n.
//   - each insertion: the new key appears at the top; the comparisons light up
//     the path from the root, node by node, with the edges between; the new
//     node pops in under its parent, its edge draws on, and the whole tree
//     glides to its new tidy layout
//   - then the tree fades and the axes draw in: the height after each insertion
//     (points), and log2(n + 1), the best a tree of n keys can do, drawn on
// The tree is a plain C BST built at script time; every step records animation.
//
//   make example-bst [EVERY=10]                    export (unattended); EVERY: also PNGs
//   ./build/examples/11_bst                        preview
//   ./build/examples/11_bst --export FILE.mp4 [--frames DIR] [--every N]
#include "anown.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 12 };

static an_scene *S;
static an_obj *tree;            // the graph: it holds every node and edge (one fade for all)
static an_obj *node[N];         // node[i]: the i-th inserted key
static int key[N], parent[N], side[N], depth[N];
static int count;

static const an_vec2 ROOT = { 960, 300 };
static const an_vec2 SPACING = { 120, 105 };

// where every node goes for the first `n` keys (relative layout + the root's spot)
static void layout(int n, an_vec2 *out) {
    an_tree_node t[N];
    for (int i = 0; i < n; i++) t[i] = (an_tree_node){ .parent = parent[i], .side = side[i] };
    if (!an_layout_tree(t, n, SPACING, out)) exit(1);
    for (int i = 0; i < n; i++) out[i] = (an_vec2){ ROOT.x + out[i].x, ROOT.y + out[i].y };
}

// a caption at the top: the old one fades out, the new one fades in
static an_obj *caption;
static void say(const char *text) {
    if (caption) an_play_async(S, an_fade_to(caption, 0, &(an_anim_opts){ .duration = 0.2f }));
    caption = an_text(S, &(an_text_opts){ .text = text, .pos = { 960, 150 }, .size = 52, .align = AN_ALIGN_CENTER,
                                          .anchor = AN_ANCHOR_MIDDLE, .font = an_font_mono(S), .hidden = true });
    an_play(S, an_fade_to(caption, 1, &(an_anim_opts){ .duration = 0.2f }));
}

static int height(void) {
    int h = 0;
    for (int i = 0; i < count; i++) h = depth[i] + 1 > h ? depth[i] + 1 : h;
    return h;
}

static void insert(int k) {
    const int me = count;
    char buf[64];
    snprintf(buf, sizeof buf, "insert %d", k);
    say(buf);

    // walk down from the root, recording the path of nodes
    an_obj *path[N];
    int np = 0, at = 0, d = 0;
    int par = -1, sd = 0;
    while (count > 0) {
        path[np++] = node[at];
        par = at;
        sd = k < key[at] ? -1 : 1;
        // the child on that side, if any
        int next = -1;
        for (int i = 0; i < count; i++)
            if (parent[i] == at && side[i] == sd) next = i;
        d++;
        if (next < 0) break;
        at = next;
    }
    if (np > 0) an_play(S, an_highlight_path(path, np, AN_STATE_ACTIVE, &(an_path_opts){ .gap = 0.3f }));

    key[me] = k;
    parent[me] = par;
    side[me] = sd;
    depth[me] = count > 0 ? d : 0;
    count++;

    // the new layout: existing nodes glide while the new node pops in place
    an_vec2 pos[N];
    layout(count, pos);
    snprintf(buf, sizeof buf, "%d", k);
    node[me] = an_graph_node(tree, &(an_node_opts){ .pos = pos[me], .radius = 38, .label = buf });
    an_anim glide[N + 1];
    int ng = 0;
    for (int i = 0; i < me; i++) glide[ng++] = an_move_to(node[i], pos[i], &(an_anim_opts){ .duration = 0.6f });
    glide[ng++] = an_pop(node[me], NULL);
    an_par(S, ng, glide);
    if (par >= 0) {
        an_obj *e = an_graph_edge(tree, node[par], node[me], &(an_edge_opts){ .undrawn = true });
        an_play(S, an_draw(e, &(an_anim_opts){ .duration = 0.35f }));
    }

    // back to normal
    if (np > 0) an_play(S, an_highlight_path(path, np, AN_STATE_BASE, &(an_path_opts){ .together = true, .duration = 0.25f }));
    an_wait(S, 0.2f);
}

static float best(float n, void *user) {
    (void)user;
    return log2f(n + 1.0f);
}

int main(int argc, char **argv) {
    const an_palette *pal = &AN_PALETTE_RIOT;
    S = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 11, .style = { .palette = pal, .shadow = true } });
    if (!S) return 1;

    // 12 distinct keys 1..99 from the scene's seeded stream
    an_rng *rng = an_scene_rng(S);
    int keys[N];
    for (int i = 0; i < N; i++) {
        bool dup;
        do {
            keys[i] = an_rng_int(rng, 1, 99);
            dup = false;
            for (int j = 0; j < i; j++) dup = dup || keys[j] == keys[i];
        } while (dup);
    }

    an_obj *title = an_text(S, &(an_text_opts){ .text = "BINARY SEARCH TREE", .pos = { 960, 70 }, .size = 84,
                                                 .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE, .fill = pal->b,
                                                 .stroke = pal->ink, .stroke_width = 6 });
    an_wait(S, 0.3f);
    for (int k = 0; k < an_text_length(title); k++) {
        an_play_async(S, an_slap(an_text_glyph(title, k), &(an_verb_opts){ .enter = AN_ON }));
        an_wait(S, 0.05f);
    }
    an_wait(S, 0.6f);

    tree = an_graph(S, NULL);
    float heights[N], ns[N];
    for (int i = 0; i < N; i++) {
        insert(keys[i]);
        ns[i] = (float)(i + 1);
        heights[i] = (float)height();
    }
    say("height: the longest root-to-leaf path");
    an_wait(S, 1.2f);

    // the plot
    an_play_async(S, an_fade_to(caption, 0, &(an_anim_opts){ .duration = 0.4f }));
    an_play(S, an_fade_to(tree, 0, &(an_anim_opts){ .duration = 0.5f }));
    an_obj *ax = an_axes(S, &(an_axes_opts){ .pos = { 360, 220 }, .size = { 1200, 640 }, .x_max = N, .y_max = 8,
                                             .y_step = 1, .x_label = "keys inserted (n)", .y_label = "tree height",
                                             .undrawn = true });
    an_play(S, an_draw(ax, &(an_anim_opts){ .duration = 0.8f }));
    an_obj *pts = an_plot_data(ax, ns, heights, N, &(an_plot_opts){ .fill = pal->a, .size = 11, .riso = AN_OFF });
    for (int i = 0; i < N; i++) {
        an_play_async(S, an_pop(an_plot_item(pts, i), NULL));
        an_wait(S, 0.12f);
    }
    an_wait(S, 0.4f);
    an_obj *curve = an_plot_function(ax, best, NULL, &(an_plot_opts){ .stroke = pal->b, .stroke_width = 6, .undrawn = true });
    an_play(S, an_draw(curve, &(an_anim_opts){ .duration = 1.2f }));
    // the callout points at the curve at n = 10; points are in the axes' space, the
    // callout wants scene space, so add the axes' position
    const an_vec2 tip = an_axes_point(ax, 10, best(10, NULL));
    an_obj *note = an_callout(S, &(an_callout_opts){ .text = "best case: log2(n+1)", .size = 40,
                                                       .pos = { 360 + tip.x - 60, 220 + tip.y + 170 },
                                                       .target = { 360 + tip.x, 220 + tip.y + 14 }, .bend = 0.3f,
                                                       .undrawn = true });
    an_play(S, an_draw(note, &(an_anim_opts){ .duration = 0.7f }));
    an_wait(S, 2.0f);

    printf("keys:");
    for (int i = 0; i < N; i++) printf(" %d", keys[i]);
    printf("\nheights:");
    for (int i = 0; i < N; i++) printf(" %.0f", heights[i]);
    printf("\ntimeline %.2f s, frames 0..%lld\n", an_scene_time(S), (long long)an_scene_length(S));

    const char *mp4 = NULL, *dir = NULL;
    int every = 10;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (strcmp(argv[k], "--export") == 0) mp4 = argv[k + 1];
        else if (strcmp(argv[k], "--frames") == 0) dir = argv[k + 1];
        else if (strcmp(argv[k], "--every") == 0) every = atoi(argv[k + 1]);
    }
    int rc = 0;
    if (mp4) {
        const int64_t n = an_scene_export(S, &(an_export_desc){ .path = mp4 });
        if (n > 0) printf("wrote %s (%lld frames)\n", mp4, (long long)n);
        rc = n > 0 ? 0 : 1;
    }
    if (dir && rc == 0) {
        const int64_t n = an_scene_save_frames(S, &(an_frames_desc){ .dir = dir, .prefix = "bst", .every = every });
        if (n >= 0) printf("wrote %lld PNG frames to %s (every %d)\n", (long long)n, dir, every);
        rc = n >= 0 ? 0 : 1;
    }
    if (!mp4 && !dir) an_scene_preview(S);
    an_scene_destroy(S);
    return rc;
}
