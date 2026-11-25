// 10_quicksort: Lomuto quicksort on 9 random values, explained.
//   - the title slaps in; the array pops in cell by cell; the code panel fades in
//   - each partition: the active range stays bright, the rest dims; the pivot is
//     highlighted; pointers i and j walk the range; the code marker tracks the
//     line being executed; swaps travel along arcs
//   - each pivot lands in place and turns green; at the end the array slaps and a
//     callout says it's sorted
// The sort runs here, at script time, in plain C; every step records animation.
//
//   make example-quicksort [EVERY=10]                 export (unattended); EVERY: also PNGs
//   ./build/examples/10_quicksort                     preview
//   ./build/examples/10_quicksort --export FILE.mp4 [--frames DIR] [--every N]
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 9 };

static an_scene *S;
static an_obj *arr, *code, *pi_ptr, *pj_ptr;
static int a[N];
static bool done[N];
static an_color c_cell, c_dim, c_pivot, c_done;

// --- small helpers a user ends up writing -------------------------------------------------

// goto a code line and wait for the marker to get there
static void line(int n) { an_play(S, an_code_goto(code, n, NULL)); }

// recolor one slot (the element currently there)
static an_handle paint(int slot, an_color c, float secs) {
    return an_play_async(S, an_array_highlight(arr, slot, c, &(an_anim_opts){ .duration = secs }));
}

// the active range bright, the rest dim (done cells stay green)
static void focus(int lo, int hi) {
    an_handle h = { 0 };
    for (int k = 0; k < N; k++) {
        if (done[k]) continue;
        h = paint(k, (k >= lo && k <= hi) ? c_cell : c_dim, 0.3f);
    }
    an_wait_until(h);
    an_wait(S, 0.05f); // a beat
}

// swap two slots: the cells travel while the code marker moves to the swap line
static void swap_slots(int i, int j, int code_line) {
    const int t = a[i];
    a[i] = a[j];
    a[j] = t;
    if (i == j) {
        line(code_line);
        return;
    }
    const an_handle sw = an_play_async(S, an_array_swap(arr, i, j, NULL));
    an_wait(S, 0.15f);
    an_play(S, an_code_goto(code, code_line, NULL)); // the marker moves while the cells travel
    an_wait_until(sw);
    an_wait(S, 0.1f); // a beat
}

static int partition(int lo, int hi) {
    line(2); // pivot = a[hi], i = lo
    paint(hi, c_pivot, 0.25f);
    an_play_async(S, an_array_point(pi_ptr, lo, NULL));
    an_play(S, an_array_point(pj_ptr, lo, NULL));
    int i = lo;
    for (int j = lo; j < hi; j++) {
        an_play_async(S, an_array_point(pj_ptr, j, NULL));
        line(3); // for j
        line(4); // if (a[j] < pivot)
        an_wait(S, 0.15f);
        if (a[j] < a[hi]) {
            swap_slots(i, j, 5); // swap(a, i++, j)
            i++;
            an_play(S, an_array_point(pi_ptr, i, NULL));
        }
    }
    swap_slots(i, hi, 6); // swap(a, i, hi): the pivot lands
    done[i] = true;
    paint(i, c_done, 0.3f);
    an_play(S, an_slap(an_array_item(arr, i), &(an_verb_opts){ .amount = 1.4f }));
    line(7); // return i
    return i;
}

static void quicksort(int lo, int hi) {
    if (lo > hi) return;
    if (lo == hi) { // one element: already in place
        done[lo] = true;
        an_wait_until(paint(lo, c_done, 0.3f));
        an_wait(S, 0.05f); // a beat
        return;
    }
    focus(lo, hi);
    const int p = partition(lo, hi);
    quicksort(lo, p - 1);
    quicksort(p + 1, hi);
}

int main(int argc, char **argv) {
    const an_palette *pal = &AN_PALETTE_RIOT;
    S = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 10, .style = { .palette = pal, .shadow = true } });
    if (!S) return 1;
    c_cell = pal->a;
    c_pivot = pal->b;
    c_done = pal->c;
    c_dim = AN_HEX(0xD6D2C8); // paper, a little darker

    // 9 distinct values 1..20 from the scene's seeded stream
    an_rng *rng = an_scene_rng(S);
    for (int k = 0; k < N; k++) {
        bool dup;
        do {
            a[k] = an_rng_int(rng, 1, 20);
            dup = false;
            for (int m = 0; m < k; m++) dup = dup || a[m] == a[k];
        } while (dup);
    }

    // title: letters slap in
    an_obj *title = an_text(S, &(an_text_opts){ .text = "QUICKSORT", .pos = { 960, 92 }, .size = 120, .align = AN_ALIGN_CENTER,
                                                 .anchor = AN_ANCHOR_MIDDLE, .fill = pal->b, .stroke = pal->ink,
                                                 .stroke_width = 7, .letters_hidden = true });
    // the code panel (Lomuto partition), top left; the array low, with room for the
    // arcs above and the pointers below
    code = an_code(S, &(an_code_opts){ .pos = { 80, 190 }, .size = 26, .line_numbers = true, .hidden = true,
                                       .code = "int partition(int *a, int lo, int hi) {\n"
                                               "    int pivot = a[hi], i = lo;\n"
                                               "    for (int j = lo; j < hi; j++)\n"
                                               "        if (a[j] < pivot)\n"
                                               "            swap(a, i++, j);\n"
                                               "    swap(a, i, hi);\n"
                                               "    return i;\n"
                                               "}" });
    an_code_marker(code, NULL);
    arr = an_array(S, &(an_array_opts){ .pos = { 960, 800 }, .values = a, .count = N, .cell = { 80, 80 },
                                        .index_labels = true, .fill = c_cell });
    pi_ptr = an_array_pointer(arr, "i", 0);
    pj_ptr = an_array_pointer(arr, "j", 0);
    an_obj_set_opacity(pi_ptr, 0);
    an_obj_set_opacity(pj_ptr, 0);
    for (int k = 0; k < N; k++) an_obj_set_opacity(an_array_item(arr, k), 0); // until their pop

    // intro
    an_wait(S, 0.3f);
    for (int k = 0; k < an_text_length(title); k++) {
        an_obj *g = an_text_glyph(title, k);
        an_obj_set_opacity(g, 1);
        an_play_async(S, an_slap(g, &(an_verb_opts){ .impact_shake = k == an_text_length(title) - 1 ? 14.0f : 0.0f }));
        an_wait(S, 0.08f);
    }
    an_wait(S, 0.5f);
    for (int k = 0; k < N; k++) {
        an_obj *e = an_array_item(arr, k);
        an_obj_set_opacity(e, 1);
        an_play_async(S, an_pop(e, NULL));
        an_wait(S, 0.07f);
    }
    an_play(S, an_fade_to(code, 1, &(an_anim_opts){ .duration = 0.5f }));
    an_play_async(S, an_fade_to(pi_ptr, 1, &(an_anim_opts){ .duration = 0.3f }));
    an_play(S, an_fade_to(pj_ptr, 1, &(an_anim_opts){ .duration = 0.3f }));

    quicksort(0, N - 1);

    // sorted
    an_play_async(S, an_fade_to(pi_ptr, 0, &(an_anim_opts){ .duration = 0.3f }));
    an_play_async(S, an_fade_to(pj_ptr, 0, &(an_anim_opts){ .duration = 0.3f }));
    an_play(S, an_fade_to(an_code_marker(code, NULL), 0, &(an_anim_opts){ .duration = 0.3f }));
    an_play(S, an_slap(arr, &(an_verb_opts){ .amount = 1.3f, .impact_shake = 20.0f }));
    an_obj *done_note = an_callout(S, &(an_callout_opts){ .text = "sorted!", .pos = { 1500, 470 }, .size = 56,
                                                           .target = { 1290, 730 }, .bend = 0.35f, .fill = pal->b,
                                                           .undrawn = true });
    an_play(S, an_callout_draw(done_note, &(an_anim_opts){ .duration = 0.8f }));
    an_wait(S, 1.5f);

    printf("values:");
    for (int k = 0; k < N; k++) printf(" %d", a[k]);
    printf("\ntimeline %.2f s, frames 0..%lld\n", an_scene_time(S), (long long)an_scene_length(S));
    // --export FILE, --frames DIR, --every N (frame dumps: every Nth frame, default
    // 10; no PNGs unless --frames is given); nothing: the preview
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
        const int64_t n = an_scene_save_frames(S, &(an_frames_desc){ .dir = dir, .prefix = "quicksort", .every = every });
        if (n >= 0) printf("wrote %lld PNG frames to %s (every %d)\n", (long long)n, dir, every);
        rc = n >= 0 ? 0 : 1;
    }
    if (!mp4 && !dir) an_scene_preview(S);
    an_scene_destroy(S);
    return rc;
}
