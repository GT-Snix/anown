# Text (Prompt 6): design

Status: implemented 2026-11-25 (see "As built" at the end for where the code
differs from this proposal). Approved 2026-11-25 with changes, which are folded in below: snapping
relative to the text's own origin, chaos from the on-screen size every frame
(with a cap found while checking 40–48 px), riso and shadow off for code text,
riso offset scaled for display text, the winding overflow behavior, and two more
tests. Numbers marked *measured* were measured on 2026-11-24 with upstream
stb_truetype v1.28 (2026-11-12) on the two fonts, by scratch programs that
aren't part of the tree.

## The question

How do glyphs get drawn?

- **(a) Vector outlines.** `stbtt_GetGlyphShape` gives each glyph's contours.
  They're flattened once per (font, glyph) and drawn through the existing
  path pipeline: fill, ink, boil, riso, hard shadow, shadow cut, draw-on.
- **(b) An SDF/MSDF atlas.** Distance fields are rendered into a texture atlas,
  and each glyph is one textured quad.

**Recommendation: (a)**, as you suggested. I tried to argue against it and found
four real problems. None of them is a reason to switch, but each changes the
design, and they're listed below with fixes:

1. Glyphs need a different fill method, and the stencil has to be re-budgeted.
2. With 4x MSAA as the hardware maximum, small text needs pixel snapping.
3. Hundreds of glyph objects only stay cheap if consecutive glyphs batch.
4. Boil at code sizes doesn't read as hand-drawn; it just makes text shimmer.
   So text gets a stem-width-based default chaos.

## (a) vs (b)

| | (a) vector outlines | (b) SDF / MSDF atlas |
|---|---|---|
| Boil | the same keyed boil as every other path (object-space buckets, 12 Hz) | only a shader domain warp: a different look, not keyed like the shapes |
| Ink outline | `ani_stroke_emit` with brush, taper and overshoot (rule 10) | a constant-width distance band; no taper, overshoot or pressure; breaks rule 10 |
| Draw-on / tag-draw | along the contours, like any stroke | impossible without the outline paths, which means doing (a) anyway |
| Riso, hard shadow, shadow cut, paper_under | work as they are | riso and shadow are easy; the cut needs discard-based stencil writes |
| Large display text | exact at any size | single-channel SDF (`stbtt_GetGlyphSDF`) rounds corners; MSDF fixes that, but msdfgen is C++ (we use C and single-header deps only) |
| Small text edges | 4x MSAA: 5 coverage levels (see problem 2) | analytic AA: smooth edges at 28 px |
| Cost of a big code block | about 26 points per glyph, fills drawn as two batched commands (problem 3) | one quad per glyph |
| New machinery | stencil-then-cover fills, glyph cache | atlas packing (we've avoided atlases so far), SDF generation, a second text renderer |

(b) wins only on small-text edge quality and raw cost. Both are addressable
inside (a) (problems 2 and 3). (b) cannot give boil, brush outlines or draw-on
without becoming (a).

## Problem 1: glyph fills need stencil-then-cover, and the stencil needs a new budget

- Today's fills are convex fans from the shape's center (rects, circles). Glyphs
  are concave, have holes (counters), and often have **overlapping contours**,
  which are common in display fonts and in static instances cut from variable
  fonts.
- **Non-zero, not even-odd.** TrueType and CFF both define the non-zero rule.
  Even-odd would punch holes wherever two contours of one glyph overlap.
- **Method: stencil-then-cover.**
  1. *Stencil pass:* each contour is drawn as a triangle fan from its first
     point, color off. Front faces increment the winding counter and back faces
     decrement it (wrapping).
  2. *Cover pass:* one quad per glyph (its bounding box). It passes where the
     winding is nonzero, and zeroes the winding as it goes.

  Because the cover zeroes as it covers, every sample is painted once. So
  translucent glyph fills are write-once by construction and don't need the
  write-once marks.
- **Why not triangulate glyphs once on the CPU?** A cached triangulation breaks
  under boil: displaced points flip thin triangles at 28 px, where stems are
  2.5 px and the boil amplitude is 1.5 px. Stencil-then-cover recomputes the
  coverage from whatever outline this frame has, so it's exact under any
  displacement.
- **Stencil budget**, 8 bits. Today: 0x80 silhouette, 0x7F write-once marks
  with values 2, 3 and 0x7F. Proposed:

  | bits | use |
  |---|---|
  | 0x80 | silhouette (shadow cut), unchanged |
  | 0x70 | write-once marks, re-encoded: shadow 0x70, fill 0x20, ink 0x30 |
  | 0x0F | winding counter, mod 16 (write mask 0x0F) |

  - **Overflow past 15.** The counter uses `INCR_WRAP` / `DECR_WRAP` on the
    four low bits, so it holds the winding number mod 16, and the cover tests
    "nonzero". A winding of −1 is stored as 15 and still reads as inside. The
    test is exact for every |winding| from 1 to 15. It fails only when the
    winding is a nonzero multiple of 16: 16 same-direction contours stacked on
    one sample read as 0, and that sample is left unpainted (a pinhole). 17
    reads as 1 and is painted again. Glyph overlaps reach 2 or 3, and a batch
    adds one per overlapping glyph (negative kerning, tight tracking), so
    reaching 16 takes 16 glyphs of one batch stacked on the same sample. I
    don't guard against it; a test pins the behavior (16 nested
    same-direction squares leave the innermost one unpainted, 15 and 17 don't).
  - The marks change encoding only. The test suite pins every existing
    behavior, so the change must leave all of it bit-identical. I'll show that
    first, as its own step.
- **The silhouette and shadow of a glyph** use the same two passes:
  - the silhouette's cover sets 0x80;
  - the shadow's *stencil pass* only counts where 0x80 is clear, so its cover
    can't paint inside the silhouette. That's the same cut, still per sample.
- **Riso** draws the fill's two passes again, translated. **paper_under** is one
  more cover in paper color.

## Problem 2: small text and 4x MSAA

- *Measured:* this M4 supports 1x, 2x and 4x MSAA, not 8x. A glyph edge gets 5
  coverage levels; a rasterizer or an SDF gets 256.
- *Measured:* JetBrains Mono Regular at 28 px has a 2.52 px H stem, a 15.4 px
  x-height and a 16.8 px advance (600/1000 em).
  - A 16.8 px advance puts every glyph at a different sub-pixel phase. Identical
    letters then render differently, and stems read as 2 or 3 px from letter to
    letter: uneven weight. 5 levels makes that worse.
- **Fix 1: snap.** When a glyph's effective chaos is 0 (the code default, see
  problem 4), its pen position is snapped to whole pixels **relative to the text
  object's own origin**, not to the screen.
  - Each pen offset `x` from the origin (local units) becomes
    `round(x × s) / s`, where `s` is the text's on-screen scale this frame. All
    glyphs of a text then share the origin's sub-pixel phase, so identical
    letters are identical, and the text as a whole still moves smoothly: a
    slow `an_move_to` doesn't step from pixel to pixel.
  - Only while the text's world transform is a translation times a uniform
    scale. Under any rotation or non-uniform scale there's no snapping at all,
    since there's no pixel grid to line up with.
  - During a uniform scale animation the rounded offsets change at different
    moments, so the gap between two glyphs can step by 1 px. That's the price
    of an integer advance; I'll check it in the code example's zoom.
  - Recommend code sizes where the advance is an integer: 0.6 × size, so 25,
    30 or 35 px. **Default code size: 30 px** (advance 18 px exactly, line height
    39.6 px, rounded to 40).
  - Then every glyph is on the same phase, identical letters are pixel-identical,
    and H.264 compresses it better too.
- **Fix 2 (only if fix 1 isn't enough): supersampled text layers.** Isolate a
  code block into a layer at 2x resolution (16 samples per output pixel) and
  downsample when compositing. That costs 4x memory for that layer.
- **Measure before deciding.** A test renders a 30 px code line and compares it
  to stb's own 256-level rasterizer (`stbtt_MakeCodepointBitmapSubpixel`) at
  the same positions. It reports mean and max coverage error and the spread of
  stem widths. I'll show the numbers and the PNG; fix 2 gets built only if
  they're bad.
- **Chroma:** exports are yuv420p, which halves color resolution. Thin colored
  text on a background of similar brightness smears, so code highlight colors
  must differ from the background in luminance, not just hue. The palettes'
  `ink` on `paper` is fine.

## Problem 3: hundreds of glyph objects

- **Per-glyph transforms** want each glyph to be an `an_obj`. Then the timeline,
  stagger, opacity, draw progress, chaos multiplier and handles all work
  unchanged.
  - `an_text` creates a group; each glyph is a child of a new kind,
    `AN_KIND_GLYPH`, pointing at a shared, immutable flattened glyph.
  - `an_text_glyph(text, i)` returns glyph *i* (in string order, spaces
    excluded) for animating it.
- **The cost is in draws, not objects.** Stencil-then-cover per glyph would be
  two draws per glyph: about 2,500 for a 30 × 60 code block.
  - Consecutive glyph siblings with the same paint state **batch**: all their
    stencil fans go in one command and all their covers in the next.
  - Same-color overlaps union correctly under non-zero.
  - Different paint states break the batch, as runs already do.
  - A 30 × 60 block with ~70% non-space characters is about 1,260 glyphs ×
    26 points ≈ 33k fan triangles plus 1,260 cover quads, in 2 draws. The
    200-stroke bench is 216k triangles.
- **Per-glyph opacity:** a glyph below opacity 1 is isolated (rule 11), so a
  per-letter fade costs one small layer per glyph *while it fades*. With a
  stagger that's 10–20 at a time, well inside the pool.
  - A glyph with only one paint (fill only, no shadow or ink) can't overlap
    itself, because the cover is write-once. It can skip isolation and draw with
    alpha directly. I propose that as a later optimization, measured first.
- **Flattening once per (font, glyph)** as you proposed, in font units, at a
  tolerance of 1/4000 em (0.075 px at 300 px).
  - *Measured* at 0.1 px tolerance: 26 points per ASCII glyph at 28 px against
    48 at 300 px. One em-relative flattening costs small text at most about 2x
    points, so per-size-class caches aren't worth it.
  - The glyph cache belongs to the font, which belongs to the scene (arena), and
    is filled at object creation (rule 10).

## Problem 4: boil at code sizes, and the default chaos for text

- **Boil at 28 px:**
  - Boil moves each edge independently by up to `1.5 px × chaos`. A stem's two
    edges sit on different buckets, so its width varies by up to
    `2 × 1.5 × chaos` px.
  - On a 2.52 px stem, chaos 1 can close it completely, and the eye of an "e"
    (about 3 px) fills in.
  - Keeping width variation under 20% of the stem (0.5 px) needs chaos ≤ 0.17.
    That's a sub-pixel wobble: invisible as "hand-drawn", but it makes the 5
    MSAA levels flicker at 12 Hz. Shimmer, no character.
- **Recommendation: text gets a size factor on its chaos, from the stem width
  on screen, recomputed every frame.**

  `size factor = min( clamp((stem_px − 3) / 5, 0, 1),  0.3 × stem_px / (2 × boil) )`

  - `stem_px` is the font's H stem (read from the outline at load) × the em size
    × the glyph's on-screen scale **at this frame** (the square root of the
    world transform's determinant). A text that zooms from 30 px to 120 px
    goes from calm to loud continuously, since both terms are continuous.
  - The second term is a cap I added while checking 40–48 px: without it,
    Archivo Black reaches 1 at 36 px and gets the most relative wobble of any
    size there (stem width varies by up to 37%). The cap holds the variation at
    30% of the stem. `boil` is the style's boil amplitude (default 1.5 px).
  - The factor multiplies the glyph's effective chaos like any multiplier, so
    it scales boil, riso and the shadow offset together.
  - **Override:** `an_text_opts.size_chaos = AN_OFF` turns the factor off (the
    text then follows the scene's chaos and its own multiplier like a shape).
    `an_set_chaos` on the text or a glyph still sets the ordinary multiplier.

  | font | em px | stem px | ramp | factor | boil width variation (2 × amp) | of the stem |
  |---|---|---|---|---|---|---|
  | JetBrains Mono | 28 | 2.52 | 0 | **0** | 0 | 0% |
  | JetBrains Mono | 30 | 2.70 | 0 | **0** | 0 | 0% |
  | JetBrains Mono | 40 | 3.60 | 0.12 | 0.12 | 0.36 px | 10% |
  | JetBrains Mono | 44 | 3.96 | 0.19 | 0.19 | 0.58 px | 15% |
  | JetBrains Mono | 48 | 4.32 | 0.26 | 0.26 | 0.79 px | 18% |
  | Archivo Black | 28 | 6.19 | 0.64 | 0.62 | 1.86 px | 30% |
  | Archivo Black | 36 | 7.96 | 0.99 | 0.80 | 2.39 px | 30% |
  | Archivo Black | 40 | 8.84 | 1 | 0.88 | 2.65 px | 30% |
  | Archivo Black | 44 | 9.72 | 1 | 0.97 | 2.92 px | 30% |
  | Archivo Black | 48 | 10.61 | 1 | **1** | 3.00 px | 28% |
  | Archivo Black | 120 | 26.52 | 1 | 1 | 3.00 px | 11% |

  At 40–48 px the mono stays under the 20% legibility line (it gets a faint
  wobble, which is right for a 48 px "heading in code"), and Archivo Black
  ramps from 0.88 to 1 at a flat 30%.
- **Code text: riso and shadow off by default.** Glyphs that `an_code` creates
  get `.riso = AN_OFF` and `.shadow = AN_OFF`; `an_code_opts` can turn either
  back on. Display text follows the scene's defaults (riso only with ink, as
  for shapes).
- **Riso offset scales with display size.** The style's riso offset (default
  {4, −3}, 5 px) is sized for strokes around 8 px. On text it's multiplied by
  `clamp(em_px / 100, 0.5, 3)`, with `em_px` on screen at this frame: 2.5 px at
  28–50 px, 6 px at 120 px, 15 px at 300 px. Then × chaos, as everywhere.
- **The code block's surroundings keep full chaos:** the box, the gutter rule
  and the highlight bars are separate objects. The block looks hand-made while
  the code stays legible.
- **Boil keying:** each glyph is an object with its own RNG key, so repeated
  letters wobble differently, not "stamped". Each contour is boiled as its own
  closed path (whole buckets, as now). The fill and ink of a contour share its
  ring, so boiled fills follow boiled outlines exactly as on shapes.
  `ani_boil_ring` grows from "first subpath" to "every subpath".

## Kerning, shaping, metrics

- **Kerning:** `stbtt_GetGlyphKernAdvance` reads both the `kern` table and GPOS
  pair adjustment (lookup type 2, including type-9 extension lookups). I
  checked this in the v1.28 source.
  - *Measured:* Archivo Black has A–V −55 and T–o −68 units (−6.6 and −8.2 px at
    120 px). The mono has none, as expected.
  - Applied between consecutive glyphs on a line, at creation. `.kerning = AN_OFF`
    turns it off.
- **No shaping:** stb has no GSUB, so no ligatures (right for code: `!=` stays
  two glyphs), no complex scripts and no mark positioning. v1 is Latin text;
  unknown codepoints render the font's .notdef glyph.
- **Size means the em:** `.size` is the em size in pixels (CSS semantics:
  `stbtt_ScaleForMappingEmToPixels`), not `ScaleForPixelHeight`, which
  scales ascent − descent.
- **Line height:** `(ascent − descent + lineGap) × scale × .line_spacing`
  (default 1). *Measured:* 37.0 px for the mono at 28 px, 130.6 px for Archivo
  Black at 120 px.

## Per-glyph transforms

- **Glyph local space:** origin at the glyph's pen position on the baseline,
  after kerning.
- **Pivot** for rotate and scale: the center of the glyph's advance box at
  half the x-height, so a letter spins and bounces in place.
- The glyph's `pos` is relative to its pen position, so `an_move_by` on a glyph
  offsets it from where layout put it.
- Layout runs once at creation. Animating a glyph never reflows the line.

## Alignment and multi-line

- `.align`: left, center or right, per line, relative to `pos.x`.
- `.anchor` (vertical): top (the first line's ascent), baseline (the first
  line's baseline, the default), middle (the block's center) or bottom (the last
  line's descent).
- `\n` breaks lines. There's no automatic wrapping in v1, and tabs expand to 4
  spaces.

## Draw-on: tag-draw and per-letter stagger

- **Text with ink, `an_draw(text)`:**
  - draw progress runs through the glyphs in reading order, each glyph's
    contours in font order;
  - the ink draws on like a marker;
  - each glyph's fill **pops in** when its last contour closes. A pop needs no
    layer; a fade would.
- **Text without ink:** the same progress reveals whole glyphs in order
  (typewriter).
- **Per-letter stagger:** `an_stagger` over `an_text_glyph` handles, with any
  anim (fade, move, scale, draw). Nothing text-specific in the timeline.

## Code block

`an_code(s, &(an_code_opts){ .code = "...", .size = 30, .line_numbers = true })`
builds a group:

- **One group per line**, holding glyph objects on the monospace grid:
  `x = gutter + column × advance`, snapped (problem 2).
- **Line numbers:** right-aligned in a gutter as wide as the widest number,
  starting at `.first_line` (default 1), in a dimmer color.
- **Per-line highlight:** `an_code_line(code, n)` returns line *n*'s highlight
  bar.
  - It's a rect behind the line, spanning the code width; vertically it covers
    the line box (the baseline minus ascent to the baseline minus descent).
  - It's hidden at first; animate its opacity or fill to highlight the line.
  - It's an ordinary rect, so it boils and casts a shadow at the block's chaos
    while the code stays calm.
- **Fonts:** the code block uses JetBrains Mono. Display text uses Archivo Black
  by default.

## Fonts and dependencies

- **`vendor/stb/stb_truetype.h`, v1.28.**
  - It needs a pin bump: the stb repo is pinned at `6b5799b` (stb_image_write
    v1.19). I'll bump to stb HEAD for both, per the dependency rule, re-run
    everything, and record the bump.
  - stb_image_write may have changed, so the PNG export goes through the full
    test set again.
- **`assets/fonts/`**, with each font's license file next to it, and source
  commits recorded in `assets/fonts/README.md`:
  - **Archivo Black Regular** (OFL 1.1), `ArchivoBlack-Regular.ttf` from
    google/fonts. Static, TrueType outlines, 426 glyphs, Latin and Latin-ext.
  - **JetBrains Mono Regular** (OFL 1.1), the static
    `fonts/ttf/JetBrainsMono-Regular.ttf` from JetBrains/JetBrainsMono.
    Google Fonts only ships the variable font, and stb can't apply variation
    axes: it would render the default instance.
  - Both are checked into `assets/` (about 360 KB). They're content, not code
    dependencies. `make` doesn't fetch them.

## Tests (hand-computed references only)

The expected values come from the font's own tables. The test reads them with
its own small reader (head, hhea, hmtx, cmap format 4, glyf headers; about 80
lines, independent of stb and of our layout code). Kerning values come from
the font through stb's raw `GetGlyphKernAdvance`; positions are computed by hand
in the comments.

- **Advances:** "HHHH" in the mono at 30 px is 4 × 600 × 30 / 1000 = 72 px
  wide. Every pen x is checked.
- **A glyph's box in pixels:** Archivo Black "H" at 120 px, pen at x = 100, chaos
  0, fill only.
  - Stems span font x 74..295 and 538..759, i.e. x 108.88..135.40 and
    164.56..191.08.
  - Pixels inside the stems are fill, the gap between them is paper above and
    below the crossbar, and the cap height comes from glyf's yMax.
- **Kerning:** "AV" at 120 px. V's pen = A's advance × 0.12 + (−55) × 0.12.
  Checked by V's left stem pixel column.
- **Counters:** "O" at 120 px: the counter's center is paper (non-zero; it
  catches a fill that ignores holes). An overlapping-contour glyph, found by
  scanning the font, must not show a hole where its contours overlap (it catches
  even-odd).
- **Multi-line and alignment:** the second baseline is the first + the line
  height (37.0 px for the mono at 28 px). A centered line's left edge is
  `pos.x − width / 2`; right-aligned, its right edge is at `pos.x`.
- **Per-glyph transform:** rotating one glyph by 90° about its pivot maps its
  box as computed by hand; its neighbors don't move.
- **Snapping:** at 30 px, two identical letters in a line are pixel-identical,
  also with the text's origin at x = 100.37 (snapping is relative to the
  origin). Rotated by 0.1 rad, pen positions are not rounded.
- **Draw-on and stagger:** glyph *k*'s fill is absent before its contours close
  and present after, at frames computed from the progress.
- **Code block:** line 10's number is right-aligned with line 9's (same right
  edge, 18 px per digit). Highlight bar *n*'s pixel rows equal the line box.
- **Shadow cut and paper_under on text:** an "o" with a halftone fill and a
  shadow shows paper in the counter, not shadow.
- **Text and shadowed shapes in one layer:** a group at opacity 0.5 holds a
  shadowed rect and a shadowed "o" overlapping it. Composited colors are checked
  against hand-computed values: where both shadows overlap (one write-once
  shadow), where the glyph's shadow falls on the rect's fill, where the rect's
  shadow shows through the "o"'s counter, and inside the glyph.
- **Kerning that fails if the table is ignored:** "AV" in Archivo Black at
  120 px. The test computes both pen positions by hand: kerned
  (A's advance − 55 units) and unkerned. It checks the pixel columns of V's
  left edge at the kerned position, and that the column 6.6 px to the right
  (where an unkerned V would start) is not V's edge. Run with
  `.kerning = AN_OFF`, the same checks must fail, which the test also asserts.
- **Winding overflow:** 16 nested same-direction squares in one batch leave
  the innermost unpainted; 15 and 17 paint it (problem 1).
- **Size factor:** at scene chaos 1, a glyph of 30 px mono text has effective
  chaos 0, a glyph of 48 px Archivo Black has 1, and a glyph of 30 px mono text
  scaled by 4 has the factor of a 10.8 px stem (1). Checked through
  `an_obj_chaos`, which includes the factor.
- **Regression:** the stencil re-encoding (problem 1) leaves every existing
  test bit-identical. Its own step, run before any text code.

## Order of work

1. Pin bump (stb HEAD) and fonts in `assets/`. Everything re-run.
2. Stencil re-encoding, with no behavior change; all tests identical.
3. Glyph loading, the cache and layout (advances, kerning, lines, alignment),
   with tests. No rendering yet.
4. Stencil-then-cover fills, batching, ink, riso, shadow, cut and paper_under
   on glyphs.
5. The small-text measurement (problem 2), shown before deciding on fix 2.
6. Per-glyph transforms, tag-draw, stagger.
7. Code block. `examples/08_text`. Frame bench with a 30 × 60 code block added
   as a scenario.

## As built (2026-11-25)

Where the implementation differs from the proposal above, or settles what it
left open:

- **Letter paths.** The flattening is shared per (font, glyph) as proposed (in
  font units, 1/4000 em). Each letter object then gets its own copy of the
  points scaled to its size, with its pivot at the origin. Stroke widths, boil
  spacing, patterns and the draw-on arc lengths all work on it unchanged. Cost:
  about 0.4 KB per letter in the arena.
- **`pos` of a letter** is pen + pivot in the text's space (the proposal said
  "relative to the pen"); `an_move_by` offsets it from where layout put it, as
  intended.
- **One batch per text object**, not across sibling texts. A text draws as one
  object: [silhouette] all shadows, [silhouette clear], [paper], all fills, all
  outlines. Its silhouette is every letter, so a word's shadow never shows
  inside any of its letters. A code block is one batch per line (about 60
  stencil-then-cover pairs for 30 lines), measured below.
- **Fill runs.** A cover quad paints every nonzero sample under it, so letters
  with different fill colors or patterns can't share a wind/cover pair: a batch
  splits its fills into runs of one fill.
- **Patterns on text** are anchored in the text's space, one grid for all its
  letters (letters' cover quads overlap, so per-letter grids would show seams).
- **Translucent text** (a letter's fill, outline or shadow below alpha 1,
  outside a layer) isolates the whole text: one layer, not one per letter. A
  letter at opacity below 1 gets its own layer, as proposed.
- **Typewriter** (no outline): letter k shows when draw progress x letters >= k + 1.
  With an outline, progress runs by arc length and the fill pops in when the
  letter's outline is complete.
- **Opts.** `an_text_opts` has `.letters_hidden` and `.letters_undrawn` (start
  every letter at opacity 0 / draw 0, for per-letter stagger) and `.size_chaos`.
  `an_code_text(code, n)` returns a line's text. In a code block, the box is at
  z -2 and the bars at z -1, behind every line.
- **Fonts are embedded** in the library (`src/fonts.S`, `.incbin`), so programs
  don't depend on the working directory. `an_font_load` reads other files.
- **No stencil buffer, no text:** a renderer without a depth-stencil main
  target skips glyph fills. Both targets always have one (rule 7).
- **Over-budget replay** keeps the winding and cover passes (they only need the
  target's stencil) and drops the cut, as for shapes.

Measured:

- **Small text (problem 2), `examples/spike_text_quality`.** JetBrains Mono at
  30 px through the pipeline (4x MSAA, snapped) against stb_truetype's 256-level
  rasterizer at the same pen positions: mean coverage error 0.045, max 0.52,
  1.7% of inked pixels off by more than 0.25 (all on edges). Stems 2.77 px vs
  stb's 2.72 ('l'), identical for every repeated letter. At 28 px: mean 0.048,
  1.4% over 0.25. **Fix 2 (supersampled text layers) isn't needed** and wasn't
  built. Comparison image: `out/text_quality_30.png`.
- **Code block cost**, `spike_frame_bench 5`: a 30 x 60 block at 24 px (~70%
  non-space, line numbers, a highlight stepping down the lines), 131k triangles:
  median 6.1 ms, p99 6.6–6.8 ms, 0 frames over 12 ms (3 runs, CPU quiet).
- **Overlapping contours in the fonts:** not scanned for. The overlap test
  stacks two letters in one batch instead, which reaches the same code (winding
  2 must fill) and doesn't depend on what the fonts happen to contain.
