# Timeline

Status: implemented (2026-11-24). The decisions in this doc were signed off before
implementation.

## The model in one paragraph

The user's `main()` is a **script**. It runs once, top to bottom, before anything
is rendered. While it runs, a **cursor** moves forward as the script plays and
waits. Each animation call **records a track** onto the timeline; it changes nothing
on screen. When the script is done, preview and export **sample** the timeline: the
state of every object at frame N is computed fresh from the object's **base** state
plus its tracks evaluated at N. Rendering never calls back into user code, and
nothing is ever carried over from frame N-1 to frame N. Any frame can be computed
alone, in any order, and gives the same result.

```
script (runs once)                 timeline (data)              render (per frame)
------------------                 ---------------              ------------------
an_rect(...)          ── base ──▶  obj base fields         ┐
an_play(move_to ...)  ── track ─▶  channel(obj, POS)       ├─▶ sample at N ─▶ tessellate ─▶ draw
an_wait(0.5)          ── cursor    (cursor: ticks)         ┘   (pure function of N)
```

## Time

- **The cursor is integer ticks: microseconds.** Every track's start and duration
  is in ticks. The API takes seconds (`float`), converted once with
  `ticks = llround(seconds * 1e6)`.
- **Frames appear only when sampling.** Frame N is the instant `N / fps` seconds.
  Comparisons are done exactly in integers: a track starting at tick `S` has started
  at frame N when `S * fps <= N * 1e6`. Progress inside a track is a double:
  `elapsed = (N * 1e6 - S * fps) / (fps * 1e6)` seconds.
- Converting through ticks doesn't drift: ten `an_wait(0.1)` at 24 fps are
  1,000,000 ticks, which is exactly 24 frames (tested). Durations that aren't whole
  frames are fine: a track ending between two frames shows its final value from
  the next frame on.
- **Scene length** `L` (in frames) is `ceil(end * fps / 1e6)`, where `end` is the
  later of the final cursor and the latest track end. **Export renders frames 0..L
  inclusive, which is L + 1 frames**, so the final state is always on screen.
  `an_scene_export` returns how many frames it wrote.
- **Preview** picks which frame to show from the wall clock (looping, Space
  pauses, ←/→ step while paused). That only chooses N; what's on screen at N comes
  purely from the timeline.

## Base state, channels, tracks

Each object stores a **base** state: the values its options struct gave it. The
base doesn't change after creation (the one exception is the setter shortcut
below, which is equivalent).

Every animatable value is addressed by a **property handle** `(target, prop)`. The
target is an object today. The scene, and later the camera, can be targets too
without changing tracks or sampling. A handle with at least one track has a
**channel**: that handle's tracks, sorted by `(start, seq)`.

| prop             | comps | interpolated in | settle floor | notes                         |
|------------------|-------|-----------------|--------------|-------------------------------|
| `AN_PROP_POS`    | 2     | linear          | 0.01 px      | relative to the parent        |
| `AN_PROP_SCALE`  | 2     | linear          | 1e-4         |                               |
| `AN_PROP_ROTATION`| 1    | linear          | 1e-4 rad     | raw angle, no wrap            |
| `AN_PROP_OPACITY`| 1     | linear          | 1e-4         | clamped to [0,1] after sampling; multiplies down the tree |
| `AN_PROP_FILL`   | 4     | OKLab + alpha   | 1e-4         | gamut-clamped back to sRGB    |
| `AN_PROP_STROKE` | 4     | OKLab + alpha   | 1e-4         |                               |
| `AN_PROP_Z`      | 1     | step            | n/a          | steps only                    |
| `AN_PROP_STROKE_WIDTH` | 1 | linear     | 0.01 px      | clamped to >= 0               |
| `AN_PROP_DRAW`   | 1     | linear          | 1e-4         | draw progress, clamped to [0,1]; see docs/strokes.md |

A **track** is `{start, duration (ticks), seq, ease, from, to, v0}`. `from`, `to`
and the initial velocity `v0` are stored in the interpolation space: colors are
OKLab L, a, b plus alpha.

### Sampling a channel at time T

1. The **winner** is the last track in `(start, seq)` order with `start <= T`. If
   there's none, the value is the base.
2. If the winner has duration 0, or `T >= start + duration`, the value is exactly
   `to`. At exactly `T = start` it is exactly `from`. Tracks keep exact natural
   `from` and `to` values alongside the interpolation ones, so a color track's
   endpoints don't pass through a round-trip conversion.
3. Otherwise the value is the winner evaluated at `elapsed = T - start` seconds
   (see Easing).

This is the **take-over** rule. A track that starts later replaces whatever was
running from its start onward. Tracks are never added together.

### Recording a track (take-over handoff)

When a track is recorded at start `S` on a channel, three things happen:
- `from` = the channel sampled at `S`, using the tracks recorded so far (or the
  base).
- `v0` = the **analytic velocity** of the winner at `S` (0 if the base wins, or if
  the winner is a step or has already finished). Every easing has an exact
  derivative, springs included.
- `to` is either given, or computed as `from + delta` for `*_by` animations.

Velocity is only used when the **new** track is a spring: it starts with `v0`, so
interrupting a spring with another spring is smooth in both position and velocity.
A linear or ease-in-out track always starts at rest and ignores `v0`, by
definition.

### Setters

`an_obj_set_pos`, `_scale`, `_rotation`, `_opacity`, `_fill`, `_stroke` and `_z`
record a **zero-duration take-over track at the cursor**. From the cursor on, the
value is the one set, even if another track is running at that moment. That track
is cut off, just as by any later track.

The shortcut: if the property has no tracks and the cursor is at or before the
object's creation, the setter writes the base directly. Nothing exists earlier for
it to differ from, so this is equivalent.

`an_obj_set_parent` is structural: it changes the tree for every frame, including
frames before the call.

### Object lifetime and visibility

An object is created at the cursor and isn't drawn before then. A subtree whose
**effective opacity** is 0 is skipped entirely, with no triangles emitted.
Effective opacity is the product of opacities from the root down, so a group at
opacity 0 hides all its children.

## Easing

Linear and ease-in-out work on normalized progress `u = elapsed / duration`.
Springs work on elapsed seconds.

- **`AN_EASE_LINEAR`**: `e = u`, with `e' = 1`.
- **`AN_EASE_IN_OUT`** (cubic): `e = 4u³` for `u < 0.5`, otherwise
  `1 - (-2u+2)³ / 2`. `e' = 12u²` or `3(-2u+2)²`. Golden values: `e(0.25) =
  0.0625`, `e(0.5) = 0.5`, `e(0.75) = 0.9375`.
- Linear and in-out values are `from + (to - from) * e`, and their velocity is
  `(to - from) * e' / duration_seconds`.
- **Springs**: `an_spring(damping_ratio, response)`. ζ = `damping_ratio`, clamped
  to [0.01, 1]: 1 is critically damped, and lower values give more bounce. ω =
  2π / `response` (seconds; ≤ 0 means 0.5 s). Each component is solved
  **analytically** as a displacement `d = x - to`, with `d(0) = from - to` and
  `d'(0) = v0`:
  - ζ = 1: `d(t) = (d0 + (v0 + ω d0) t) e^(-ωt)`
  - ζ < 1: `d(t) = e^(-ζωt) (d0 cos ω_d t + B sin ω_d t)`, where
    `ω_d = ω√(1-ζ²)` and `B = (v0 + ζω d0) / ω_d`
  - The velocity `d'(t)` is the exact derivative of each formula.

  Since this is a closed form in seconds, a spring passes through the same values
  at 30 and 60 fps (tested), and frame N needs no frames before it.
- **Spring duration.** A spring with duration 0 (the default) lasts until it
  **settles**: the first time after which the analytic error envelope stays under
  `tol`. Here `tol = max(1e-3 × travel, floor)`, where `travel` is the largest
  component of `|to - from|` and `floor` comes from the property table. The
  envelope is `e^(-ζωt) √(d0² + B²)` for ζ < 1, or `(|d0| + |v0 + ω d0| t) e^(-ωt)`
  for ζ = 1 (solved by bisection after its peak). The time is rounded **up** to
  whole ticks, and after it the value is exactly `to`. An explicit duration
  overrides this and snaps to `to` at the end.

## Color interpolation: OKLab

Colors are converted sRGB → linear → OKLab (Björn Ottosson's matrices), the L, a
and b components are interpolated (or sprung), and the result converts back. When
the result falls outside sRGB, the **gamut clamp** keeps L and hue and reduces
chroma by bisection until the color fits. Alpha interpolates linearly and
is clamped. Lightness outside [0,1] (spring overshoot) becomes black or white
before the chroma step, since no other sRGB color has that lightness. Golden values: sRGB red is OKLab (0.62796, 0.22486, 0.12585),
green (0.86644, -0.23389, 0.17950), blue (0.45201, -0.03246, -0.31153).

## Script API

```c
an_ease an_spring(float damping_ratio, float response_seconds);
// AN_EASE_LINEAR, AN_EASE_IN_OUT

typedef struct an_anim_opts {
    float duration;  // seconds; 0 = 1 s (springs: settle time)
    an_ease ease;    // zero value = AN_EASE_IN_OUT
} an_anim_opts;

an_anim an_move_to(an_obj *o, an_vec2 to, const an_anim_opts *opts);
an_anim an_move_by(an_obj *o, an_vec2 delta, const an_anim_opts *opts);
an_anim an_scale_to(an_obj *o, an_vec2 to, const an_anim_opts *opts);
an_anim an_rotate_to(an_obj *o, float radians, const an_anim_opts *opts);
an_anim an_rotate_by(an_obj *o, float radians, const an_anim_opts *opts);
an_anim an_fade_to(an_obj *o, float opacity, const an_anim_opts *opts);
an_anim an_fill_to(an_obj *o, an_color to, const an_anim_opts *opts);
an_anim an_stroke_to(an_obj *o, an_color to, const an_anim_opts *opts);

void an_play(an_scene *s, an_anim a);        // start at cursor; cursor = its end
void an_play_async(an_scene *s, an_anim a);  // start at cursor; cursor unchanged
void an_wait(an_scene *s, float seconds);    // cursor += ticks(seconds)
void an_par(an_scene *s, int n, const an_anim *anims);  // all at cursor; cursor = latest end
void an_stagger(an_scene *s, int n, const an_anim *anims, float gap);
                                             // anim i at cursor + i*ticks(gap); cursor = latest end
void an_seek(an_scene *s, int64_t frame);    // set the frame an_obj_get reports
an_obj_state an_obj_get(const an_obj *o);    // local resolved state at the seek frame
int64_t an_scene_length(const an_scene *s);  // L; export writes L + 1 frames
double an_scene_time(const an_scene *s);     // cursor, seconds
```

- `an_draw(o, opts)` is the one anim with an explicit start value: draw progress
  always goes from 0 to 1, and the track starts at rest. Everything else
  continues from the current value (take-over).
- An `an_anim` is a plain value that describes an animation. Nothing is recorded
  until it's played. `an_par` and `an_stagger` record in array order (stagger's
  starts are increasing in that order), so on a shared channel the later entry
  wins ties.
- Each anim's end is known when it's recorded (a spring's settle time depends on
  its `from` and `v0`), so `an_par` and `an_stagger` set the cursor to the latest
  actual end.
- Not in v1: nesting anims, animated geometry, `an_remove`, and updaters.
- Anims on a NULL object do nothing.

## Randomness

Two kinds, for two moments:

- **Script time: streams.** `an_scene_rng(s)` and `an_obj_rng(o)` are PCG32
  streams derived from the seed. Drawing from them is stateful and follows the
  order of calls, which is fine because the script runs once in a fixed order.
- **Render time: `an_hash(seed, stream_id, counter)`.** A stateless hash returning
  a `uint32_t` (and `an_hash_float` in [0,1)). Anything random computed while
  rendering (grain, jitter, misregistration) must use it, with inputs such as
  `(seed, object id, frame)`, so the result doesn't depend on which frames were
  rendered before or in what order. Rendering never touches an RNG stream.
  Mixing: SplitMix64's finalizer applied to `seed`, then folded with `stream_id`
  and `counter`, each pre-mixed with its own constant. The top 32 bits are
  returned. The values are pinned by a test.

### Object stream keys and names

An object's stream key is its creation id, or, if it has a `.name`, the
**name hash**:

```
FNV-1a 64-bit over the name's bytes (no terminator):
    h = 0xcbf29ce484222325
    for each byte c: h ^= c; h *= 0x100000001b3   (mod 2^64)
key = h | (1 << 63)      // top bit set: can't collide with a creation id
stream = PCG32 seeded from SplitMix64-mixed (scene seed, key)
```

Names are unique within a scene. Creating an object with a name that's already
in use fails: the creation function returns NULL and prints a message to stderr.
Unnamed objects' streams follow creation order, so inserting an object earlier in
the script shifts the ids of the unnamed objects after it. Naming an object pins
its stream.

## Tests

- `tests/test_ease.c`:
  - Golden easing values.
  - Golden spring values at known frames, for ζ ∈ {1, 0.5, 0.2}, at 30 and 60 fps.
    The constants were computed independently with `bc`.
  - Velocity matches a finite difference.
  - Spring settle respects the tolerance, and the last frame is exactly `to`.
  - OKLab golden values, round trips, and the gamut clamp.
- `tests/test_timeline.c`:
  - Cursor arithmetic for play, wait, par, stagger and async, and `L`.
  - 10 × `an_wait(0.1)` at 24 fps is exactly 24 frames.
  - Seek order independence: in order, reverse and a seeded shuffle give
    byte-identical states and meshes.
  - Take-over continuity in value, and in velocity for spring to spring.
  - Setters step at the exact frame and cut off a running track.
  - Creation time and zero-opacity skipping.
- `tests/test_rng.c`: `an_hash` pinned values and order independence; duplicate
  names rejected.
