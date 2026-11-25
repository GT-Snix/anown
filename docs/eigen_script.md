# Eigenvectors: the directions that don't turn (examples/12_eigen)

The brief (`~/Downloads/eigenvectors.txt`) with the approved corrections
(2026-11-25). Built on `an_plane` (docs/plane.md). Target length 8:10.

## Global

**The plane:**
- Origin at the frame center; 1 unit = 100 px at 1080p (scaled with the
  frame).
- Cream paper, static grain.
- The grid has chaos 0.3, no shadow, and is thin.
- Arrows have full chaos and filled heads.

**The main transformation** is A = [[3, 1], [0, 2]]:

| Eigenvector | λ | Color |
|---|---|---|
| u1 = (1, 0) | 3 | magenta #E6007E |
| u2 = (1, −1) | 2 | cyan #00B4D8 |

- Ordinary vectors are charcoal. Highlights and probes are safety orange.
- Archivo Black is used for titles, JetBrains Mono for math. λ is missing from
  Archivo Black: titles spell "lambda" or set it in mono.
- Arrowheads, dots, ticks and labels use `.riso = AN_OFF, .shadow = AN_OFF`
  (no effect caps).

**Timing:**
- Durations live in one table: `DUR = { 30, 60, 90, 120, 90, 70, 30 }`
  (seconds, chapters 0–6).
- Each chapter ends with a hold to its duration.

**Structure:**
- One function per chapter, all on the same plane.
- Each chapter creates its objects inside its own fixed-name group
  (`"ch0"` … `"ch6"`), so its random keys don't depend on other chapters.
- `--chapter N` first sets the scene instantly to the state chapter N − 1
  ends in (the plane's matrix and the visible objects), then plays chapter N.
  Its frames have the same pixels as the same frames of the full render.

**No subtitles.** The narration below is only a timing guide.

## The 24-arrow ring and its count

Arrows from the origin every 15°. An arrow stays on its line when A v ∥ v
(cross product 0, to 1e-6 relative). That's true for 0° and 180° (along u1)
and for 135° and 315° (along u2): four arrows. So **20 of 24 leave their
line**. The script computes that count; nothing is hardcoded.

## Chapter 0: Hook (30 s)

1. The grid and the 24 charcoal unit arrows.
2. "EIGENVECTORS" slaps in with a shake, holds, then shrinks to a corner.
3. A is applied over 3 s: every arrow swings to its image.
4. The **four** arrows that stay on their lines turn magenta (0°, 180°) and
   cyan (135°, 315°).
5. Hold 3 s.

## Chapter 1: Vectors and directions (60 s)

1. Reset the plane to the identity. Draw the axes.
2. v = (2, 1) pops in, in charcoal.
3. A long thin line fades in through it: its line of directions.
4. A probe arrow sweeps once around the origin.
5. Dots appear at grid points, then arrows draw to a few of them.

## Chapter 2: A transformation moves everything (90 s)

1. Only the basis arrows (1, 0) and (0, 1) are shown.
2. Apply A over 3 s: the grid warps, and the basis arrows land on (3, 0) and
   (1, 2). Hold, then reset.
3. v = (1, 1) moves to (4, 2). The original 45° line is shown as a **thin,
   pale solid line**, and the image is off it.
4. Repeat with (0, 1) → (1, 2).
5. Finish with the 24 arrows, replayed slower. The counter "20 of 24 left
   their line" is **one static text object**, computed at script time.

## Chapter 3: Eigenvectors and eigenvalues (120 s)

**a) Probe:**
- A unit orange arrow v sweeps from −30° to 150° over 20 s, with a lighter
  arrow showing A v.
- A readout shows the angle between v and A v.
- It hits 0 at 0° and at 135°. At each hit, a ring slaps on, magenta (u1) or
  cyan (u2), and the sweep holds for 2 s.

**b) Span lines:**
- At each hit, the full line is drawn in its color.
- Replay A over 3 s: points slide along the lines, stretching ×3 (magenta)
  and ×2 (cyan).
- Ticks with "×3" and "×2" mark the stretch.

**c) The equation:** "A v = λ v" in mono, split by two callouts into "same
direction" and "just scaled", then λ1 = 3 and λ2 = 2.

**d) The name:** a callout, "eigen = German for own, or characteristic".

## Chapter 4: Edge cases (90 s; built later)

- **a)** B = [[−1, 0], [0, 2]]: the x-axis arrow flips but stays on its line.
- **b)** C = [[1, 1], [1, 1]]: the plane collapses onto y = x.
- **c)** R = rotation by 90°: animated through three keyframes (30°, 60°, 90°)
  so lengths don't shrink. Callout: "no arrow stays on its line: no real
  eigenvectors".

## Chapter 5: Why it matters (70 s; built later)

- **Power iteration** from v0 = (0, 1): rescaled to unit length after each
  step ("rescaled to fit"). Its angles, computed: about 90, 63.4, 38.7, 22.8,
  13.8, 8.6 and 5.5°.
- **Then the ring:** every arrow is normalized after every step and pulled
  toward the magenta line. The two ring arrows on the cyan line (135°, 315°)
  stay put.
- **Narration:** "...it swings toward the eigenvector with the biggest
  stretch, from almost anywhere."

## Chapter 6: Recap (30 s)

1. The hook scene again, labeled "direction that survives" on the magenta and
   cyan lines, and "lambda = how much it stretches".
2. An end card with the title.

## Build notes

- Preview at 960×540, 30 fps, one chapter at a time.
- Stills: `--every 30` into `out/eigen/`.
- Ship order: chapters 0–3 and 6, then 4, then 5.
- Missing features are listed, not built.
