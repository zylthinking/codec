
#include "silk.h"
#include "mbuf.h"
#include "media_buffer.h"
#include <string.h>

struct my_buffer* silk_muted_frame_get(audio_format* fmt, uint32_t ms)
{
    struct silk_param* param = *(struct silk_param **) fmt->pparam;
    uint32_t frames = ms / param->frame_ms;
    if (frames == 0) {
        return NULL;
    }

    struct my_buffer* mbuf = mbuf_alloc_2(sizeof(media_buffer) + frames * silk_frame_leading);
    if (mbuf != NULL) {
        mbuf->ptr[1] = mbuf->ptr[0] + sizeof(media_buffer);
        mbuf->length -= sizeof(media_buffer);

        media_buffer* media = (media_buffer *) mbuf->ptr[0];
        memset(media->vp, 0, sizeof(media->vp));
        media->frametype = 0;
        media->angle = 0;
        media->fragment[0] = 0;
        media->fragment[1] = 1;
        media->iden = media_id_unkown;
        media->pptr_cc = &fmt->cc;
        media->pts = 0;
        media->seq = 0;
        media->vp[0].ptr = mbuf->ptr[1];
        media->vp[0].stride = (uint32_t) mbuf->length;
        uint16_t* sp = (uint16_t *) mbuf->ptr[1];
        for (int i = 0; i < frames; ++i) {
            silk_write_frame_bytes(&sp[i], 0);
        }
    }
    return mbuf;
}

uint32_t silk_frame_muted(struct my_buffer* mbuf)
{
    uintptr_t bytes = mbuf->length;
    char* pch = mbuf->ptr[1];

    while (bytes > 0) {
        my_assert(bytes >= silk_frame_leading);
        uint32_t nb = silk_read_frame_bytes(pch);
        if (nb != 0) {
            return 0;
        }
        pch += (silk_frame_leading + nb);
        bytes -= (silk_frame_leading + nb);
    }

    return 1;
}
