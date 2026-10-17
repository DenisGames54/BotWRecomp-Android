#include "mem.h"

#include <stdlib.h>
#include <sys/mman.h>

#include <map>
#include <mutex>

#include "log.h"

uint8_t* g_mem = nullptr;
uint8_t* memory_base = nullptr;  // Cemu-compat alias of g_mem

namespace mem {
namespace {
// Simple first-fit allocator over [kSysBase, kSysEnd); runtime allocations are few and long-lived.
std::mutex g_lock;
std::map<uint32_t, uint32_t> g_free = {{kSysBase, kSysEnd - kSysBase}};  // addr -> size
std::map<uint32_t, uint32_t> g_used;
}  // namespace

void init() {
    size_t size = (1ull << 32) + 0x10000;  // + guard so accesses near 0xFFFFFFFF stay in bounds
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOG("FATAL: cannot reserve the 4 GB guest address space");
        abort();
    }
    g_mem = (uint8_t*)p;
    memory_base = g_mem;
    mprotect(g_mem, 0x10000, PROT_NONE);  // catch guest null-pointer accesses
    LOG("guest memory at %p", (void*)g_mem);
}

uint32_t sys_alloc(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> l(g_lock);
    size = (size + 15) & ~15u;
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        uint32_t a = (it->first + align - 1) & ~(align - 1);
        uint32_t pad = a - it->first;
        if (it->second < pad + size) continue;
        uint32_t base = it->first, len = it->second;
        g_free.erase(it);
        if (pad) g_free[base] = pad;
        if (len > pad + size) g_free[a + size] = len - pad - size;
        g_used[a] = size;
        memset(g_mem + a, 0, size);
        return a;
    }
    LOG("FATAL: runtime guest heap exhausted (size %x)", size);
    abort();
}

void sys_free(uint32_t addr) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_used.find(addr);
    if (it == g_used.end()) return;
    uint32_t a = addr, s = it->second;
    g_used.erase(it);
    auto nx = g_free.lower_bound(a);
    if (nx != g_free.end() && nx->first == a + s) {
        s += nx->second;
        g_free.erase(nx);
    }
    auto pv = g_free.lower_bound(a);
    if (pv != g_free.begin()) {
        --pv;
        if (pv->first + pv->second == a) {
            pv->second += s;
            return;
        }
    }
    g_free[a] = s;
}

uint32_t sys_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    uint32_t a = sys_alloc((uint32_t)n, 4);
    memcpy(g_mem + a, s, n);
    return a;
}
}  // namespace mem
