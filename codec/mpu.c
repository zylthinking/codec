
#include "mpu.h"
#include "mem.h"
#include "my_errno.h"
#include "resampler.h"
#include "media_buffer.h"

#define same_ptr (void *) (1)
static mpu_item* same_link(fourcc** ccptr);

extern media_process_unit speex_dec_8k16b1_mode8;
extern media_process_unit silk_dec_8k16b1;
extern media_process_unit silk_dec_16k16b1;
extern media_process_unit silk_enc_16k16b1;
extern media_process_unit silk_enc_8k16b1;

extern media_process_unit aac_dec_8k16b1;
extern media_process_unit aac_dec_8k16b2;
extern media_process_unit aac_dec_16k16b1;
extern media_process_unit aac_dec_16k16b2;
extern media_process_unit aac_dec_22k16b1;
extern media_process_unit aac_dec_22k16b2;
extern media_process_unit aac_dec_24k16b1;
extern media_process_unit aac_dec_24k16b2;
extern media_process_unit aac_dec_32k16b1;
extern media_process_unit aac_dec_32k16b2;
extern media_process_unit aac_dec_44k16b1;
extern media_process_unit aac_dec_44k16b2;
extern media_process_unit aac_dec_48k16b1;
extern media_process_unit aac_dec_48k16b2;

extern media_process_unit aac_enc_8k16b1;
extern media_process_unit aac_enc_8k16b2;
extern media_process_unit aac_enc_16k16b1;
extern media_process_unit aac_enc_16k16b2;

extern media_process_unit aacplus_dec_8k16b1;
extern media_process_unit aacplus_dec_8k16b2;
extern media_process_unit aacplus_dec_11k16b1;
extern media_process_unit aacplus_dec_11k16b2;
extern media_process_unit aacplus_dec_16k16b1;
extern media_process_unit aacplus_dec_16k16b2;
extern media_process_unit aacplus_dec_22k16b1;
extern media_process_unit aacplus_dec_22k16b2;
extern media_process_unit aacplus_dec_24k16b1;
extern media_process_unit aacplus_dec_24k16b2;

extern media_process_unit opus_dec_8k16b1;
extern media_process_unit opus_dec_16k16b1;
extern media_process_unit opus_dec_24k16b1;
extern media_process_unit opus_dec_24k16b2;
extern media_process_unit opus_dec_32k16b1;
extern media_process_unit opus_dec_32k16b2;
extern media_process_unit opus_dec_48k16b1;
extern media_process_unit opus_dec_48k16b2;

extern media_process_unit x264_h264_enc;
extern media_process_unit ffmpeg_h264_dec;

#if defined(__ANDROID__)
void probe_omx_h264_dec();
extern media_process_unit omx_h264_dec;

void probe_java_h264_enc();
extern media_process_unit java_h264_enc;

#elif defined(__APPLE__)
void probe_apple_h264_dec();
extern media_process_unit apple_h264_dec;

void probe_apple_h264_enc();
media_process_unit apple_h264_enc;
#endif

static media_process_unit* mpu_table[] = {
    &ffmpeg_h264_dec,
#if defined(__ANDROID__)
    &omx_h264_dec,
    &java_h264_enc,
#elif defined(__APPLE__)
    &apple_h264_dec,
    &apple_h264_enc,
#endif
    &x264_h264_enc,

    &silk_dec_16k16b1,
    &silk_dec_8k16b1,
    &silk_enc_16k16b1,
    &silk_enc_8k16b1,

    &aac_dec_8k16b1,
    &aac_dec_8k16b2,
    &aac_dec_16k16b1,
    &aac_dec_16k16b2,
    &aac_dec_22k16b1,
    &aac_dec_22k16b2,
    &aac_dec_24k16b1,
    &aac_dec_24k16b2,
    &aac_dec_32k16b1,
    &aac_dec_32k16b2,
    &aac_dec_44k16b1,
    &aac_dec_44k16b2,
    &aac_dec_48k16b1,
    &aac_dec_48k16b2,

    &aac_enc_8k16b1,
    &aac_enc_8k16b2,
    &aac_enc_16k16b1,
    &aac_enc_16k16b2,

    &aacplus_dec_8k16b1,
    &aacplus_dec_8k16b2,
    &aacplus_dec_11k16b1,
    &aacplus_dec_11k16b2,
    &aacplus_dec_16k16b1,
    &aacplus_dec_16k16b2,
    &aacplus_dec_22k16b1,
    &aacplus_dec_22k16b2,
    &aacplus_dec_24k16b1,
    &aacplus_dec_24k16b2,

    &opus_dec_8k16b1,
    &opus_dec_16k16b1,
    &opus_dec_24k16b1,
    &opus_dec_24k16b2,
    &opus_dec_32k16b1,
    &opus_dec_32k16b2,
    &opus_dec_48k16b1,
    &opus_dec_48k16b2,

    &speex_dec_8k16b1_mode8,
    all_resampler
};

struct dijstra_node {
    struct list_head entry;
    media_process_unit* unit;
    struct dijstra_node* prev;
    fourcc** fmt;
    uint32_t point;
};

static struct dijstra_node* shortest(struct dijstra_node* from, struct list_head* head, fourcc** left)
{
    struct dijstra_node* selected = NULL;
    for (struct list_head* ent = head->next; ent != head; ent = ent->next) {
        struct dijstra_node* item = list_entry(ent, struct dijstra_node, entry);

        if (media_same(left, item->unit->infmt)) {
            if (from == NULL) {
                item->point = item->unit->point;
                item->fmt = dynamic_output(left, item->unit->outfmt);
            } else {
                uint32_t point = from->point + item->unit->point;
                if (point < item->point) {
                    item->fmt = dynamic_output(left, item->unit->outfmt);
                    item->point = point;
                    item->prev = from;
                }
            }
        }

        if (selected == NULL || selected->point > item->point) {
            selected = item;
        }
    }

    if (selected != NULL) {
        list_del(&selected->entry);
    }
    return selected;
}

static void* same_open(fourcc** in, fourcc** out)
{
    my_assert(in == out);
    return same_ptr;
}

static int32_t same_write(void* any, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay)
{
    (void) delay;
    my_assert(any == same_ptr);
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }
    list_add_tail(&mbuf->head, headp);

    int32_t n = 1;
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    if (audio_type == media_type(*media->pptr_cc)) {
        n = (int32_t) media->vp[0].stride;
    }
    return n;
}

static void same_close(void* any)
{
    my_assert(any == same_ptr);
}

static void same_unit_init(media_process_unit* unit, fourcc** ccptr)
{
    static mpu_operation same_ops = {
        same_open,
        same_write,
        same_close
    };
    unit->infmt = unit->outfmt = ccptr;
    unit->ops = &same_ops;
    unit->name = "same";
}

static mpu_item* collect_node(struct dijstra_node* last, intptr_t need_same)
{
    LIST_HEAD(head);

    int nr = 0;
    struct dijstra_node* cur = last;
    while (cur != NULL) {
        ++nr;
        cur = cur->prev;
    }

    if (need_same != 0) {
        need_same = sizeof(mpu_item) + sizeof(media_process_unit);
    }

    mpu_item* link = (mpu_item *) my_malloc(sizeof(mpu_item) * nr + need_same);
    if (link == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    if (need_same != 0) {
        media_process_unit* unit = (media_process_unit *) (link + nr + 1);
        same_unit_init(unit, last->unit->outfmt);
        link[nr].unit = unit;
        link[nr].handle = NULL;
        list_add(&link[nr].entry, &head);
    }

    for (int i = nr - 1; i >= 0; --i) {
        list_add(&link[i].entry, &head);
        link[i].unit = last->unit;
        link[i].handle = NULL;
        link[i].infmt = link[i].unit->infmt;
        link[i].outfmt = link[i].unit->outfmt;
        last = last->prev;
    }

    list_del(&head);
    return link;
}

mpu_item* alloc_mpu_link(fourcc** in, fourcc** out, intptr_t need_same)
{
    if (media_same(in, out)) {
        my_assert(need_same != 0);
        return same_link(in);
    }

    uint32_t exclude = 0;
    if (media_type_raw(in)) {
        exclude |= mpu_decoder;
    }

    if (media_type_raw(out)) {
        exclude |= mpu_encoder;
    }

    LIST_HEAD(head);
    fourcc** cur = in;

    struct dijstra_node nodes[elements(mpu_table)];
    for (int i = 0; i < elements(nodes); ++i) {
        INIT_LIST_HEAD(&nodes[i].entry);
        nodes[i].unit = mpu_table[i];
        if ((nodes[i].unit->role & exclude) ||
            (nodes[i].unit->infmt[0]->type != in[0]->type))
        {
            continue;
        }

#if defined(__ANDROID__)
        if (nodes[i].unit == &omx_h264_dec) {
            probe_omx_h264_dec();
        } else if (nodes[i].unit == &java_h264_enc) {
            probe_java_h264_enc();
        }
#elif defined(__APPLE__)
        if (nodes[i].unit == &apple_h264_dec) {
            probe_apple_h264_dec();
        } else if (nodes[i].unit == &apple_h264_enc) {
            probe_apple_h264_enc();
        }
#endif
        if (nodes[i].unit->ops == NULL) {
            continue;
        }
        nodes[i].prev = NULL;
        nodes[i].fmt = nodes[i].unit->outfmt;
        nodes[i].point = ((uint32_t) -1);
        list_add_tail(&nodes[i].entry, &head);
    }
    struct dijstra_node* pitem = NULL;

    do {
        pitem = shortest(pitem, &head, cur);
        if (pitem == NULL) {
            break;
        }

        if (media_same(pitem->fmt, out)) {
            return collect_node(pitem, need_same);
        }
        cur = pitem->fmt;
    } while (!list_empty(&head) && pitem->point != (uint32_t) (-1));

    errno = ESRCH;
    return NULL;
}

void free_mpu_link(mpu_item* link)
{
    my_free(link);
}

static mpu_item* same_link(fourcc** ccptr)
{
    mpu_item* link = (mpu_item *) my_malloc(sizeof(mpu_item) + sizeof(media_process_unit));
    if (link == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    media_process_unit* unit = (media_process_unit *) (link + 1);
    same_unit_init(unit, ccptr);
    link->unit = unit;
    link->handle = NULL;
    INIT_LIST_HEAD(&link->entry);
    return link;
}
