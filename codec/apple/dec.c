
#if defined(__APPLE__)
#include <VideoToolbox/VideoToolbox.h>
#include <arpa/inet.h>
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "fmt.h"
#include "ios.h"
#include "lock.h"
#include "my_handle.h"
#include "enter.h"
#include "mp4.h"

typedef struct {
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef videofmt;
    CMBlockBufferRef memblock;
    annexb_to_mp4_t annexb_mp4;

    int32_t frames;
    struct list_head input;
    uint64_t sn;
    uint64_t pts;
    int32_t maxrefs;
    int32_t hold;

    struct my_buffer* sps;
    video_format* fmt;
    fourcc** in;
    media_iden_t id;
    uintptr_t nr, seq, bytes;
} apple_wrapper_t;

#define ignore_none_idr ((struct my_buffer *) 1)
entry_point h264_decoder_point = enter_init(0);
extern int32_t num_ref_frames(const uint8_t* nalu, int32_t bytes);

static void* apple_open(fourcc** in, fourcc** out)
{
    errno = ENOMEM;
    apple_wrapper_t* wrapper = heap_alloc(wrapper);
    if (wrapper == NULL) {
        return NULL;
    }

    wrapper->sn = 0;
    wrapper->frames = 0;
    wrapper->hold = 0;
    wrapper->maxrefs = 0;
    wrapper->pts = 0;
    wrapper->session = NULL;
    wrapper->videofmt = NULL;
    wrapper->in = in;
    wrapper->fmt = to_video_format(out);
    wrapper->sps = NULL;
    wrapper->id = media_id_unkown;
    wrapper->nr = 0;
    wrapper->seq = 0;
    wrapper->bytes = 0;
    wrapper->memblock = NULL;
    INIT_LIST_HEAD(&wrapper->input);
    return wrapper;
}

static intptr_t param_get(char* pch, intptr_t bytes, uint8_t* param[2], size_t size[2])
{
    char* base = pch;
    char code[3] = {0, 0, 1};
    while (bytes > 0 && (param[0] == NULL || param[1] == NULL)) {
        my_assert(pch[0] == 0 && pch[1] == 0);
        if (pch[2] == 0) {
            my_assert(pch[3] == 1);
            pch += 4;
            bytes -= 4;
        } else {
            my_assert(pch[2] == 1);
            pch += 3;
            bytes -= 3;
        }

        char* end = (char *) memmem(pch, bytes, code, 3);
        if (end == NULL) {
            end = pch + bytes;
        } else if (end[-1] == 0) {
            end -= 1;
        }

        intptr_t type = pch[0] & 0x1f;
        intptr_t length = (intptr_t) (end - pch);
        my_assert(bytes >= length);

        if (type == 7) {
            param[0] = (uint8_t *) pch;
            size[0] = length;
        } else if (type == 8) {
            param[1] = (uint8_t *) pch;
            size[1] = length;
        }
        pch = end;
        bytes -= length;
    }
    my_assert(bytes >= 0);
    return (intptr_t) (pch - base);
}

static void push_pixel(void* ctx, void* uptr, OSStatus status, VTDecodeInfoFlags flag,
                       CVImageBufferRef image, CMTime timestamp, CMTime duration)
{
    if (__builtin_expect(status == noErr, 1)) {
        my_assert(image != NULL);
        my_assert2(*((void **) uptr) == NULL, "called multi times while one decode");
        CVPixelBufferRef* pixelref = (CVPixelBufferRef *) uptr;
        *pixelref = CVPixelBufferRetain(image);
    } else {
        logmsg("decoder failed with err %d\n", status);
        my_assert(image == NULL);
    }
}

static VTDecompressionSessionRef create_session(CMFormatDescriptionRef ref)
{
    uint32_t v = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef numref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    const void* values[] = {numref};
    const void* keys[] = {kCVPixelBufferPixelFormatTypeKey};

    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 1, NULL, NULL);
    if (attrs == NULL) {
        return NULL;
    }

    VTDecompressionOutputCallbackRecord callBackRecord;
    callBackRecord.decompressionOutputCallback = push_pixel;
    callBackRecord.decompressionOutputRefCon = NULL;

    VTDecompressionSessionRef session = NULL;
    OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault,
                                          ref, NULL, attrs, &callBackRecord, &session);
    CFRelease(attrs);
    CFRelease(numref);
    if (status != noErr) {
        return NULL;
    }
    return session;
}

static intptr_t decoder_init(apple_wrapper_t* wrapper, struct my_buffer* mbuf)
{
    uint8_t* param[2] = {NULL, NULL};
    size_t size[2];

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    intptr_t n = param_get(media->vp[0].ptr, media->vp[0].stride, (uint8_t **) param, size);
    my_assert(param[0] != NULL);
    my_assert(param[1] != NULL);

    struct my_buffer* header = mbuf_alloc_2(sizeof(media_buffer) + n);
    if (header == NULL) {
        return -1;
    }
    header->ptr[1] += sizeof(media_buffer);
    header->length -= sizeof(media_buffer);;
    media_buffer* media2 = (media_buffer *) header->ptr[0];
    *media2 = *media;
    media2->vp[0].ptr = header->ptr[1];
    media2->vp[0].stride = (uint32_t) header->length;
    memcpy(media2->vp[0].ptr, media->vp[0].ptr, media2->vp[0].stride);
    stream_begin(header);
    header->mop->free(header);

    CMFormatDescriptionRef ref = NULL;
    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                        kCFAllocatorDefault, 2, (const uint8_t**) param, size, 4, &ref);
    if (status != noErr) {
        return -1;
    }

    VTDecompressionSessionRef session = create_session(ref);
    if (NULL == session) {
        CFRelease(ref);
        return -1;
    }

    media->vp[0].stride -= n;
    media->vp[0].ptr += n;
    wrapper->session = session;
    wrapper->videofmt = ref;
    return 0;
}

static void block_free(void* uptr, void* addr, size_t bytes)
{
    struct my_buffer* mbuf = (struct my_buffer *) uptr;
    mbuf->mop->free(mbuf);
}

static intptr_t memblock_get(apple_wrapper_t* wrapper, struct my_buffer* mbuf)
{
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    media = (media_buffer *) mbuf->ptr[0];

    CMBlockBufferCustomBlockSource custom_mem_block = {
        .version = kCMBlockBufferCustomBlockSourceVersion,
        .FreeBlock = block_free,
        .refCon = mbuf
    };

    CMBlockBufferRef ref = wrapper->memblock;
    if (media->fragment[0] == 0) {
        my_assert(ref == NULL);
        if (media->vp[0].ptr[2] == 1) {
            // VCL nalu should always has 4 bytes annexb.
            // reference_able is subset of VCL nalu, however
            // it may fails some frame to be decoded.
            my_assert(!reference_able(media->frametype));
            return -1;
        }
        my_assert(media->vp[0].ptr[2] == 0 && media->vp[0].ptr[3] == 1);

        OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                                              media->vp[0].ptr, media->vp[0].stride,
                                                              kCFAllocatorNull,
                                                              &custom_mem_block, 0, media->vp[0].stride,
                                                              0, &ref);
        if (status != noErr) {
            if (reference_able(media->frametype)) {
                wrapper->sps = ignore_none_idr;
            }
            return -1;
        }

        annexb_to_mp4_begin(&wrapper->annexb_mp4, media->vp[0].ptr, media->vp[0].stride);
        wrapper->memblock = ref;
        wrapper->seq = (uintptr_t) media->seq;
        wrapper->nr = 1;
        wrapper->bytes = media->vp[0].stride;
    } else if (ref == NULL) {
        return -1;
    } else if (media->fragment[1] > 1) {
        OSStatus status = CMBlockBufferAppendMemoryBlock(ref,
                                                         media->vp[0].ptr, media->vp[0].stride,
                                                         kCFAllocatorNull,
                                                         &custom_mem_block, 0, media->vp[0].stride, 0);
        if (status != noErr) {
            CFRelease(wrapper->memblock);
            wrapper->memblock = NULL;
            if (reference_able(media->frametype)) {
                wrapper->sps = ignore_none_idr;
            }
            return -1;
        }
        ++wrapper->nr;
        wrapper->bytes += media->vp[0].stride;
        annexb_to_mp4(&wrapper->annexb_mp4, media->vp[0].ptr, media->vp[0].stride);
    }

    my_assert(media->seq - media->fragment[0] == wrapper->seq);
    if (media->fragment[0] == media->fragment[1] - 1) {
        my_assert(wrapper->nr == media->fragment[1]);
        wrapper->nr = 0;
        wrapper->seq = 0;
        annexb_to_mp4_end(&wrapper->annexb_mp4);
        return wrapper->bytes;
    }
    return 0;
}

static struct my_buffer* init_with_stream(apple_wrapper_t* wrapper, struct my_buffer* mbuf)
{
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    if (wrapper->sps == NULL) {
        if (media->frametype != video_type_sps) {
            mbuf->mop->free(mbuf);
            return NULL;
        }
        wrapper->sps = mbuf;
        wrapper->maxrefs = num_ref_frames((const uint8_t *) media->vp[0].ptr, media->vp[0].stride);
        mbuf = NULL;
    }

    if (1 == enter_in(&h264_decoder_point)) {
        if (mbuf != NULL) {
            mbuf->mop->free(mbuf);
        }
        return NULL;
    }

    intptr_t n = decoder_init(wrapper, wrapper->sps);
    enter_out(&h264_decoder_point);
    if (-1 == n) {
        if (mbuf != NULL) {
            mbuf->mop->free(mbuf);
        }
        return NULL;
    }

    wrapper->id = media->iden;
    if (mbuf != NULL || media->vp[0].stride == 0) {
        wrapper->sps->mop->free(wrapper->sps);
    } else {
        mbuf = wrapper->sps;
    }
    wrapper->sps = ignore_none_idr;
    return mbuf;
}

static void frame_push(apple_wrapper_t* wrapper, struct my_buffer* mbuf, uint32_t angle, uint64_t pts, uint32_t frametype)
{
    wrapper->frames++;
    if (reference_able(frametype)) {
        wrapper->hold++;
    }

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    media->frametype = frametype;
    media->angle = angle;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->iden = wrapper->id;
    media->seq = 0;
    media->pptr_cc = &wrapper->fmt->cc;
    media->pts = pts;

    struct list_head* ent;
    for (ent = wrapper->input.prev; ent != &wrapper->input; ent = ent->prev) {
        struct my_buffer* mbuf2 = list_entry(ent, struct my_buffer, head);
        media_buffer* media2 = (media_buffer *) mbuf2->ptr[0];
        if (media2->pts < pts) {
            break;
        }
    }
    list_add(&mbuf->head, ent);
}

static int32_t frame_pop(apple_wrapper_t* wrapper, struct list_head* headp)
{
    int32_t n = 0;
    do {
        struct list_head* ent = wrapper->input.next;
        struct my_buffer* mbuf = list_entry(ent, struct my_buffer, head);
        media_buffer* media = (media_buffer *) mbuf->ptr[0];

        if (reference_able(media->frametype) && wrapper->hold <= wrapper->maxrefs) {
            break;
        }

        my_assert2(wrapper->pts < media->pts,
                   "frametype = %d, pts %llu < prev %llu",
                   media->frametype, media->pts, wrapper->pts);
        --wrapper->frames;
        wrapper->pts = media->pts;
        wrapper->hold -= reference_able(media->frametype);

        media->frametype = 0;
        media->seq = (++wrapper->sn);
        list_del(ent);
        list_add_tail(ent, headp);
        ++n;
    } while (wrapper->hold >= wrapper->maxrefs);

    return n;
}

static int32_t apple_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    apple_wrapper_t* wrapper = handle;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    if (__builtin_expect(wrapper->session == NULL, 0)) {
        if (wrapper->videofmt != NULL) {
            wrapper->session = create_session(wrapper->videofmt);
            if (wrapper->session == NULL) {
                if (mbuf != NULL) {
                    mbuf->mop->free(mbuf);
                }
                return 0;
            }
        }

        mbuf = init_with_stream(wrapper, mbuf);
        if (mbuf == NULL) {
            return 0;
        }
    }

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    if (__builtin_expect(wrapper->sps == ignore_none_idr, 0)) {
        if (reference_others(media->frametype)) {
            mbuf->mop->free(mbuf);
            return 0;
        } else if (media->frametype == video_type_idr) {
            wrapper->sps = NULL;
        }
    }

    intptr_t bytes = memblock_get(wrapper, mbuf);
    if (bytes == -1) {
        mbuf->mop->free(mbuf);
        return 0;
    }

    if (bytes == 0) {
        return 0;
    }

    if (1 == enter_in(&h264_decoder_point)) {
        if (reference_able(media->frametype)) {
            wrapper->sps = ignore_none_idr;
        }
        CFRelease(wrapper->memblock);
        wrapper->memblock = NULL;
        return 0;
    }

    CMSampleBufferRef sampleBuffer = NULL;
    const size_t sampleSizeArray[] = {bytes};
    OSStatus status = CMSampleBufferCreateReady(kCFAllocatorDefault, wrapper->memblock,
                                       wrapper->videofmt, 1, 0, NULL, 1, sampleSizeArray, &sampleBuffer);
    CFRelease(wrapper->memblock);
    wrapper->memblock = NULL;
    if (status != kCMBlockBufferNoErr) {
        if (reference_able(media->frametype)) {
            wrapper->sps = ignore_none_idr;
        }
        enter_out(&h264_decoder_point);
        return 0;
    }

    uint32_t angle = media->angle;
    uint32_t frametype = media->frametype;
    uint64_t pts = media->pts;
    CVPixelBufferRef pixelref = NULL;
    do {
        status = VTDecompressionSessionDecodeFrame(wrapper->session, sampleBuffer, 0, &pixelref, NULL);
        if (status == kVTInvalidSessionErr) {
            VTDecompressionSessionFinishDelayedFrames(wrapper->session);
            VTDecompressionSessionInvalidate(wrapper->session);
            CFRelease(wrapper->session);
            wrapper->session = create_session(wrapper->videofmt);
            if (wrapper->session == NULL) {
                status = kVTAllocationFailedErr;
            }
        }
    } while (status == kVTInvalidSessionErr);
    CFRelease(sampleBuffer);

    int32_t n = 0;
    if (pixelref != NULL) {
        struct my_buffer* mbuf = buffer_attach(pixelref, 1);
        CVPixelBufferRelease(pixelref);

        if (__builtin_expect(mbuf != NULL, 1)) {
            frame_push(wrapper, mbuf, angle, pts, frametype);
            n = frame_pop(wrapper, head);
        }
    }
    *delay = wrapper->frames;
    enter_out(&h264_decoder_point);
    return n;
}

static void apple_close(void* any)
{
    apple_wrapper_t* wrapper = any;
    if (wrapper->session != NULL) {
        OSStatus status = VTDecompressionSessionFinishDelayedFrames(wrapper->session);
        check_status(status, (void) 0);
        VTDecompressionSessionInvalidate(wrapper->session);
        CFRelease(wrapper->session);
        CFRelease(wrapper->videofmt);
    }
    free_buffer(&wrapper->input);

    if (wrapper->sps != NULL && wrapper->sps != ignore_none_idr) {
        wrapper->sps->mop->free(wrapper->sps);
    }
    stream_end(wrapper->id, wrapper->in);
    heap_free(wrapper);
}

static mpu_operation apple_ops = {
    apple_open,
    apple_write,
    apple_close
};

media_process_unit apple_h264_dec = {
    &h264_0x0.cc,
    &raw_0x0_nv12.cc,
    &apple_ops,
    1,
    mpu_decoder,
    "apple_h264_dec"
};

void probe_apple_h264_dec()
{
    double ver = ios_version();
    if (ver < 8.0) {
        apple_h264_dec.ops = NULL;
    } else {
        apple_h264_dec.ops = &apple_ops;
    }
}

#endif
