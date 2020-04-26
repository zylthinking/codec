
#ifndef vendor_h
#define vendor_h
#include "mydef.h"
#include "media_buffer.h"

typedef struct {
    intptr_t width;
    intptr_t height;
    intptr_t bytes;
} panel_t;

typedef struct {
    void (*panel_copy) (media_buffer* meida, char* src, intptr_t bytes, panel_t dst_size[3], panel_t src_size[3]);
    void (*size_fixup) (int32_t w1, int32_t h1, int32_t w0, int32_t h0, panel_t pane[3]);
} vendor_ops_t;

static __attribute__((unused)) void generic_panel_copy(media_buffer* meida, char* pixel,
                                        intptr_t bytes, panel_t dst_size[3], panel_t src_size[3])
{
    if (0 == memcmp(dst_size, src_size, sizeof(dst_size) * 3)) {
        memcpy(meida->vp[0].ptr, pixel, dst_size[0].bytes + dst_size[1].bytes + dst_size[2].bytes);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        if (src_size[i].width == 0) {
            break;
        }

        uintptr_t lines = my_min(dst_size[i].height, src_size[i].height);
        if (dst_size[i].width == src_size[i].width) {
            memcpy(meida->vp[i].ptr, pixel, dst_size[i].width * lines);
        } else {
            uintptr_t bytes = my_min(dst_size[i].width, src_size[i].width);
            char* dst = meida->vp[i].ptr;
            char* src = pixel;
            for (int j = 0; j < lines; ++j) {
                memcpy(dst, src, bytes);
                dst += dst_size[i].width;
                src += src_size[i].width;
            }
        }
        pixel += src_size[i].bytes;
    }
}

#endif
