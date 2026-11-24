#include "render_internal.h"

#include "shape.glsl.h"

bool ani_renderer_init(ani_renderer *r, sg_pixel_format color_format,
                       sg_pixel_format depth_format, int sample_count) {
    *r = (ani_renderer){ 0 };
    ani_mesh_init(&r->mesh);
    r->pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(shape_shader_desc(sg_query_backend())),
        .layout.attrs = {
            [ATTR_shape_pos].format = SG_VERTEXFORMAT_FLOAT2,
            [ATTR_shape_color0].format = SG_VERTEXFORMAT_FLOAT4,
        },
        .colors[0] = {
            .pixel_format = color_format,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                // keep destination alpha at 1 over an opaque background
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
        .depth.pixel_format = depth_format, // compare defaults to ALWAYS, no writes
        .sample_count = sample_count,
        .label = "anown-shape",
    });
    return sg_query_pipeline_state(r->pip) == SG_RESOURCESTATE_VALID;
}

void ani_renderer_shutdown(ani_renderer *r) {
    sg_destroy_buffer(r->vbuf);
    sg_destroy_pipeline(r->pip);
    ani_mesh_free(&r->mesh);
    *r = (ani_renderer){ 0 };
}

bool ani_renderer_draw(ani_renderer *r, const an_scene *s, int target_w, int target_h) {
    if (!ani_tess_scene(&r->mesh, s)) return false;
    const size_t bytes = (size_t)r->mesh.count * sizeof(ani_vertex);

    if (bytes > r->vbuf_bytes) {
        size_t cap = r->vbuf_bytes ? r->vbuf_bytes : 64 * 1024;
        while (cap < bytes) cap *= 2;
        sg_destroy_buffer(r->vbuf);
        r->vbuf = sg_make_buffer(&(sg_buffer_desc){
            .usage = { .vertex_buffer = true, .write_transient = true },
            .size = cap,
            .label = "anown-shape-vertices",
        });
        r->vbuf_bytes = cap;
    }
    // must happen before the buffer is bound in this frame
    sg_write_buffer_transient(&(sg_write_buffer_desc){
        .src.data = { .ptr = r->mesh.v, .size = bytes },
        .dst.buffer = r->vbuf,
    });

    // letterbox: fit the scene's aspect ratio inside the target, centered
    const float sw = (float)s->width, sh = (float)s->height;
    const float k = (float)target_w / sw < (float)target_h / sh ? (float)target_w / sw
                                                                : (float)target_h / sh;
    const float vw = sw * k, vh = sh * k;
    sg_apply_viewportf(((float)target_w - vw) * 0.5f, ((float)target_h - vh) * 0.5f, vw, vh, true);

    const shape_params_t params = { .scene_size = { sw, sh } };
    sg_apply_pipeline(r->pip);
    sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = r->vbuf });
    sg_apply_uniforms(UB_shape_params, &SG_RANGE(params));
    sg_draw(0, (uint32_t)r->mesh.count, 1);
    return true;
}
