
#if defined(__ANDROID__)
#include "fmt.h"
#include "mydef.h"
#include "vendor.h"
#include <media/stagefright/OMXCodec.h>

static void sec_nv12_size_fixup(int32_t w1, int32_t h1, int32_t w0, int32_t h0, panel_t pane[3])
{
    h1 = h0;
    pane[0].width = panel_width(0, w1, csp_nv12);
    pane[0].width = pixels_to_bytes(0, pane[0].width, csp_nv12);
    pane[0].height = panel_height(0, h1, csp_nv12);
    pane[0].bytes = pane[0].width * pane[0].height;

    pane[1].width = pane[0].width;
    pane[1].height = panel_height(1, pane[0].height, csp_nv12);
    pane[1].bytes = pane[1].width * pane[1].height;
}

extern "C" video_format* sec_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr)
{
    video_format* fmt = NULL;
    if (csp == OMX_COLOR_FormatYUV420SemiPlanar) {
        static vendor_ops_t sec_ops = {
            .panel_copy = generic_panel_copy,
            .size_fixup = sec_nv12_size_fixup
        };

        if (ops_ptr) {
            ops_ptr[0] = &sec_ops;
        }
        fmt = video_format_get(codec_pixel, csp_nv12, width, height);
    }
    return fmt;
}

#endif
