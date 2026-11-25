# Experiment: 24-byte vertices (not adopted)

`vertex24.patch` (against the tree of 2026-11-24, after the shadow cut) takes the
pattern parameters off the vertex and puts them on the draw command:

- `ani_vertex` goes back to `x, y, r, g, b, a` (24 bytes, from 36).
- `ani_cmd` carries the pattern as an affine map from scene position to pattern
  coordinates (`pu`, `pv`; kind + density in `pu[3]`). It's sent as a per-draw
  uniform, and the vertex shader computes the coordinate from position.
- A patterned fill is always its own command (merge barriers before and after);
  everything else merges as before.

Apply from the repo root with `patch -p1 < docs/experiments/vertex24.patch`
(it applied cleanly on 2026-11-24, checked with `--dry-run`).

## Measured (2026-11-24)

- All 15 test programs pass, with the same pattern coverage numbers.
- `06_look`, 10 frames: 656 of 20.7 M pixels differ, each by exactly 1 level
  (the coordinate is computed on the GPU from the affine instead of per vertex).
- Clean build: 4.0 s for both layouts.
- Stroke build, 200 strokes (`test_stroke`), alternated run by run at load ~2.7:
  - average: 3.45–3.55 ms (24 bytes) vs 3.72–3.74 ms (36 bytes), about 7% faster;
  - everything drawn: 6.80–6.90 vs 7.34–7.47 ms.
- Frame time: not measured (the machine never got quiet enough).

## Decision

Keep 36 bytes for now (2026-11-24). Revisit if the frame bench misses the 12 ms
ceiling; this is the first thing to try.
