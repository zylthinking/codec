
#if defined(__ANDROID__)
#include "logmsg.h"
#include "android.h"
#include <errno.h>
#include "mpu.h"
#include "fmt_in.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/system_properties.h>

media_process_unit omx_h264_dec = {
    &h264_0x0.cc,
    &raw_0x0.cc,
    NULL,
    -1,
    mpu_decoder,
    "omx_h264_dec"
};

static const char* so_name(char* release)
{
    char* ver = release;
    char* major = NULL;
    char* minor = NULL;

    char* pch = strchr(ver, '.');
    if (pch != NULL) {
        *pch = 0;
        major = pch + 1;

        pch = strchr(major, '.');
        if (pch != NULL) {
            *pch = 0;
            minor = pch + 1;
        }
    }

    int version_num = atoi(ver);
    int major_num = 0, minor_num = 0;
    if (major != NULL) {
        major_num = atoi(major);
        if (minor != NULL) {
            minor_num = atoi(minor);
        }
    }

    static char path[512] = {'.', '/'};
    const char* p = getenv("andaroid_so_path");
    if (p != NULL) {
        sprintf(path, "%s", p);
    }

    const char* so_name = NULL;
    if (version_num == 4) {
        so_name = "libomx40.so";
        if (major_num > 0) {
            so_name = "libomx41.so";
        }
    } else if (version_num == 5 || version_num == 6) {
        so_name = "libomx50.so";
    }

    if (so_name == NULL) {
        return NULL;
    }

    strcat(path, so_name);
    return path;
}

void probe_omx_h264_dec()
{
    static mpu_operation* mops = NULL;
    if (mops == NULL) {
        if (omx_h264_dec.ops == NULL) {
            return;
        }
        mops = omx_h264_dec.ops;
    }

    int64_t* feature = android_feature_address();
    if (feature && feature[omx_decode] == 0) {
        omx_h264_dec.ops = NULL;
    } else {
        omx_h264_dec.ops = mops;
    }
}

typedef media_process_unit* (*mpu_get_t) ();
__attribute__((constructor)) void omx_decoder_unit()
{
    int64_t* feature = android_feature_address();
    if (feature && feature[omx_decode] == -1) {
        return;
    }

    char prop[PROP_VALUE_MAX];
    prop[0] = 0;
    __system_property_get("ro.build.version.release", prop);
    const char* so = so_name(prop);
    if (so == NULL) {
        return;
    }

    void* ptr = dlopen(so, RTLD_NOW);
    if (ptr == NULL) {
        logmsg("faild open %s, errno = %d\n", so, errno);
        return;
    }

    mpu_get_t mpu_get_decoder = (mpu_get_t) dlsym(ptr, "mpu_get_decoder");
    if (mpu_get_decoder == NULL) {
        dlclose(ptr);
        return;
    }

    media_process_unit* unit = mpu_get_decoder();
    if (unit == NULL) {
        if (0 == (feature[bug_mask] & bug_dlclose)) {
            dlclose(ptr);
        }
        return;
    }
    omx_h264_dec = *unit;
}
#endif
