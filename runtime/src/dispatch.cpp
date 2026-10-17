// Guest address -> host function, HLE thunks, traps.
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_table.h"
#include "hle.h"
#include "thread.h"

namespace {
std::map<std::string, GuestFn>& registry() {
    static std::map<std::string, GuestFn> r;
    return r;
}
std::string key(const char* lib, const char* name) { return std::string(lib) + "." + name; }

GuestFn* g_text_table = nullptr;  // (addr - kText) >> 2
uint32_t g_text_end = 0;
std::shared_mutex g_lock;
GuestFn g_thunks[mem::kThunkSize / 4];
const char* g_thunk_names[mem::kThunkSize / 4];
uint32_t g_next_thunk = 0;
std::unordered_map<uint32_t, GuestFn> g_stub_fns;
std::unordered_map<uint32_t, std::string> g_stub_names;
std::map<std::string, uint32_t> g_export_thunks;

struct CacheEntry {
    uint32_t addr;
    GuestFn fn;
};
thread_local CacheEntry t_cache[1024];
}  // namespace

HleRegistration::HleRegistration(const char* lib, const char* name, GuestFn fn) { registry()[key(lib, name)] = fn; }

GuestFn hle::find(const char* lib, const char* name) {
    auto it = registry().find(key(lib, name));
    return it == registry().end() ? nullptr : it->second;
}

namespace dispatch {
void init() {
    uint32_t hi = 0;
    for (uint32_t i = 0; i < g_guest_function_count; i++) hi = std::max(hi, g_guest_functions[i].addr);
    g_text_end = hi + 4;
    size_t n = (g_text_end - mem::kText) >> 2;
    g_text_table = (GuestFn*)calloc(n, sizeof(GuestFn));
    for (uint32_t i = 0; i < g_guest_function_count; i++)
        g_text_table[(g_guest_functions[i].addr - mem::kText) >> 2] = g_guest_functions[i].fn;
    LOG("dispatch: %u guest functions, %zu HLE exports registered", g_guest_function_count, registry().size());
}

GuestFn lookup(uint32_t addr) {
    if (addr >= mem::kText && addr < g_text_end) return g_text_table[(addr - mem::kText) >> 2];
    if (addr >= mem::kThunkBase && addr < mem::kThunkBase + mem::kThunkSize) return g_thunks[(addr - mem::kThunkBase) >> 2];
    CacheEntry& ce = t_cache[(addr >> 2) & 1023];
    if (ce.addr == addr && ce.fn) return ce.fn;
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto it = g_stub_fns.find(addr);
    if (it == g_stub_fns.end()) return nullptr;
    ce = {addr, it->second};
    return it->second;
}

uint32_t thunk(GuestFn fn, const char* name) {
    std::unique_lock<std::shared_mutex> l(g_lock);
    uint32_t i = g_next_thunk++;
    if (i >= mem::kThunkSize / 4) {
        LOG("FATAL: out of HLE thunks");
        abort();
    }
    g_thunks[i] = fn;
    g_thunk_names[i] = name;
    uint32_t a = mem::kThunkBase + i * 4;
    mem::w32(a, 0x4E800020);  // blr, in case anything reads the code
    return a;
}

static void trap_unimplemented_thunk(PPCContext* c);
static thread_local const char* t_last_unimpl = nullptr;

uint32_t export_thunk(const char* lib, const char* name) {
    std::string k = key(lib, name);
    {
        std::shared_lock<std::shared_mutex> l(g_lock);
        auto it = g_export_thunks.find(k);
        if (it != g_export_thunks.end()) return it->second;
    }
    GuestFn fn = hle::find(lib, name);
    const char* nm = strdup(k.c_str());
    uint32_t a = thunk(fn ? fn : trap_unimplemented_thunk, nm);
    std::unique_lock<std::shared_mutex> l(g_lock);
    g_export_thunks[k] = a;
    return a;
}

const char* thunk_name(uint32_t addr) {
    if (addr >= mem::kThunkBase && addr < mem::kThunkBase + mem::kThunkSize) {
        uint32_t i = (addr - mem::kThunkBase) >> 2;
        return i < g_next_thunk ? g_thunk_names[i] : nullptr;
    }
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto it = g_stub_names.find(addr);
    return it == g_stub_names.end() ? nullptr : it->second.c_str();
}

void add_import_stub(uint32_t addr, GuestFn fn, const char* lib, const char* name) {
    std::unique_lock<std::shared_mutex> l(g_lock);
    g_stub_fns[addr] = fn;
    g_stub_names[addr] = key(lib, name);
}

static void trap_unimplemented_thunk(PPCContext* c) {
    rt_trap(c, c->lr, "call to an unimplemented dynamically-loaded export (see thunk name above)");
}
}  // namespace dispatch

// ---- guest calls -----------------------------------------------------------------------------
uint32_t guest_call(PPCContext* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    PPCContext saved = *c;
    int k = 3;
    for (uint32_t a : args) c->r[k++] = a;
    c->lr = 0;  // returns come back here through the C++ call stack, never via LR
    rt_call(c, fn);
    uint32_t r = c->r[3];
    uint32_t r4 = c->r[4];
    double f1 = c->f[1];
    *c = saved;
    c->r[3] = r;
    c->r[4] = r4;
    c->f[1] = f1;
    return r;
}

// ---- runtime entry points used by generated code -----------------------------------------------
extern "C" void rt_call(PPCContext* c, uint32_t target) {
    GuestFn fn = dispatch::lookup(target);
    if (__builtin_expect(!fn, 0)) rt_trap(c, target, "indirect call to an address with no recompiled function");
    fn(c);
}

static void dump_context(PPCContext* c) {
    if (!c) return;
    for (int i = 0; i < 32; i += 8)
        LOG("  r%-2d %08x %08x %08x %08x %08x %08x %08x %08x", i, c->r[i], c->r[i + 1], c->r[i + 2], c->r[i + 3],
            c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
    LOG("  lr %08x ctr %08x", c->lr, c->ctr);
    // guest stack back-chain: [sp] = caller's sp, [caller sp + 4] = saved LR
    uint32_t sp = c->r[1];
    for (int i = 0; i < 24 && sp > 0x10000 && sp < 0xFFFF0000u; i++) {
        uint32_t next = mem::r32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        uint32_t lr = mem::r32(next + 4);
        LOG("  guest frame %2d: sp %08x saved lr %08x", i, next, lr);
        sp = next;
    }
}

// Host backtrace: raw addresses plus the image base; tools/symbolize.sh turns them into guest
// addresses with atos (generated code carries `#line <guest address>` for every instruction).
static void host_backtrace() {
    void* frames[48];
    int n = backtrace(frames, 48);
    char line[2048];
    int k = snprintf(line, sizeof line, "  host frames (base 0x%lx):", (unsigned long)_dyld_get_image_header(0));
    for (int i = 0; i < n && k < (int)sizeof line - 24; i++) k += snprintf(line + k, sizeof line - k, " %p", frames[i]);
    LOG("%s", line);
}

extern "C" PPC_NORETURN void rt_fatal_exit(void);

extern "C" PPC_NORETURN void rt_trap(PPCContext* c, uint32_t addr, const char* why) {
    const char* tn = threads::current_name();
    LOG("FATAL: %s at %08x (thread %s)", why, addr, tn ? tn : "?");
    if (const char* n = dispatch::thunk_name(addr)) LOG("  target is %s", n);
    dump_context(c);
    host_backtrace();
    rt_fatal_exit();
}

extern "C" PPC_NORETURN void rt_unimplemented(PPCContext* c, const char* lib, const char* name) {
    LOG("FATAL: unimplemented import %s.%s (called from %08x)", lib, name, c->lr - 4);
    dump_context(c);
    rt_fatal_exit();
}

extern "C" PPC_NORETURN void rt_bad_return(PPCContext* c, uint32_t addr, uint32_t lr0) {
    LOG("FATAL: blr at %08x with LR %08x but the function was entered with LR %08x (computed jump/longjmp?)",
        addr, c->lr, lr0);
    dump_context(c);
    host_backtrace();
    rt_fatal_exit();
}

extern "C" void rt_bad_switch(PPCContext* c, uint32_t addr, uint32_t target) {
    rt_trap(c, addr, "switch jump to an address outside its table");
}

extern "C" void rt_probe(PPCContext* c, uint32_t addr) {
    LOG("probe %08x: r3=%08x r4=%08x r5=%08x r6=%08x r31=%08x lr=%08x", addr, c->r[3], c->r[4], c->r[5], c->r[6],
        c->r[31], c->lr);
}

// --trace-hle: every import call with arguments and result
extern "C" int g_trace_hle = 0;
extern "C" void rt_trace_hle(PPCContext* c, const char* name, int ret) {
    if (!ret)
        LOG("[%s] %s(%08x, %08x, %08x, %08x, %08x) from %08x", threads::current_name() ? threads::current_name() : "?",
            name, c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->lr - 4);
    else
        LOG("[%s]   %s -> %08x", threads::current_name() ? threads::current_name() : "?", name, c->r[3]);
}
