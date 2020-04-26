
#ifndef codec_hook_h
#define codec_hook_h

#include "my_buffer.h"
#include "fmt.h"

typedef struct {
    fourcc** left;
    fourcc** right;
    struct my_buffer* mbuf;
} codectx_t;

#endif
