// GHS C runtime data exported by coreinit: stdio stream table, FOPEN_MAX, CRT hook pointers.
#include "hle.h"
#include "loader.h"

namespace {
constexpr uint32_t kFopenMax = 100;
uint32_t g_iob = 0, g_iob_lock = 0;

struct Defs {
    Defs() {
        loader::define_data_import("coreinit", "_iob", kFopenMax * 16, [](uint32_t a) {
            g_iob = a;
            mem::w32(a + 0 * 16 + 12, 0x1 | (0u << 18));  // stdin
            mem::w32(a + 1 * 16 + 12, 0x2 | (1u << 18));  // stdout
            mem::w32(a + 2 * 16 + 12, 0x2 | (2u << 18));  // stderr
            g_iob_lock = mem::sys_alloc(kFopenMax * 4, 4);
            for (uint32_t i = 0; i < 3; i++) mem::w32(g_iob_lock + 4 * i, i + 1);
        });
        loader::define_data_import("coreinit", "__gh_FOPEN_MAX", 4, [](uint32_t a) { mem::w16(a, kFopenMax); });
        loader::define_data_import("coreinit", "__atexit_cleanup", 4, nullptr);
        loader::define_data_import("coreinit", "__cpp_exception_init_ptr", 4, nullptr);
        loader::define_data_import("coreinit", "__cpp_exception_cleanup_ptr", 4, nullptr);
    }
} g_defs;
}  // namespace

// ghs_flock* __ghs_flock_ptr(iobbuf*): the lock slot belonging to a stream
HLE_EXPORT(coreinit, __ghs_flock_ptr) {
    uint32_t idx = (ARG(0) - g_iob) / 16;
    RET(idx < kFopenMax ? g_iob_lock + 4 * idx : 0);
}
