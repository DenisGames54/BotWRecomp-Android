// Debug aid: BOTW_WATCH=<addr> or <ptr>+<off> (hex guest addresses). Write-protects the host page
// holding the watched byte and logs the host frames of every write that hits it (symbolize with
// tools/symbolize.sh: recompiled frames map to guest addresses). For "<ptr>+<off>" the watch is
// armed once the guest pointer at <ptr> becomes non-zero. Other writes to the page are let through
// by unprotecting it; a helper thread re-protects it shortly after.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "hle.h"

namespace {
std::atomic<uintptr_t> g_page{0}, g_target{0};
std::atomic<bool> g_open{false};
size_t g_psize = 0;

void protect(bool ro) { mprotect((void*)g_page.load(), g_psize, ro ? PROT_READ : PROT_READ | PROT_WRITE); }

void watcher(uint32_t ptr, uint32_t off, uint32_t direct) {
    uint32_t guest = direct;
    while (!guest) {
        uint32_t p = mem::r32(ptr);
        if (p) guest = p + off;
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    uintptr_t host = (uintptr_t)g_mem + guest;
    LOG("watch: armed on %08x (value %02x)", guest, mem::r8(guest));
    g_target = host;
    g_page = host & ~(g_psize - 1);
    protect(true);
    uint8_t last = mem::r8(guest);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(5));
        uint8_t v = *(volatile uint8_t*)host;
        if (v != last) {
            LOG("watch: %08x changed %02x -> %02x", guest, last, v);
            if (const char* d = getenv("BOTW_WATCH_DUMP")) {  // "<ptr>:<idx>,<idx>": array of objects
                uint32_t tab = mem::r32((uint32_t)strtoul(d, nullptr, 16));
                LOG("  table %08x count %u arr %08x", tab, mem::r32(tab + 20), mem::r32(tab + 24));
                for (const char* q = strchr(d, ':'); q; q = strchr(q + 1, ',')) {
                    uint32_t idx = (uint32_t)strtoul(q + 1, nullptr, 10), o = mem::r32(mem::r32(tab + 24) + idx * 4);
                    LOG("  [%u] = %08x", idx, o);
                    for (uint32_t i = 0; o && i < 0x100; i += 32)
                        LOG("    +%02x: %08x %08x %08x %08x %08x %08x %08x %08x", i, mem::r32(o + i), mem::r32(o + i + 4), mem::r32(o + i + 8),
                            mem::r32(o + i + 12), mem::r32(o + i + 16), mem::r32(o + i + 20), mem::r32(o + i + 24), mem::r32(o + i + 28));
                }
            }
            last = v;
        }
        if (g_open.exchange(false)) protect(true);
        if (const char* d = getenv("BOTW_WATCH_DUMP")) {
            static uint32_t prev[8];
            uint32_t tab = mem::r32((uint32_t)strtoul(d, nullptr, 16));
            int n = 0;
            for (const char* q = strchr(d, ':'); q && n < 8; q = strchr(q + 1, ','), n++) {
                uint32_t idx = (uint32_t)strtoul(q + 1, nullptr, 10), o = tab ? mem::r32(mem::r32(tab + 24) + idx * 4) : 0;
                if (o != prev[n]) {
                    LOG("watch: entry [%u] %08x -> %08x", idx, prev[n], o);
                    prev[n] = o;
                    for (uint32_t i = 0; o && i < 0x80; i += 32)
                        LOG("    +%02x: %08x %08x %08x %08x %08x %08x %08x %08x", i, mem::r32(o + i), mem::r32(o + i + 4), mem::r32(o + i + 8),
                            mem::r32(o + i + 12), mem::r32(o + i + 16), mem::r32(o + i + 20), mem::r32(o + i + 24), mem::r32(o + i + 28));
                }
            }
        }
    }
}
}  // namespace

void debug_watch_init() {
    const char* e = getenv("BOTW_WATCH");
    if (!e) return;
    g_psize = (size_t)getpagesize();
    std::string s = e;
    size_t plus = s.find('+');
    uint32_t ptr = 0, off = 0, direct = 0;
    if (plus != std::string::npos) {
        ptr = (uint32_t)strtoul(s.substr(0, plus).c_str(), nullptr, 16);
        off = (uint32_t)strtoul(s.substr(plus + 1).c_str(), nullptr, 16);
    } else {
        direct = (uint32_t)strtoul(s.c_str(), nullptr, 16);
    }
    std::thread(watcher, ptr, off, direct).detach();
}

// Called first by the fault handler: true if the fault was a watched-page write (handled).
bool debug_watch_fault(void* addr, void* uctx) {
    uintptr_t page = g_page.load(), a = (uintptr_t)addr;
    if (!page || a < page || a >= page + g_psize) return false;
    if (a >= g_target.load() - 3 && a <= g_target.load()) {
        auto* uc = (ucontext_t*)uctx;
        uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
        char line[1024];
        int k = snprintf(line, sizeof line, "watch: write at %08x, host frames (base 0x%lx): 0x%llx 0x%llx",
                         (uint32_t)(a - (uintptr_t)g_mem), (unsigned long)_dyld_get_image_header(0),
                         (unsigned long long)pc, (unsigned long long)lr);
        for (int i = 0; i < 16 && fp && k < (int)sizeof line - 24; i++) {
            uint64_t* f = (uint64_t*)fp;
            k += snprintf(line + k, sizeof line - k, " 0x%llx", (unsigned long long)f[1]);
            fp = f[0];
        }
        log_write("%s", line);
    }
    mprotect((void*)page, g_psize, PROT_READ | PROT_WRITE);
    g_open = true;
    return true;
}
