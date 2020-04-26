
#if defined(__ANDROID__)
#include "media_source.h"
#include "media_buffer.h"
#include "mbuf.h"
#include <sys/syscall.h>
#define futex(p1, p2, p3, p4, p5, p6) syscall(__NR_futex, p1, p2, p3, p4, p5, p6)

typedef struct {
    lock_t lck;
    struct list_head head;
    int nr[2];
    uint64_t seq;
    intptr_t hbuf;
    struct my_buffer* mbuf;
} buffer_pool_t;

media_source::media_source(sp<MetaData> meta)
{
    handle = NULL;
    lck = lock_val;
    this->meta = meta;
}

media_source::~media_source()
{
    if (handle != NULL) {
        handle_dettach(handle);
    }
}

sp<MetaData> media_source::getFormat() {
    return meta;
}

static void source_free(void* addr)
{
    buffer_pool_t* pool = (buffer_pool_t *) addr;
    free_buffer(&pool->head);
    if (pool->mbuf != NULL) {
        pool->mbuf->mop->free(pool->mbuf);
    }
    mbuf_reap(pool->hbuf);
    futex(&pool->nr[0], FUTEX_WAKE, 64, NULL, NULL, 0);
    my_free(addr);
}

status_t media_source::start(MetaData* params) {
    annexb = 1;
    if (params != NULL) {
        int32_t val;
        if (params->findInt32(kKeyWantsNALFragments, &val)) {
            annexb = (val == 0);
        }
    }
    return OK;
}

status_t media_source::stop()
{
    if (handle != NULL) {
        handle_clone(handle);
        handle_dettach(handle);
    }
    return OK;
}

static intptr_t nalu_bytes(char* pch, intptr_t bytes)
{
    my_assert(bytes >= 4);
    my_assert(pch[0] == 0);
    my_assert(pch[1] == 0);
    if (pch[2] == 1) {
        return 3;
    }

    my_assert(pch[2] == 0);
    my_assert(pch[3] == 1);
    return 4;
}

static status_t read_from_pool(MediaBuffer* buffer, buffer_pool_t* pool, intptr_t annexb)
{
    struct list_head* ent;
    lock(&pool->lck);
    --pool->nr[0];
    ent = pool->head.next;
    list_del(ent);
    unlock(&pool->lck);

    struct my_buffer* mbuf = list_entry(ent, struct my_buffer, head);
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    if (annexb == 0) {
        intptr_t bytes = nalu_bytes(media->vp[0].ptr, media->vp[0].stride);
        media->vp[0].ptr += bytes;
        media->vp[0].stride -= bytes;
    }

    memcpy(buffer->data(), media->vp[0].ptr, media->vp[0].stride);
    buffer->set_range(0, media->vp[0].stride);

    buffer->meta_data()->clear();
    buffer->meta_data()->setInt64(kKeyTime, media->pts * 1000 + media->angle);
    mbuf->mop->free(mbuf);
    return OK;
}

status_t media_source::read(MediaBuffer** buffer, const MediaSource::ReadOptions* options)
{
    if (handle == NULL) {
        return ERROR_NOT_CONNECTED;
    }

    buffer_pool_t* pool = (buffer_pool_t *) handle_get(handle);
    while (pool != NULL && pool->nr[0] == 0) {
        handle_put(handle);
        futex(&pool->nr[0], FUTEX_WAIT, 0, NULL, NULL, 0);
        pool = (buffer_pool_t *) handle_get(handle);
    }

    if (pool == NULL) {
        return ERROR_END_OF_STREAM;
    }

    status_t stat = group.acquire_buffer(buffer);
    my_assert(stat == OK);

    stat = read_from_pool(buffer[0], pool, annexb);
    handle_put(handle);
    return stat;
}

intptr_t media_source::add_buffer(struct my_buffer* mbuf)
{
    my_assert2(mbuf != NULL && this != NULL, "mbuf = %p, this = %p\n", mbuf, this);
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    buffer_pool_t* pool = (buffer_pool_t *) handle_get(handle);
    if (pool == NULL) {
        return -1;
    }

    if (media->fragment[1] > 1) {
        media_buffer* media2 = NULL;
        if (pool->mbuf == NULL) {
            my_assert(media->fragment[0] == 0);

            pool->mbuf = mbuf_alloc_1(pool->hbuf);
            // mbuf pool of 8 entries should be enough
            my_assert(pool->mbuf != NULL);
            pool->mbuf->ptr[1] = pool->mbuf->ptr[0] + sizeof(media_buffer);
            pool->mbuf->length -= sizeof(media_buffer);
            media2 = (media_buffer *) pool->mbuf->ptr[0];

            memset(media2->vp, 0, sizeof(media->vp));
            media2->frametype = media->frametype;
            media2->angle = media->angle;
            media2->fragment[0] = 0;
            media2->fragment[1] = media->fragment[1];
            media2->pptr_cc = media->pptr_cc;
            media2->pts = media->pts;
            media2->iden = media->iden;
            media2->seq = pool->seq++;
            media2->vp[0].ptr = pool->mbuf->ptr[1];
            media2->vp[0].stride = 0;
        } else {
            media2 = (media_buffer *) pool->mbuf->ptr[0];
            my_assert2(media->fragment[0] > 0, "seq = %llu", media->seq);
            my_assert(media->fragment[1] == media2->fragment[1]);
            my_assert(media2->pts == media->pts);
        }

        memcpy(media2->vp[0].ptr + media2->vp[0].stride, media->vp[0].ptr, media->vp[0].stride);
        media2->vp[0].stride += media->vp[0].stride;

        if (media->fragment[0] != media->fragment[1] - 1) {
            mbuf->mop->free(mbuf);
            handle_put(handle);
            return pool->nr[1];
        }
        mbuf->mop->free(mbuf);
        media2->fragment[1] = 1;
        mbuf = pool->mbuf;
        pool->mbuf = NULL;
    }

    lock(&pool->lck);
    list_add_tail(&mbuf->head, &pool->head);
    intptr_t n = ++pool->nr[0];
    unlock(&pool->lck);
    ++pool->nr[1];

    if (n == 1) {
        futex(&pool->nr[0], FUTEX_WAKE, 64, NULL, NULL, 0);
    }
    handle_put(handle);
    return pool->nr[1];
}

static my_handle* do_init(MediaBufferGroup* gp, intptr_t size)
{
    intptr_t hbuf = mbuf_hget(sizeof(media_buffer) + size, 8, 0);
    if (hbuf == -1) {
        return NULL;
    }

    MediaBuffer* buffer1 = new (std::nothrow) MediaBuffer(size);
    MediaBuffer* buffer2 = new (std::nothrow) MediaBuffer(size);
    MediaBuffer* buffer3 = new (std::nothrow) MediaBuffer(size);
    MediaBuffer* buffer4 = new (std::nothrow) MediaBuffer(size);

    if (buffer1 == NULL || buffer2 == NULL || buffer3 == NULL || buffer4 == NULL) {
        if (buffer1) buffer1->release();
        if (buffer2) buffer2->release();
        if (buffer3) buffer3->release();
        if (buffer4) buffer4->release();
        mbuf_reap(hbuf);
        return NULL;
    }
    gp->add_buffer(buffer1);
    gp->add_buffer(buffer2);
    gp->add_buffer(buffer3);
    gp->add_buffer(buffer4);

    buffer_pool_t* pool = (buffer_pool_t *) my_malloc(sizeof(buffer_pool_t));
    if (pool == NULL) {
        mbuf_reap(hbuf);
        return NULL;
    }
    pool->nr[0] = pool->nr[1] = 0;
    pool->lck = lock_val;
    pool->mbuf = NULL;
    pool->seq = 0;
    pool->hbuf = hbuf;
    INIT_LIST_HEAD(&pool->head);

    my_handle* handle = handle_attach(pool, source_free);
    if (handle == NULL) {
        mbuf_reap(hbuf);
        my_free(pool);
        return NULL;
    }
    return handle;
}

intptr_t media_source::initialize(intptr_t size)
{
    lock(&lck);
    if (handle == NULL) {
        handle = do_init(&group, size);
    }
    unlock(&lck);

    if (handle == NULL) {
        return -1;
    }
    return 0;
}

#endif
