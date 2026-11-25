# Camera and the first verbs (7A): design

Status: decided 2026-11-25 (the review's answers are folded in below). Scope: a
2D camera (position, zoom, rotation) as properties of the scene target, a verb
transform with three stateless verbs (`an_slap`, `an_pop`, `an_squash_stretch`),
and `an_shake`. Nothing else from 7.

## Camera

The camera is the scene's, and its properties go through the same
(target, property) handle as the chaos dial: `an_anim` with `.scene` set,
recorded by `an_play` / `an_par` / `an_stagger`, sampled fresh at every frame.

| property | type | default | meaning |
|---|---|---|---|
| `AN_PROP_CAMERA_POS` | vec2, scene px | the scene's center | the world point shown at the center of the frame |
| `AN_PROP_CAMERA_ZOOM` | float | 1 | screen px per world px; interpolated in log space, so 1 → 4 feels even |
| `AN_PROP_CAMERA_ROTATION` | float, radians | 0 | turning the camera clockwise turns the world counter-clockwise on screen |

```c
an_play(s, an_camera_move_to(s, (an_vec2){ 700, 400 }, &(an_anim_opts){ .duration = 1 }));
an_play(s, an_camera_zoom_to(s, 2.0f, NULL));
an_play(s, an_camera_rotate_to(s, 0.05f, NULL));
an_camera_set_pos(s, ...); an_camera_set_zoom(s, ...); an_camera_set_rotation(s, ...);
an_camera_state cs = an_camera_get(s); // at the frame last passed to an_seek, shake included
```

**The view transform**, the root of every object's world transform:

`view = translate(frame center) · rotate(−rotation) · scale(zoom) · translate(−(pos + shake))`

The paper background, its grain and its vignette are not under the camera:
they're the print surface, fixed to the frame.

### Pure translation after the cache (to evaluate before threads)

A camera that only translates (zoom exactly 1 and rotation exactly 0, which
includes every frame of a shake, since shake is translation-only) doesn't have
to enter the world transform at all.
- Objects are tessellated in world coordinates, as if the camera were at rest.
- The camera's translation is applied per draw: the shape shader's `view`
  origin uniform shifts by it, and so do the layer rectangles.
- The tessellation cache keys then don't change while the camera pans or
  shakes, so static objects keep hitting.
- With any zoom or rotation, the camera goes into the world transform on the
  CPU, as above.

Equivalence:
- Riso offset, shadow offset and boil amplitude are already added in screen
  space, and a translation commutes with all of them.
- Snapping is relative to the text's origin.
- So the two ways of applying a pure translation differ only by float rounding
  (the order of a few additions). That's invisible, but not bit-identical.
- That's why the mode is a pure function of the camera's state at the frame,
  never of history. A zoom that returns to exactly 1 returns to the fast path,
  and the two paths never mix within a frame.

Gate (the same as the cache's):
- cache on == cache off, frame by frame, in forward, reverse and shuffled
  order, with camera pans and shakes in the scenes;
- the reference frames stay byte-identical, since the default camera is
  identity.

Numbers (measured 2026-11-25, `spike_frame_bench 7`: scenario 6, all drawn with
10% of objects moving, plus a continuous 6 s camera pan; 5 guarded runs each):

| scenario 7 | median ms | p99 ms | frames > 16.7 ms | frames > 12 ms | replayed |
|---|---|---|---|---|---|
| fast path, cache on | 6.17–6.34 | 13.3–13.6 | 0 | 58–67 | 71.8% |
| camera in the transforms, cache on | 12.38–12.49 | 15.0–19.1 | 0–5 | 289–305 | 0% |
| cache off | 12.32–12.44 | 14.7–17.4 | 0–5 | 278–298 | |

- Under a pan the fast path keeps the static-camera frame time (scenario 6 was
  5.7–6.3 ms median). In the transforms, every object misses: cache-off speed.
- The p99 is still the boil-step frames (13.3–13.6 ms), as in scenario 6:
  parallel tessellation's job.
- **The two paths in pixels** (13 pan frames at 1080p, text, code, shapes with
  ink): 43 of 27 million pixels differ, each by one MSAA sample's worth (at
  most 59 levels) at an edge. That's float rounding, as expected.
- **Gate:** test_memo's scenes now pan and zoom out and back to exactly 1, so
  frames switch between the paths; cache on == off in all orders.

## Verbs: the verb transform

A per-object **verb transform** sits between the object's own transform and its
geometry: scale (vec2), rotation (radians) and a pivot (local units).

`local = translate(pos) · rotate(rotation) · scale(scale) · translate(pivot) · rotate(verb_rotation) · scale(verb_scale) · translate(−pivot)`

| property | type | default |
|---|---|---|
| `AN_PROP_VERB_SCALE` | vec2 | {1, 1} |
| `AN_PROP_VERB_ROTATION` | float | 0 |
| `AN_PROP_VERB_PIVOT` | vec2, step | the object's center |

**The center (the default pivot), fixed at creation:**
- rect and circle: their local origin;
- line, arrow and stroke: the center of the path's bounding box;
- a letter: its pivot (the middle of its advance at half the x-height);
- a text: the center of its block (advance width by ascent-to-descent);
- a group or code block: its origin.

A verb can override the pivot (`.pivot` with `.custom_pivot = true`). Every
verb writes the pivot as a step at its start, so a later default-pivot verb
puts it back.

Keeping verbs out of `pos`, `scale` and `rotation`:
- a verb runs on top of a move or scale animation without taking it over;
- it gives (d) a clean "rest scale";
- it makes "the final frame is exactly the base state" structural. Every verb
  track ends at its identity value, and an ended track returns its stored end
  value, so the last frame's transform is the base transform bit for bit.

```c
typedef struct an_verb_opts {
    float duration;        /* 0: the spring's settle time */
    float amount;          /* verb-specific strength; 0 = its default */
    an_vec2 pivot; bool custom_pivot;
    float impact_shake;    /* an_slap: camera shake intensity (screen px) at impact; 0 = none */
    float impact_shake_duration; /* 0: 0.5 s */
} an_verb_opts;
an_anim an_slap(an_obj *o, const an_verb_opts *opts);
an_anim an_pop(an_obj *o, const an_verb_opts *opts);
an_anim an_squash_stretch(an_obj *o, const an_verb_opts *opts);
```

They return `an_anim`s, so `an_play` and `an_stagger` work on them. Inside, one
verb anim records its tracks together: scale, and rotation where it has one.

| verb | tracks (spring: damping, response) | amount |
|---|---|---|
| `an_slap` | scale from {a, a} to {1, 1} (0.45, 0.22 s); rotation from −0.06 rad to 0 (same spring) | a = 2.5 |
| `an_pop` | scale from {0, 0} to {1, 1} (0.5, 0.3 s) | overshoot comes from the spring (16.3%) |
| `an_squash_stretch` | scale from {1 + a, 1 / (1 + a)} to {1, 1} (0.35, 0.3 s) | a = 0.3 |

- **Stateless:** they're ordinary tracks with explicit starts, so any frame in
  any seek order gives the same result.
- **Slap's impact frame:** the first frame at or after the moment the scale
  spring first reaches its target, computed analytically at script time.
  - With v0 = 0 and d0 = a − 1, the displacement is
    `d(t) = d0 e^{−ζωt} (cos ω_d t + (ζω/ω_d) sin ω_d t)`.
  - Its first zero is `t* = (π − atan(ω_d / (ζω))) / ω_d`, where
    `ω = 2π / response` and `ω_d = ω √(1 − ζ²)`.
  - At ζ = 0.45 and response 0.22 s, t* = 0.0799 s (4.79 frames at 60 fps).
  - The impact frame is `ceil((start + t*) × fps)`: frame 5 after the start at
    60 fps.
  - A critically damped spring never crosses its target, so its impact is the
    end of the track.
- **`.impact_shake`** records a shake event on the camera, starting exactly at
  the impact frame (its tick is the frame's time, rounded down to a whole
  tick, so it's reached at that frame and not the one before).

### What a non-uniform verb scale does

Squash and stretch, or anything with sx ≠ sy:

- **Text snapping:** off. Snapping needs an effective scale of exactly 1 on
  both axes; see (b).
- **Ink width:** strokes take their width from the area scale √|det| of the
  world transform, as for any non-uniformly scaled object, not per axis. So
  ink is drawn with a constant-width pen along the squashed path, not squashed
  like an image. An area-preserving squash (det = 1) keeps the ink width
  exactly.
- **Riso offset:** constant in screen px, unchanged. The shadow offset's ink
  term follows the same √|det|.
- **Text size factor and text riso scale:** they exclude verbs, see (d), so a
  squash doesn't change them.

## Shake

`an_shake(s, intensity, opts)` returns an `an_anim` on the scene. When played
it records an event (start tick, duration, intensity, and its index among the
scene's shake events) on a camera shake channel. Translation only, which keeps
shakes on the fast path above.

The camera's shake offset at frame N, summed over events:

```
u      = (t(N) − start) / duration           exact ticks, as for tracks
         (u < 0 or u ≥ 1: the event contributes exactly nothing; it isn't computed)
trauma = 1 − u                               linear decay from 1 at the trigger to 0
amp    = intensity × trauma²                 trauma exponent 2
k      = (t(N) − start) × 15                 noise rate 15 Hz, from the trigger
i = floor(k), f = k − i, s = f² (3 − 2f)     smoothstep: continuous, C1
n_c    = lerp(h(i), h(i + 1), s) × 2 − 1     h(j) = an_hash_float(seed, 0x5EA4E000 + 2 × event + c, j), c = 0 x, 1 y
offset = amp × (n_x, n_y)                    screen px, within ± intensity
```

- **Trauma exponent 2:** the shake fades fast at first and lingers a little,
  which reads as an impact. Exponent 1 sounds like a buzz, 3 like a single
  thump.
- **Noise rate 15 Hz**, smooth between samples: a handheld jolt, not a vibration.
  It also stays off the 12 Hz boil rhythm, so the two never lock together.
- **A pure function of (scene seed, event index, frame).** Nothing is carried
  between frames, and nothing depends on wall time or on what was rendered
  before.
- **It returns exactly to rest:** for u ≥ 1 the term isn't evaluated, so from
  the frame the decay ends, the camera is exactly its base position. `==`
  holds, not "near".
- **(e) Rendering order:**
  - The offset is evaluated once per frame, from the scene, before the tree
    is traversed.
  - Only `an_hash` feeds it: a stateless hash of (seed, stream, counter). No
    RNG streams, object order, layer order or earlier renders are involved.
  - The event index is its recording order in the script.
  - It can't leak through the tessellation cache either: on the fast path it
    isn't in the geometry at all; otherwise it's in every world transform,
    which is in the key.

## (a) Facets and `max_zoom`

Paths are flattened once, at object creation (rule 10).

- **Declared limit:** `an_scene_desc.max_zoom`, default 4.
- **Tolerances at creation:**
  - curves: `0.2 / max_zoom` local px;
  - circles: the same tolerance picks their segment count;
  - glyphs: `1 / (1000 × max_zoom)` em.
  - At the default these are exactly today's values (0.05 px, 1/4000 em), so
    existing scenes don't change.
- **Cost of raising it:** a scene that zooms to 10x declares `max_zoom = 10`
  (tolerance 0.02: about 1.6x the points on curves, since points go with
  1/√tolerance). It then never shows a facet or a threshold pop.
- **What "zoom" means:** the effective scale on screen: camera zoom ×
  the object's and its ancestors' scales × verb scales. That is the largest
  singular value of the object's world transform, so non-uniform scale counts
  by its larger axis.
- **Checking it** includes spring overshoot (a pop overshoots by ~15%; a
  zoom spring can overshoot its target). Rather than predict that at script
  time, the renderer measures it on every frame it renders:
  - for every drawn object it computes that singular value;
  - export samples every frame of the timeline; the preview, every frame it
    shows;
  - the first time it exceeds `max_zoom` (by more than 1e-4), it warns once
    per renderer: `anown: an object reaches 5.20x on screen at frame 312
    (max_zoom 4); curves may show facets`.
  - It's a warning, not a clamp: rendering goes on.
- **The cache** is unaffected: paths stay immutable, one geometry per object.

## (b) Text snapping

Snapping (docs/text.md) needs the text's **effective scale to be exactly 1**:
- its world transform, camera, ancestors, verbs and the letter's own transform
  included, is a pure translation (a = d = 1, b = c = 0);
- any zoom, rotation, verb in flight, or non-unit scale turns it off;
- pans and shakes are translations, so they keep it on, relative to the text's
  origin.

This replaces "translation × uniform scale". The snap test's uniform-1.5 case
becomes "not snapped".

## (c) What zooms and what doesn't

| | under zoom z | why |
|---|---|---|
| geometry, stroke width, patterns | x z | they're the object |
| boil wavelength (bucket spacing, local arc length) | x z | the wobble belongs to the line |
| **boil amplitude** | **fixed in screen px** | applied to the transformed points |
| riso offset | fixed, screen px | the press's misregistration, not the object's |
| shadow offset | fixed screen px + the ink width x z | it still clears the zoomed ink |
| paper grain, vignette | not under the camera | the paper |

**Boil below a 6 px wavelength.** Zoomed out, the bucket spacing on screen
(15 local px × scale) shrinks while the amplitude stays 1.5 px. Below about
6 px the line stops wobbling and turns to fuzz.
- **Coarser buckets:** the spacing is multiplied by the smallest power of two
  m ≥ 1 with `15 × m × s ≥ 6` px, where s is the object's effective area scale
  (camera zoom × scales × verbs, √|det|).
- **When m changes:** s is sampled at **the start of the current boil step**
  (t = step / 12 s), not at the current frame. So m changes only on a boil
  step, where the boil re-rolls anyway, and a zoom-out never pops between steps.
- **Purity:** m is still a pure function of the frame, and it's in the cache
  key (it's part of the spacing).
- **Effect:** at zoom 0.4 the wavelength is exactly 6 px (no change); at 0.3 the
  spacing doubles, at 0.1 it quadruples. The wavelength on screen stays at or
  above 6 px (below 12), with the same amplitude.
- **No `min_zoom`:** zooming out is never an error.

## (d) The text size factor

The factor follows the stem width on screen (docs/text.md):
- **it includes the camera zoom** (and the object's and its ancestors'
  scales), so zooming into text raises its chaos smoothly, as approved;
- **it excludes verbs**, so a slap from 2.5x doesn't sweep a title's chaos.
  Archivo Black at 28 px would otherwise go from 1 to 0.62 in 0.2 s, and code
  text would cross 0, toggling its shadow and snapping.

At a verb's end both scales agree, so nothing jumps. The text riso scale uses
the same scale. `an_obj_chaos` follows the same rule, camera zoom included.

**Zooming into code text** doesn't toggle its shadow or its snapping:
- code text has its shadow off by default, so rising chaos never draws one;
- snapping turns off the first frame the zoom leaves 1 and stays off until it's
  exactly 1 again. It never flickers on and off.

## Interaction with the tessellation cache

- **Fast path (translation only):** keys don't change, so pans and shakes keep
  the hits.
- **Zoom or rotation:** every object's world transform changes, so every
  object misses on such a frame, and frame time is the cache-off time. That is
  where the parallel tessellation proposal (docs/progress.md) matters.
- **Boil coarsening** is part of the spacing in the key.

## Tests (hand-computed only)

- **Zoom 2x doubles bounds, not the riso offset:** a 60×40 rect with ink width
  4 at the center (480, 270) of a 960×540 scene, camera at (480, 270), chaos 1,
  no boil.
  - At zoom 2 the fill spans 120×80 px around the center, shifted by the riso
    offset (+4, −3) exactly as at zoom 1, not doubled.
  - The ink band is 8 px wide.
  - Checked in pixel columns and rows computed by hand.
- **Shake:**
  - The camera offset at every frame, read through `an_camera_get`, is
    identical in forward, reverse and shuffled seek order.
  - From the end of the decay on it's exactly the base position (`==`).
  - One frame's offset is recomputed in the test from `an_hash_float` and the
    formula above.
  - Pixel frames are identical in all three orders with the cache on and off.
- **Slap:**
  - Its last frame has `verb_scale == {1, 1}` and `verb_rotation == 0`
    exactly, and is byte-identical to a render without the slap.
  - Its first frame is 2.5x: the rect's bounds, by hand.
  - Its impact frame is the computed one (frame 5 at 60 fps: t* = 0.0799 s), and the
    impact shake's event starts there: the camera is at rest at frame 4 and
    moves at frame 5.
- **Squash:** with ink, the ink band's width is unchanged at a det-1 squash.
- **Pivot:** a rect slapped about a custom pivot keeps that pivot point fixed on
  screen at every frame.
- **max_zoom:** the warning fires (once) for an object at 4.5x and not at 3.9x.
- **Text:**
  - Snapping is off at camera zoom 1.5 and on at zoom 1.
  - The size factor during a slap equals the factor at rest (`an_obj_chaos`).
  - Zooming into code text never draws a shadow pixel, and snapping turns off
    once, without coming back until the zoom is 1.
- **Boil coarsening:** the spacing is 15, 15, 30 and 60 local px at zoom 1,
  0.4, 0.3 and 0.1. It changes only at boil-step frames during a zoom-out.
- **Fast path:** test_memo's scenes gain a camera pan and shakes, and its gate
  holds.

## Commits

Three, in this order (the slap's impact shake needs the shake):
1. **Camera properties:** pan, zoom, rotation, the translation fast path,
   `max_zoom`, snapping (b), (c) boil coarsening, (d) size factor with the
   camera zoom.
2. **Shake.**
3. **Verbs:** the verb transform, slap / pop / squash-stretch,
   `.impact_shake`.

## Example

`examples/09_camera`:
- a title slaps in letter by letter, with an impact shake on the last letter;
- the camera pans, zooms to 2.5x onto a detail, and back;
- a node pops in, and another squash-stretches.

Exported in share mode, with export time and the frame bench numbers reported.
