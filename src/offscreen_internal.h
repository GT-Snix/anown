// Internal: headless Metal setup, offscreen render targets, GPU->CPU readback.
// Not part of the public API. Implemented in src/offscreen.m.
#ifndef ANOWN_OFFSCREEN_INTERNAL_H
#define ANOWN_OFFSCREEN_INTERNAL_H

#include "anown.h"
#include "sokol_gfx.h"

#include <stdbool.h>
#include <stddef.h>

// Set up sokol_gfx without a window, on the system default MTLDevice.
// Defaults: RGBA8 color, no depth, 1 sample. Returns false on failure.
bool ani_gfx_setup_headless(void);
void ani_gfx_shutdown(void);

// Autorelease pool around a frame. A headless program has no AppKit run loop
// draining pools, so wrap each frame (and setup) in push/pop.
void *ani_pool_push(void);
void ani_pool_pop(void *pool);

typedef struct ani_offscreen {
    int width, height, sample_count;
    sg_image image;        // RGBA8 color attachment (multisampled if sample_count > 1)
    sg_view color_view;
    sg_image resolve;      // sample_count > 1 only: single-sampled resolve target
    sg_view resolve_view;
    void *staging;         // retained id<MTLBuffer>, shared storage
} ani_offscreen;

// sample_count 0 or 1: no MSAA. With MSAA, readback reads the resolved image.
bool ani_offscreen_make(ani_offscreen *off, int width, int height, int sample_count);
void ani_offscreen_destroy(ani_offscreen *off);

// Begin a pass into the target, cleared to `clear`. End it with sg_end_pass().
void ani_offscreen_begin(const ani_offscreen *off, an_color clear);

// Bytes in one frame: width * height * 4.
size_t ani_offscreen_frame_bytes(const ani_offscreen *off);

// Copy the target into dst as tightly packed RGBA8, rows top-down.
// Call after sg_commit(). Blocks until the GPU has finished the frame.
void ani_offscreen_readback(ani_offscreen *off, void *dst);

#endif
