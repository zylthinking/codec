
#ifndef mp4_h
#define mp4_h

#include "mydef.h"

typedef struct {
    uint32_t* nbptr1;
    uint32_t* nbptr2;
    uint32_t* nbptr3;
    int32_t zero1, zero2, len;
} annexb_to_mp4_t;

capi void annexb_to_mp4_begin(annexb_to_mp4_t* mp4, char* payload, intptr_t bytes);
capi void annexb_to_mp4(annexb_to_mp4_t* mp4, char* payload, intptr_t bytes);
capi void annexb_to_mp4_end(annexb_to_mp4_t* mp4);

#endif
