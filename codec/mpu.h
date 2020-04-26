
#ifndef mpu_h
#define mpu_h

#include "mydef.h"
#include "fmt_in.h"
#include "list_head.h"
#include "my_buffer.h"

#define mpu_encoder 1
#define mpu_decoder 2
#define mpu_convert 4

typedef struct {
    void* (*open) (fourcc**, fourcc**);
    int32_t (*write) (void*, struct my_buffer*, struct list_head*, int32_t*);
    void (*close) (void*);
} mpu_operation;

typedef struct {
    fourcc** infmt;
    fourcc** outfmt;
    mpu_operation* ops;
    uint32_t point;
    uint32_t role;
    const char* name;
} media_process_unit;

typedef struct tag_mpu_item {
    struct list_head entry;
    void* handle;
    media_process_unit* unit;
    fourcc** infmt;
    fourcc** outfmt;
} mpu_item;

__attribute__((unused)) static fourcc** dynamic_output(fourcc** left, fourcc** right)
{
    if (audio_type == media_type(*left)) {
        return right;
    }

    video_format* fmt_left = to_video_format(left);
    video_format* fmt_right = to_video_format(right);
    if (fmt_right->pixel->size == &size_0x0) {
        fmt_right = video_format_get(fmt_right->cc->code, fmt_right->pixel->csp, fmt_left->pixel->size->width, fmt_left->pixel->size->height);
        right = &fmt_right->cc;
    }
    return right;
}

capi mpu_item* alloc_mpu_link(fourcc** in, fourcc** out, intptr_t need_same);
capi void free_mpu_link(mpu_item* link);

#endif
