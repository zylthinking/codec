
#include "codec.h"
#include "mpu.h"
#include "mem.h"
#include "codec_hook.h"
#include "my_errno.h"
#include <string.h>

typedef struct {
    struct list_head head;
    callback_t callback;
} codec_struct;

static void __codec_close(codec_struct* codec)
{
    struct list_head* ent;
    mpu_item* link = NULL;

    for (ent = codec->head.next; ent != &codec->head; ent = ent->next) {
        link = list_entry(ent, mpu_item, entry);
        if (link->handle == NULL) {
            break;
        }
        link->unit->ops->close(link->handle);
    }

    link = list_entry(codec->head.next, mpu_item, entry);
    free_mpu_link(link);

    if (codec->callback.uptr_put != NULL) {
        codec->callback.uptr_put(codec->callback.uptr);
    }
    my_free(codec);
}

static void trace_link_path(struct list_head* headp)
{
#if 0
    char path[4096] = {0};
    struct list_head* ent;
    for (ent = headp->next; ent != headp; ent = ent->next) {
        mpu_item* link = list_entry(ent, mpu_item, entry);
        media_process_unit* unit = link->unit;
        fourcc** infmt = unit->infmt;
        fourcc** outfmt = dynamic_output(infmt, unit->outfmt);

        char name[256];
        sprintf(name, "--->%s(%s:%s)", unit->name, format_name(infmt), format_name(outfmt));
        strcat(path, name);

        infmt = outfmt;
    }
    logmsg("link path: %s\n", &path[4]);
#endif
}

void* codec_open(fourcc** cc_pptr_in, fourcc** cc_pptr_out, callback_t* cb)
{
    codec_struct* codec = (codec_struct *) my_malloc(sizeof(codec_struct));
    if (codec == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    INIT_LIST_HEAD(&codec->head);
    if (cb != NULL) {
        codec->callback = *cb;
    } else {
        memset(&codec->callback, 0, sizeof(callback_t));
    }

    mpu_item* link = alloc_mpu_link(cc_pptr_in, cc_pptr_out, cb != NULL);
    if (link == NULL) {
        my_free(codec);
        return NULL;
    }

    list_add_tail(&codec->head, &link->entry);
    trace_link_path(&codec->head);
    fourcc** infmt = cc_pptr_in;

    struct list_head* ent;
    for (ent = codec->head.next; ent != &codec->head; ent = ent->next) {
        link = list_entry(ent, mpu_item, entry);
        fourcc** outfmt = dynamic_output(infmt, link->unit->outfmt);
        link->handle = link->unit->ops->open(infmt, outfmt);
        if (link->handle == NULL) {
            codec->callback.uptr_put = NULL;
            __codec_close(codec);
            codec = NULL;
            break;
        }

        link->infmt = infmt;
        link->outfmt = outfmt;
        infmt = outfmt;
    }
    return codec;
}

static int32_t link_write(mpu_item* link, struct list_head* in, struct list_head* out,
                          int flush, callback_t* cb, int32_t* delay)
{
    int32_t bytes = 0, nr = 0;
    int32_t empty = list_empty(in);
    notify_t notify = cb->notify;

    while (!empty || flush) {
        struct my_buffer* mbuf = NULL;
        if (!empty) {
            struct list_head* ent = in->next;
            list_del(ent);
            mbuf = list_entry(ent, struct my_buffer, head);
        }

        if (notify != NULL) {
            codectx_t ctx;
            ctx.left = link->infmt;
            ctx.right = link->outfmt;
            ctx.mbuf = mbuf;
            intptr_t n = notify(cb->uptr, 0, &ctx);
            if (n != -1) {
                mbuf = ctx.mbuf;
            }

            if (empty) {
                notify = NULL;
            } else if (mbuf == NULL) {
                empty = list_empty(in);
                continue;
            }
        }

        int n = link->unit->ops->write(link->handle, mbuf, out, &nr);
        if (__builtin_expect(n >= 0, 1)) {
            bytes += n;
        }

        if (__builtin_expect(mbuf == NULL, 0)) {
            break;
        }

        if (n == -1) {
            mbuf->mop->free(mbuf);
        }
        empty = list_empty(in);
    }

    if (nr != 0) {
        my_assert(nr > 0);
        *delay += nr;
    }
    return bytes;
}

int32_t codec_write(void* handle, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay)
{
    int32_t n = 0, bytes = 0;
    codec_struct* codec = (codec_struct *) handle;
    LIST_HEAD(head0);
    LIST_HEAD(head1);

    int32_t flush = (mbuf == NULL);
    if (!flush) {
        list_add(&mbuf->head, &head0);
    }

    struct list_head* ent;
    for (ent = codec->head.next; ent != &codec->head; ent = ent->next) {
        mpu_item* link = list_entry(ent, mpu_item, entry);
        bytes = link_write(link, &head0, &head1, flush, &codec->callback, &n);
        list_add(&head0, &head1);
        list_del_init(&head1);
    }

    if (delay != NULL) {
        *delay = n;
    }

    list_join(headp, &head0);
    list_del(&head0);
    return bytes;
}

void codec_close(void* handle)
{
    codec_struct* codec = (codec_struct *) handle;
    __codec_close(codec);
}
