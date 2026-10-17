// Run-time selection for the native GPU path: read straight from the process arguments (and the
// environment), so runtime/src/main.cpp needs no changes. See gpu_native.h.
#if defined(BOTW_NATIVE_GPU)
#include <crt_externs.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "gpu_native.h"

namespace gpu_native {
namespace {
// value of "--name value" on the command line, else the environment variable, else nullptr
const char* option(const char* name, const char* env) {
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return getenv(env);
}
}  // namespace

bool native_selected() {
    static const bool v = [] {
        const char* s = option("--renderer", "BOTW_RENDERER");
        return s && !strcmp(s, "native");
    }();
    return v;
}

int64_t dump_frame_index() {
    static const int64_t v = [] {
        const char* s = option("--gpu-dump-frame", "BOTW_GPU_DUMP_FRAME");
        return s ? (int64_t)atoll(s) : (int64_t)-1;
    }();
    return v;
}

int stats_interval() {
    static const int v = [] {
        const char* s = option("--gpu-stats", "BOTW_GPU_STATS");
        return s ? atoi(s) : 0;
    }();
    return v;
}
}  // namespace gpu_native
#endif  // BOTW_NATIVE_GPU
