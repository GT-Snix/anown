// Seeded RNG: reference output, same seed -> same sequences, independent streams.
#include "anown.h"
#include "check.h"
#include "rng_internal.h"
#include "scene_internal.h"

#include <string.h>

enum { N = 1000 };

static void draw(an_rng *r, uint32_t out[N]) {
    for (int i = 0; i < N; i++) out[i] = an_rng_u32(r);
}

// The core generator must match the reference pcg32 implementation:
// pcg32-demo, pcg32_srandom_r(&rng, 42u, 54u), first six outputs.
static void test_reference_vector(void) {
    an_rng r = ani_pcg32_seed(42u, 54u);
    const uint32_t want[6] = { 0xa15c02b7u, 0x7b47f409u, 0xba1d3330u,
                               0x83d2f293u, 0xbfa4784bu, 0xcbed606eu };
    for (int i = 0; i < 6; i++) {
        const uint32_t got = an_rng_u32(&r);
        if (got != want[i]) fprintf(stderr, "  output %d: got 0x%08x want 0x%08x\n", i, got, want[i]);
        CHECK(got == want[i]);
    }
}

// Two scenes built the same way with the same seed give identical sequences,
// from the scene stream and from every object's stream.
static void test_same_seed_same_sequences(void) {
    uint32_t a[N], b[N];
    for (uint64_t seed = 0; seed < 3; seed++) {
        an_scene *s1 = an_scene_create(&(an_scene_desc){ .seed = seed * 0x123456789ull });
        an_scene *s2 = an_scene_create(&(an_scene_desc){ .seed = seed * 0x123456789ull });
        draw(an_scene_rng(s1), a);
        draw(an_scene_rng(s2), b);
        CHECK(memcmp(a, b, sizeof a) == 0);
        for (int i = 0; i < 5; i++) {
            an_obj *o1 = an_rect(s1, NULL);
            an_obj *o2 = an_rect(s2, NULL);
            draw(an_obj_rng(o1), a);
            draw(an_obj_rng(o2), b);
            CHECK(memcmp(a, b, sizeof a) == 0);
        }
        an_scene_destroy(s1);
        an_scene_destroy(s2);
    }
}

static void test_different_seeds_differ(void) {
    uint32_t a[N], b[N];
    an_scene *s1 = an_scene_create(&(an_scene_desc){ .seed = 1 });
    an_scene *s2 = an_scene_create(&(an_scene_desc){ .seed = 2 });
    draw(an_scene_rng(s1), a);
    draw(an_scene_rng(s2), b);
    CHECK(memcmp(a, b, sizeof a) != 0);
    an_scene_destroy(s1);
    an_scene_destroy(s2);
}

// Drawing from one object (or the scene) doesn't move another object's stream,
// and objects' streams differ from each other and from the scene's.
static void test_streams_independent(void) {
    uint32_t fresh[N], after[N], other[N], scene[N];
    an_scene *s1 = an_scene_create(&(an_scene_desc){ .seed = 7 });
    an_obj *a1 = an_rect(s1, NULL);
    an_obj *b1 = an_rect(s1, NULL);
    draw(an_obj_rng(b1), fresh); // b drawn first, nothing else touched

    an_scene *s2 = an_scene_create(&(an_scene_desc){ .seed = 7 });
    an_obj *a2 = an_rect(s2, NULL);
    an_obj *b2 = an_rect(s2, NULL);
    draw(an_obj_rng(a2), other);   // heavy use of a's stream...
    draw(an_scene_rng(s2), scene); // ...and of the scene's
    draw(an_obj_rng(b2), after);   // b is unaffected
    CHECK(memcmp(fresh, after, sizeof fresh) == 0);
    CHECK(memcmp(fresh, other, sizeof fresh) != 0);
    CHECK(memcmp(fresh, scene, sizeof fresh) != 0);
    CHECK(memcmp(other, scene, sizeof fresh) != 0);
    (void)a1;
    an_scene_destroy(s1);
    an_scene_destroy(s2);
}

// Adding objects after X leaves X's stream alone. Inserting an unnamed object
// before X shifts X's id and so its stream, unless X is named.
static void test_adding_objects(void) {
    uint32_t base[N], appended[N], named_a[N], named_b[N], shifted[N];

    an_scene *s1 = an_scene_create(&(an_scene_desc){ .seed = 99 });
    an_obj *x1 = an_circle(s1, NULL);
    an_obj *n1 = an_circle(s1, &(an_circle_opts){ .name = "pivot" });
    draw(an_obj_rng(x1), base);
    draw(an_obj_rng(n1), named_a);
    an_scene_destroy(s1);

    // same, plus objects appended afterwards
    an_scene *s2 = an_scene_create(&(an_scene_desc){ .seed = 99 });
    an_obj *x2 = an_circle(s2, NULL);
    an_circle(s2, &(an_circle_opts){ .name = "pivot" });
    for (int i = 0; i < 10; i++) an_rect(s2, NULL);
    draw(an_obj_rng(x2), appended);
    CHECK(memcmp(base, appended, sizeof base) == 0);
    an_scene_destroy(s2);

    // an unnamed object inserted first: the unnamed x moves, named "pivot" doesn't
    an_scene *s3 = an_scene_create(&(an_scene_desc){ .seed = 99 });
    an_rect(s3, NULL);
    an_obj *x3 = an_circle(s3, NULL);
    an_obj *n3 = an_circle(s3, &(an_circle_opts){ .name = "pivot" });
    draw(an_obj_rng(x3), shifted);
    draw(an_obj_rng(n3), named_b);
    CHECK(memcmp(base, shifted, sizeof base) != 0); // documented limitation
    CHECK(memcmp(named_a, named_b, sizeof named_a) == 0);
    CHECK(strcmp(n3->name, "pivot") == 0);
    an_scene_destroy(s3);
}

static void test_ranges(void) {
    an_rng r = an_rng_make(5, 5);
    int hits[6] = { 0 };
    for (int i = 0; i < 60000; i++) {
        const float f = an_rng_float(&r);
        CHECK(f >= 0.0f && f < 1.0f);
        const float g = an_rng_range(&r, -2.0f, 3.0f);
        CHECK(g >= -2.0f && g < 3.0f);
        const int k = an_rng_int(&r, 1, 6);
        CHECK(k >= 1 && k <= 6);
        if (k >= 1 && k <= 6) hits[k - 1]++;
        CHECK(an_rng_below(&r, 3) < 3);
    }
    for (int i = 0; i < 6; i++) CHECK(hits[i] > 9000 && hits[i] < 11000); // ~10000 each
    CHECK(an_rng_int(&r, 4, 4) == 4);
    CHECK(an_rng_below(&r, 0) == 0);
    const int full = an_rng_int(&r, -2147483647 - 1, 2147483647); // full range must not hang
    (void)full;
}

// an_hash: pinned values (changing the function changes every render), no call-order
// dependence, and every input matters.
static void test_hash(void) {
    const uint32_t a = an_hash(1, 2, 3);
    for (int i = 0; i < 100; i++) an_hash((uint64_t)i, 7, 9); // unrelated calls in between
    CHECK(an_hash(1, 2, 3) == a);
    // pinned: render-time noise in existing scenes must not change silently
    CHECK(an_hash(0, 0, 0) == 0x4fee4699u);
    CHECK(an_hash(1, 2, 3) == 0x56460618u);
    CHECK(an_hash(3, 2, 1) == 0x57b17c99u);
    CHECK(an_hash(42, 7, 1000) == 0x00fb9e45u);
    CHECK(an_hash(1, 2, 3) != an_hash(3, 2, 1));
    CHECK(an_hash(1, 2, 3) != an_hash(1, 3, 2));
    CHECK(an_hash(1, 2, 3) != an_hash(2, 2, 3));
    CHECK(an_hash(1, 2, 3) != an_hash(1, 2, 4));
    // rough uniformity of the float form over consecutive counters
    int bins[10] = { 0 };
    for (uint64_t i = 0; i < 100000; i++) {
        const float f = an_hash_float(9, 1, i);
        CHECK(f >= 0.0f && f < 1.0f);
        bins[(int)(f * 10.0f)]++;
    }
    for (int i = 0; i < 10; i++) CHECK(bins[i] > 9500 && bins[i] < 10500);
}

static void test_duplicate_names(void) {
    an_scene *s = an_scene_create(NULL);
    CHECK(an_rect(s, &(an_rect_opts){ .name = "node" }) != NULL);
    fprintf(stderr, "  (expected message follows)\n");
    CHECK(an_circle(s, &(an_circle_opts){ .name = "node" }) == NULL);
    CHECK(s->count == 1);
    CHECK(an_circle(s, &(an_circle_opts){ .name = "node2" }) != NULL);
    CHECK(an_rect(s, NULL) != NULL && an_rect(s, NULL) != NULL); // unnamed never clash
    // name hash as documented: FNV-1a 64 with the top bit set
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = "node"; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
    CHECK(ani_rng_name_key("node") == (h | (1ull << 63)));
    an_scene_destroy(s);
}

int main(void) {
    test_hash();
    test_duplicate_names();
    test_reference_vector();
    test_same_seed_same_sequences();
    test_different_seeds_differ();
    test_streams_independent();
    test_adding_objects();
    test_ranges();
    return check_done("test_rng");
}
