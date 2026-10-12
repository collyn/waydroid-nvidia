/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WNV_HWC_PITCH_H
#define WNV_HWC_PITCH_H
#include <stdbool.h>
#include <stdint.h>
/* Hybrid RGB imports require 256-byte row alignment, not 256 pixels.
 * Preserve the existing admission policy for unknown pixel formats. */
static inline bool hwc_rgb32_pitch_aligned(uint32_t pixel_stride, bool rgb32) {
    return !rgb32 || pixel_stride % (256 / 4) == 0;
}
/* Client framebuffer targets and overlay layers share this policy. */
static inline bool hwc_buffer_needs_shm(uint32_t pixel_stride, bool rgb32,
                                      bool compositor_importable) {
    return !hwc_rgb32_pitch_aligned(pixel_stride, rgb32) || !compositor_importable;
}
#endif
