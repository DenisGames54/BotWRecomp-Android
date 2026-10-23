#pragma once
extern "C" char __executable_start;
static inline const void* _dyld_get_image_header(int) { return &__executable_start; }
