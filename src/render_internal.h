// Internal: draws a scene into the current sokol pass. The same code serves the
// preview window and the offscreen export; only the pass and target differ.
#ifndef ANOWN_RENDER_INTERNAL_H
#define ANOWN_RENDER_INTERNAL_H

#include "sokol_gfx.h"
#include "tess_internal.h"

typedef struct ani_renderer {
    sg_pipeline pip;
    sg_buffer vbuf;
    size_t vbuf_bytes;
    ani_mesh mesh;
} ani_renderer;

// Formats and sample count must match the passes it will draw into (the preview
// swapchain has a depth buffer; offscreen targets don't). Depth is never tested.
bool ani_renderer_init(ani_renderer *r, sg_pixel_format color_format,
                       sg_pixel_format depth_format, int sample_count);
void ani_renderer_shutdown(ani_renderer *r);

// Call inside a pass whose attachments are target_w x target_h pixels.
// The scene is scaled to fit and centered (letterboxed); the area outside is
// left as the pass cleared it. Returns false on allocation failure.
bool ani_renderer_draw(ani_renderer *r, const an_scene *s, int target_w, int target_h);

#endif
