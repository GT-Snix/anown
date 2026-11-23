/* anown: an animation library for explainer videos.
 * Public API. This is the only header users include.
 */
#ifndef ANOWN_H
#define ANOWN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AN_VERSION_MAJOR 0
#define AN_VERSION_MINOR 0
#define AN_VERSION_PATCH 1

/* RGBA in 0..1, sRGB-encoded (the same values you'd write in hex). */
typedef struct an_color {
    float r, g, b, a;
} an_color;

/* Build an opaque color from 0xRRGGBB. */
#define AN_HEX(rgb)                                         \
    ((an_color){ (float)(((rgb) >> 16) & 0xFF) / 255.0f,    \
                 (float)(((rgb) >> 8) & 0xFF) / 255.0f,     \
                 (float)((rgb) & 0xFF) / 255.0f, 1.0f })

#define AN_CREAM AN_HEX(0xF3EDE2)

/* Options for an_window_run. Zero fields get defaults:
 * title "anown", 1280x720, clear color AN_CREAM. */
typedef struct an_window_desc {
    const char *title;
    int width;
    int height;
    an_color clear; /* alpha 0 (the zero value) means AN_CREAM */
} an_window_desc;

/* Open a window and clear it every frame until it is closed. */
void an_window_run(const an_window_desc *desc);

/* Convert 0xRRGGBB to an_color (function form of AN_HEX). */
an_color an_hex(uint32_t rgb);

#ifdef __cplusplus
}
#endif

#endif /* ANOWN_H */
