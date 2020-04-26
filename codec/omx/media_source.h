
#ifndef media_source_h
#define media_source_h
#if defined(__ANDROID__)

#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
using namespace android;

#include "my_buffer.h"
#include "lock.h"
#include "my_handle.h"

class media_source : public MediaSource
{
    lock_t lck;
    my_handle* handle;
public:
    media_source(sp<MetaData> meta);
    ~media_source();
    sp<MetaData> getFormat();
    status_t start(MetaData *params);
    status_t stop();
    status_t read(MediaBuffer** buffer, const MediaSource::ReadOptions *options);

    intptr_t add_buffer(struct my_buffer* mbuf);
    intptr_t initialize(intptr_t size);
private:
    MediaBufferGroup group;
    sp<MetaData> meta;
    intptr_t annexb;
};

#endif
#endif
