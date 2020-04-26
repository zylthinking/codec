
#if defined(__ANDROID__)
#include "fmt.h"
#include "mydef.h"
#include "vendor.h"
#include <string.h>

#define block_width 16
#define block_height 32

static void mtk_size_fixup(int32_t w1, int32_t h1, int32_t w0, int32_t h0, panel_t pane[3])
{
    pane[0].width = panel_width(0, w1, csp_nv12);
    pane[0].width = roundup(pane[0].width, block_width);
    pane[0].width = pixels_to_bytes(0, pane[0].width, csp_nv12);
    pane[0].height = panel_height(0, h1, csp_nv12);
    pane[0].height = roundup(pane[0].height, block_height);
    pane[0].bytes = pane[0].width * pane[0].height;

    pane[1].width = panel_width(1, w1, csp_nv12);
    pane[1].width = roundup(pane[1].width, block_width / 2);
    pane[1].width = pixels_to_bytes(1, pane[1].width, csp_nv12);
    pane[1].height = panel_height(1, h1, csp_nv12);
    pane[1].height = roundup(pane[1].height, block_height / 2);
    pane[1].bytes =  pane[1].width * pane[1].height;
}

static void do_16x32_tile_copy(char* dst, char* src, panel_t* dst_size, panel_t* src_size, intptr_t idx)
{
    my_assert(0 == (src_size->width & (block_width - 1)));
    intptr_t bytes = my_min(dst_size->width, src_size->width);
    intptr_t lines = my_min(dst_size->height, src_size->height);
    intptr_t chunk_bytes, y_step = block_height, x_step = block_width;
    if (idx != 0) {
        y_step /= 2;
    }
    intptr_t nb = 0;

LABEL0:
    chunk_bytes = y_step * dst_size->width;

    for (; lines >= y_step; lines -= y_step) {
        char* pch = dst;
        intptr_t total = bytes;
LABEL1:
        for (; total >= x_step; total -= x_step) {
            char* addr = pch;
            for (int i = 0; i < y_step; ++i) {
                memcpy(addr, src, x_step);
                src += block_width;
                addr += dst_size->width;
            }
            src += nb;
            pch += x_step;
        }

        if (total > 0) {
            x_step = total;
            goto LABEL1;
        }
        dst += chunk_bytes;
    }

    if (lines > 0) {
        nb = (y_step - lines) * block_width;
        y_step = lines;
        goto LABEL0;
    }
}

static void mtk_16x32_tile_copy(media_buffer* meida, char* src, intptr_t bytes, panel_t dst_size[3], panel_t src_size[3])
{
    for (int i = 0; i < 2; ++i) {
        do_16x32_tile_copy(meida->vp[i].ptr, src, &dst_size[i], &src_size[i], i);
        src += src_size[i].bytes;
    }
}

video_format* mtk_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr)
{
    static vendor_ops_t mtk_ops = {
        .panel_copy = mtk_16x32_tile_copy,
        .size_fixup = mtk_size_fixup
    };

    if (0x7f000001 != csp) {
        logmsg("unknown mtk color space %d\n", csp);
        return NULL;
    }

    if (ops_ptr) {
        ops_ptr[0] = &mtk_ops;
    }
    return video_format_get(codec_pixel, csp_nv12, width, height);
}
#endif
