

#ifndef nalu_h
#define nalu_h

#include "mydef.h"

capi int32_t num_ref_frames(const uint8_t* nalu, int32_t bytes);
capi int32_t none_dir_frametype(char* nalu, intptr_t bytes);

#endif
