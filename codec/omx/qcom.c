
#if defined(__ANDROID__)
#include "fmt.h"
#include "mydef.h"
#include "vendor.h"
#include "android.h"
#include <string.h>

static void qcom_nv12_32m_size_fixup(int32_t w1, int32_t h1, int32_t w0, int32_t h0, panel_t pane[3])
{
    pane[0].width = panel_width(0, w1, csp_nv12);
    pane[0].width = pixels_to_bytes(0, pane[0].width, csp_nv12);
    pane[0].width = roundup(pane[0].width, 128);
    pane[0].height = panel_height(0, h1, csp_nv12);
    pane[0].height = roundup(pane[0].height, 32);
    pane[0].bytes = pane[0].width * pane[0].height;

    pane[1].width = pane[0].width;
    pane[1].height = panel_height(1, pane[0].height, csp_nv12);
    pane[1].bytes = pane[1].width * pane[1].height;
}

//===============================================

static void qcom_nv12_32m4ka_size_fixup(int32_t w1, int32_t h1, int32_t w0, int32_t h0, panel_t pane[3])
{
    qcom_nv12_32m_size_fixup(w1, h1, w0, h0, pane);
}

//===============================================

#define block_width 64
#define block_height 32
#define block_full (block_width * block_height)
#define block_half (block_full / 2)

#define pixel_address(base, x, y, w, h) \
({ \
    char* addr = base; \
    intptr_t idx = x + (y & ~1) * w; \
    if (y & 1) { \
        idx += (x & ~3) + 2; \
    } else if ((h & 1) == 0 || y != (h - 1)) { \
        idx += (x + 2) & ~3; \
    } \
    addr += idx * block_full; \
    addr; \
})

static void qcom_nv12_64x32Tile2m8ka_size_fixup(int32_t w1, int32_t h1, int32_t w0, int32_t h0, panel_t pane[3])
{
    pane[0].width = panel_width(0, w1, csp_nv12);
    pane[0].width = pixels_to_bytes(0, pane[0].width, csp_nv12);
    pane[0].width = roundup(pane[0].width, block_width * 2);
    pane[0].height = panel_height(0, h1, csp_nv12);
    pane[0].height = roundup(pane[0].height, block_height);
    pane[0].bytes = pane[0].width * pane[0].height;
    pane[0].bytes = roundup(pane[0].bytes, 8192);

    pane[1].width = panel_width(1, w1, csp_nv12);
    pane[1].width = pixels_to_bytes(1, pane[1].width, csp_nv12);
    pane[1].width = roundup(pane[1].width, block_width * 2);
    pane[1].height = panel_height(1, h1, csp_nv12);
    pane[1].height = roundup(pane[1].height, block_height);
    pane[1].bytes = pane[1].width * pane[1].height;
    pane[1].bytes = roundup(pane[1].bytes, 8192);
}

static void qcom_nv12_64x32Tile2m8ka_copy(media_buffer* meida, char* src,
                        intptr_t bytes, panel_t dst_size[3], panel_t src_size[3])
{
    size_t height = meida->vp[0].height;
    intptr_t stride = meida->vp[0].stride;
    size_t x_blocks = (meida->vp[0].stride - 1) / block_width + 1;
    size_t y_blocks = (height - 1) / block_height + 1;

    size_t tile_x = src_size[0].width / block_width;
    size_t tile_y0 = src_size[0].height / block_height;
    size_t tile_y1 = (src_size[0].height / 2 - 1) / block_height + 1;

    intptr_t even = 0;
    char* bottom[x_blocks];

    char* dst_0 = meida->vp[0].ptr;
    char* dst_1 = meida->vp[1].ptr;
    for(intptr_t y = 0; y < y_blocks; y++) {
        bytes = stride;
        char* block_0 = dst_0;
        char* block_1 = dst_1;
        even = !even;

        for(intptr_t x = 0; x < x_blocks; x++) {
            dst_0 = block_0;
            dst_1 = block_1;

            char* src_1 = NULL;
            char* src_0 = pixel_address(src, x, y, tile_x, tile_y0);
            if (even == 1) {
                intptr_t n = y / 2;
                src_1 = pixel_address(src + src_size[0].bytes, x, n, tile_x, tile_y1);
                bottom[x] = src_1 + block_half;
            } else {
                src_1 = bottom[x];
            }

            size_t nb = my_min(bytes, block_width);
            size_t lines = my_min(height, block_height);
            while (lines > 1) {
                memcpy(dst_0, src_0, nb);
                dst_0 += stride;
                src_0 += block_width;

                memcpy(dst_0, src_0, nb);
                dst_0 += stride;
                src_0 += block_width;

                memcpy(dst_1, src_1, nb);
                dst_1 += stride;
                src_1 += block_width;

                lines-= 2;
            }

            if (lines == 1) {
                memcpy(dst_0, src_0, nb);
                dst_0 += nb;
                memcpy(dst_1, src_1, nb);
                dst_1 += nb;
            } else {
                dst_0 -= (stride - nb);
                dst_1 -= (stride - nb);
            }
            block_0 += block_width;
            block_1 += block_width;
            bytes -= block_width;
        }
        height -= block_height;
    }
}

video_format* qcom_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr)
{
    video_format* fmt = NULL;
    if (0x7FA30C04 == csp) {
        static vendor_ops_t qcom_ops = {
            .panel_copy = generic_panel_copy,
            .size_fixup = qcom_nv12_32m_size_fixup
        };

        if (ops_ptr) {
            ops_ptr[0] = &qcom_ops;
        }
        fmt = video_format_get(codec_pixel, csp_nv12, width, height);
    } else if (0x7fa30c03 == csp) {
        static vendor_ops_t qcom_ops = {
            .panel_copy = qcom_nv12_64x32Tile2m8ka_copy,
            .size_fixup = qcom_nv12_64x32Tile2m8ka_size_fixup
        };

        if (ops_ptr) {
            ops_ptr[0] = &qcom_ops;
        }
        fmt = video_format_get(codec_pixel, csp_nv12, width, height);
    } else if (0x7fa30c01 == csp) {
        static vendor_ops_t qcom_ops = {
            .panel_copy = generic_panel_copy,
            .size_fixup = qcom_nv12_32m4ka_size_fixup
        };

        if (ops_ptr) {
            ops_ptr[0] = &qcom_ops;
        }

        int64_t* feature = android_feature_address();
        if (feature[bug_mask] & bug_nv12_nv21) {
            fmt = video_format_get(codec_pixel, csp_nv21, width, height);
        } else {
            fmt = video_format_get(codec_pixel, csp_nv12, width, height);
        }
    } else {
        logmsg("qcom csp %x does not support yet\n", csp);
    }
    return fmt;
}

#endif
