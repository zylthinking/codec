
#ifndef silk_h
#define silk_h

#include "mydef.h"
#include "fmt.h"
#include "my_buffer.h"

capi struct my_buffer* silk_muted_frame_get(audio_format* fmt, uint32_t ms);
capi uint32_t silk_frame_muted(struct my_buffer* mbuf);

#define silk_mpf 20
#define silk_frame_leading sizeof(uint16_t)
#define silk_write_frame_bytes(sp, nb) \
do { \
    (sp)[0] = nb; \
} while (0)
#define silk_read_frame_bytes(sp) *((uint16_t *) sp)

#endif
