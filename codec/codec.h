
#ifndef codec_h
#define codec_h

#include "mydef.h"
#include "my_buffer.h"
#include "fmt.h"

capi void* codec_open(fourcc** fmt_in, fourcc** fmt_out, callback_t* cb);
capi int32_t codec_write(void* handle, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay);
capi void codec_close(void* handle);

#endif
