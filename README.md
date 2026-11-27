# anown

anown is a small C library for making explainer videos, the kind where boxes
slide around to show how a sort works, or a tree grows one node at a time. It's
in the same spirit as Manim, but I wanted a louder look: cream paper, flat ink
colors, thick wobbly outlines, a bit of print misregistration and grain. Less
lecture slide, more photocopied zine.

You write a normal C program. It builds a scene, says what should move and when,
and then either opens a preview window or writes an mp4. That's all there is to
it. There's no editor and no scripting language.

## Before you start: it only runs on Apple Silicon Macs

I'm saying this first because it's the thing most likely to trip you up. anown
draws with Metal and the build targets `arm64` macOS only. It will not build on
Windows, Linux or an Intel Mac. I'm not planning to port it for now.

What you need:

- An M1/M2/M3/M4 Mac.
- The Xcode Command Line Tools, for clang and the macOS SDK:
  `xcode-select --install`
- `ffmpeg` on your PATH if you want to export video: `brew install ffmpeg`.
  The preview window and PNG output work without it.
- An internet connection the first time you build. `make` downloads the
  shader compiler (`sokol-shdc`) into `tools/`.

Nothing else. The other dependencies (sokol, stb) are single headers and are
already in `vendor/`.

## Building it

```sh
git clone https://github.com/GT-Snix/anown.git
cd anown
make
```

That builds the library (`build/libanown.a`), all the examples and the tests.
On my machine a clean build takes a few seconds. To check everything works:

```sh
make test                  # headless tests, needs a Metal GPU but no window
make run EX=00_hello       # opens a cream window, nothing else
make run EX=10_quicksort   # the quicksort explainer in a preview window
make example-quicksort     # the same thing, written to out/quicksort.mp4
```

In the preview window, Space pauses and the left/right arrow keys step one frame
while paused. Close the window to quit.

If `make` stops while downloading, run `make deps` again once you're back
online. If the export says `ffmpeg not found on PATH`, install ffmpeg.

## Your first video

Make a folder next to the anown checkout and put this in `main.c`:

```c
#include "anown.h"
#include <string.h>

int main(int argc, char **argv) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 7 });
    if (!s) return 1;

    an_obj *title = an_text(s, &(an_text_opts){
        .text = "swap two numbers", .pos = { 960, 260 }, .size = 96,
        .align = AN_ALIGN_CENTER, .hidden = true,
    });
    an_play(s, an_fade_to(title, 1, NULL));

    const int values[] = { 4, 9, 2, 7 };
    an_obj *arr = an_array(s, &(an_array_opts){
        .pos = { 960, 600 }, .values = values, .count = 4,
    });

    an_play(s, an_array_swap(arr, 1, 2, NULL));
    an_play(s, an_slap(arr, NULL));
    an_wait(s, 1.0f);

    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        int64_t n = an_scene_export(s, &(an_export_desc){ .path = argv[2] });
        an_scene_destroy(s);
        return n > 0 ? 0 : 1;
    }
    an_scene_preview(s);
    return 0;
}
```

Build it against the library:

```sh
clang -std=c11 -I../anown/include main.c ../anown/build/libanown.a \
    -framework Metal -framework AppKit -framework QuartzCore -o myvideo

./myvideo                      # preview
./myvideo --export swap.mp4    # 1080p60 mp4
```

The title fades in, the 9 and the 2 trade places, and the whole row gets a
little slap. If that works, you have everything set up.

The only header you ever include is `include/anown.h`. The fonts are compiled
into the library, so there are no files to copy around. You just need the `.a`
and the three frameworks.

## How it thinks

A few ideas cover most of the library.

**A scene is a timeline with a cursor.** `an_play` runs an animation and moves
the cursor to where it ends. `an_play_async` starts one and leaves the cursor
where it was, so the next thing happens at the same time. `an_wait` moves the
cursor forward. `an_par` plays several at once and waits for all of them. Your
program runs top to bottom once, recording all of this, and only then does
anything get drawn. That's why the quicksort example can just run quicksort in
plain C and call `an_play` at each step.

**Options are structs, and zero means default.** Every function that makes
something takes an options struct, and you only fill in the fields you care
about:

```c
an_rect(s, &(an_rect_opts){ .pos = {960, 540}, .size = {200, 100} });
an_circle(s, NULL);   /* NULL means all defaults */
```

**Coordinates are pixels on a 1920x1080 page.** That's the default size.
(0, 0) is the top left and y goes down. Rotation is in radians, clockwise.

**You never free objects.** Everything belongs to the scene and goes away with
`an_scene_destroy`.

**Same program, same video.** Time is a frame number, not the clock, and all
randomness comes from the scene's seed. If you run it twice you get identical
frames. Change `.seed` if you want a different wobble.

## What's in the box

- **Shapes:** rectangles, circles, lines, arrows, polygons, free strokes, groups.
- **Animation:** move, scale, rotate, fade, recolor, draw-on for outlines,
  arcs, springs and easing, plus a few punchy "verbs" like `an_slap`, `an_pop`,
  `an_shake` and squash-and-stretch.
- **Text and code:** a display font and a monospace font are built in.
  `an_code` shows a code snippet with line numbers and a marker you can move
  from line to line.
- **Arrays:** cells or bars, swaps, highlights, named states (active, dimmed,
  done) and pointers under the cells.
- **Graphs and trees:** nodes and edges with tree and layered layouts that can
  animate as the graph changes.
- **Plots:** axes, function plots and data plots.
- **Callouts:** little labels that point at things.
- **Camera:** pan, zoom and rotate the whole view.
- **Planes and a 3D space:** a 2D grid you can apply matrices to, and an early
  3D space with vectors, lines and a sphere. The 3D part is newer than the
  rest and still rough.
- **The look:** three palettes (`riot`, `hazard`, `acid`), plus grain, line
  boil, shadows and riso offset. All of it scales with a single "chaos" dial
  (`an_scene_set_chaos`), so you can calm it down to 0 if you want clean lines.
- **Output:** a preview window, mp4 export through ffmpeg, single PNG frames,
  and PNG frame dumps.

## Where to look next

The examples are the best documentation. Each one is a single `main.c` and
they get more involved as the numbers go up:

| example | what it shows |
|---|---|
| `00_hello` | an empty window |
| `01_shapes` | every object kind in one still scene |
| `02_timeline` | a bubble sort pass, the timeline basics |
| `03_strokes` | outlines drawing themselves on |
| `05_style`, `06_look`, `07_palettes` | the look, the chaos dial, the palettes |
| `08_text` | a title and a code snippet |
| `09_camera` | camera moves and verbs |
| `10_quicksort` | a full explainer: array, code panel, pointers, callouts |
| `11_bst` | a binary search tree growing, then a plot |
| `12_eigen` | a longer video about eigenvectors, split into chapters |
| `13_space` | the 3D space |

`include/anown.h` has a comment on every public function and every field.
The `docs/` folder has longer write-ups: `timeline.md`, `text.md`,
`camera.md`, `graphs.md`, `plane.md`, `space.md`. `docs/progress.md` is my
running log of what works and what's still stubbed.

## Honest limits

- Apple Silicon macOS only, as I said up top.
- It's version 0.1. The API still changes when I find something awkward, so
  pin a commit if you build something on it.
- There's no LaTeX, no audio and no GUI. You add narration and music in a video
  editor afterwards.
- It's not on Homebrew or any package manager. You build it from source and
  link the `.a` yourself.
- The 3D space is early and only does what the `13_space` example needs.

## Credits

Rendering goes through [sokol](https://github.com/floooh/sokol) by Andre
Weissflog, text through
[stb_truetype](https://github.com/nothings/stb) by Sean Barrett. The built-in
fonts are Archivo Black and JetBrains Mono, both under the SIL Open Font
License; their licenses are in `assets/fonts/`.
