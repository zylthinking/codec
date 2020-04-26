
#if defined(__ANDROID__)
#include "fmt.h"
#include "mydef.h"
#include "vendor.h"
#include <string.h>

#define OMX_TI_COLOR_FormatYUV420PackedSemiPlanar 0x7f000100

static void ti_nv12_copy(media_buffer* meida, char* src, intptr_t bytes, panel_t dst_size[3], panel_t src_size[3])
{
    char* uv = src + bytes - src_size[1].bytes;
    for (int i = 0; i < 2; ++i) {
        uintptr_t stride = my_min(dst_size[i].width, src_size[i].width);
        uintptr_t lines = my_min(dst_size[i].height, src_size[i].height);
        if (dst_size[i].width == src_size[i].width) {
            memcpy(meida->vp[i].ptr, src, dst_size[i].width * lines);
        } else {
            char* dst = meida->vp[i].ptr;
            for (int j = 0; j < lines; ++j) {
                memcpy(dst, src, stride);
                dst += dst_size[i].width;
                src += src_size[i].width;
            }
        }

        intptr_t y_bytes = src_size[1].width * (src_size[1].height - dst_size[1].height);
        if (y_bytes < 0) {
            y_bytes = 0;
        }

        intptr_t x_bytes = (src_size[1].width - dst_size[1].width);
        if (x_bytes < 0) {
            x_bytes = 0;
        }
        src = uv + x_bytes + y_bytes;
    }
}

video_format* ti_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr)
{
    video_format* fmt = NULL;
    if (OMX_TI_COLOR_FormatYUV420PackedSemiPlanar == csp) {
        static vendor_ops_t ti_ops = {
            .panel_copy = ti_nv12_copy,
            .size_fixup = NULL
        };

        if (ops_ptr) {
            ops_ptr[0] = &ti_ops;
        }
        fmt = video_format_get(codec_pixel, csp_nv12, width, height);
    } else {
        logmsg("ti csp %x does not support yet\n", csp);
    }
    return fmt;
}
#endif
