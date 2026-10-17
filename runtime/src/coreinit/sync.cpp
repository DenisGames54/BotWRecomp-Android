// coreinit synchronization: OSMutex, OSFastMutex, OSEvent, OSSemaphore, OSRendezvous,
// OSMessageQueue and the GHS C-library locks. State lives in the guest structs where games may
// look; waiting uses per-object host condition variables under threads::g_klock.
#include <chrono>
#include <condition_variable>
#include <unordered_map>

#include "hle.h"
#include "thread.h"

using threads::g_klock;
using threads::GuestThread;

namespace {
std::unordered_map<uint32_t, std::condition_variable*> g_cvs;
std::condition_variable& cv(uint32_t obj) {
    auto& p = g_cvs[obj];
    if (!p) p = new std::condition_variable;
    return *p;
}

// ---- recursive mutexes (OSMutex and OSFastMutex share the implementation) ----------------
struct MutexState {
    GuestThread* owner = nullptr;
    int count = 0;
};
std::unordered_map<uint32_t, MutexState> g_mutexes;

void mutex_lock(uint32_t m) {
    GuestThread* me = threads::current();
    std::unique_lock<std::mutex> l(g_klock);
    MutexState& s = g_mutexes[m];
    if (s.owner != me && s.owner) {
        threads::WaitMark w("mutex", m);
        cv(m).wait(l, [&] { return s.owner == nullptr; });
    }
    s.owner = me;
    s.count++;
}
bool mutex_trylock(uint32_t m) {
    GuestThread* me = threads::current();
    std::lock_guard<std::mutex> l(g_klock);
    MutexState& s = g_mutexes[m];
    if (s.owner && s.owner != me) return false;
    s.owner = me;
    s.count++;
    return true;
}
void mutex_unlock(uint32_t m) {
    GuestThread* me = threads::current();
    std::lock_guard<std::mutex> l(g_klock);
    MutexState& s = g_mutexes[m];
    if (s.owner != me) {
        LOG("mutex %08x unlocked by a non-owner (%s)", m, me ? me->name.c_str() : "?");
        return;
    }
    if (--s.count == 0) {
        s.owner = nullptr;
        cv(m).notify_one();
    }
}
void mutex_init(uint32_t m) {
    std::lock_guard<std::mutex> l(g_klock);
    g_mutexes[m] = MutexState{};
}
}  // namespace

HLE_EXPORT(coreinit, OSInitMutex) {
    uint32_t m = ARG(0);
    memset(mem::host(m), 0, 0x2C);
    mem::w32(m, 0x6D557458);  // 'mUtX'
    mutex_init(m);
}
HLE_EXPORT(coreinit, OSLockMutex) { mutex_lock(ARG(0)); }
HLE_EXPORT(coreinit, OSTryLockMutex) { RET(mutex_trylock(ARG(0))); }
HLE_EXPORT(coreinit, OSUnlockMutex) { mutex_unlock(ARG(0)); }
HLE_EXPORT(coreinit, OSFastMutex_Init) {
    memset(mem::host(ARG(0)), 0, 0x2C);
    mem::w32(ARG(0), 0x664D7458);  // 'fMtX'
    mutex_init(ARG(0));
}
HLE_EXPORT(coreinit, OSFastMutex_Lock) { mutex_lock(ARG(0)); }
HLE_EXPORT(coreinit, OSFastMutex_TryLock) { RET(mutex_trylock(ARG(0))); }
HLE_EXPORT(coreinit, OSFastMutex_Unlock) { mutex_unlock(ARG(0)); }

// GHS C/C++ runtime locks: one global recursive lock plus mutexes allocated per object.
HLE_EXPORT(coreinit, __ghsLock) { mutex_lock(0xFFFF0001u); }
HLE_EXPORT(coreinit, __ghsUnlock) { mutex_unlock(0xFFFF0001u); }
HLE_EXPORT(coreinit, __ghs_flock_file) { mutex_lock(0xFFFE0000u + (ARG(0) & 0xFFFF)); }
HLE_EXPORT(coreinit, __ghs_funlock_file) { mutex_unlock(0xFFFE0000u + (ARG(0) & 0xFFFF)); }
// __ghs_mtx_init(void** m): allocate; lock/unlock(void* m); dst(void** m)
HLE_EXPORT(coreinit, __ghs_mtx_init) {
    uint32_t m = mem::sys_alloc(0x2C, 8);
    mutex_init(m);
    mem::w32(ARG(0), m);
}
HLE_EXPORT(coreinit, __ghs_mtx_dst) {}
HLE_EXPORT(coreinit, __ghs_mtx_lock) { mutex_lock(ARG(0)); }
HLE_EXPORT(coreinit, __ghs_mtx_unlock) { mutex_unlock(ARG(0)); }

// ---- events: +0x0C value, +0x20 mode (0 manual reset, 1 auto reset) ----------------------
HLE_EXPORT(coreinit, OSInitEvent) {
    uint32_t e = ARG(0);
    memset(mem::host(e), 0, 0x24);
    mem::w32(e, 0x65566E54);  // 'eVnT'
    mem::w32(e + 0x0C, ARG(1));
    mem::w32(e + 0x20, ARG(2));
}
HLE_EXPORT(coreinit, OSSignalEvent) {
    uint32_t e = ARG(0);
    std::lock_guard<std::mutex> l(g_klock);
    mem::w32(e + 0x0C, 1);
    if (mem::r32(e + 0x20) == 1) cv(e).notify_one();
    else cv(e).notify_all();
}
HLE_EXPORT(coreinit, OSResetEvent) {
    std::lock_guard<std::mutex> l(g_klock);
    mem::w32(ARG(0) + 0x0C, 0);
}
HLE_EXPORT(coreinit, OSWaitEvent) {
    uint32_t e = ARG(0);
    threads::WaitMark w("event", e);
    std::unique_lock<std::mutex> l(g_klock);
    cv(e).wait(l, [&] { return mem::r32(e + 0x0C) != 0; });
    if (mem::r32(e + 0x20) == 1) mem::w32(e + 0x0C, 0);
}
// BOOL OSWaitEventWithTimeout(OSEvent*, OSTime timeout_ns)
HLE_EXPORT(coreinit, OSWaitEventWithTimeout) {
    uint32_t e = ARG(0);
    uint64_t ns = ARG64(2);
    threads::WaitMark w("event/timeout", e);
    std::unique_lock<std::mutex> l(g_klock);
    bool ok = cv(e).wait_for(l, std::chrono::nanoseconds(ns), [&] { return mem::r32(e + 0x0C) != 0; });
    if (ok && mem::r32(e + 0x20) == 1) mem::w32(e + 0x0C, 0);
    RET(ok);
}

// ---- semaphores: +0x0C count -----------------------------------------------------------------
HLE_EXPORT(coreinit, OSInitSemaphore) {
    uint32_t s = ARG(0);
    memset(mem::host(s), 0, 0x20);
    mem::w32(s, 0x73506852);  // 'sPhR'
    mem::w32(s + 0x0C, ARG(1));
}
HLE_EXPORT(coreinit, OSWaitSemaphore) {
    uint32_t s = ARG(0);
    threads::WaitMark w("semaphore", s);
    std::unique_lock<std::mutex> l(g_klock);
    cv(s).wait(l, [&] { return (int32_t)mem::r32(s + 0x0C) > 0; });
    int32_t v = (int32_t)mem::r32(s + 0x0C);
    mem::w32(s + 0x0C, v - 1);
    RET(v);
}
HLE_EXPORT(coreinit, OSSignalSemaphore) {
    uint32_t s = ARG(0);
    std::lock_guard<std::mutex> l(g_klock);
    int32_t v = (int32_t)mem::r32(s + 0x0C);
    mem::w32(s + 0x0C, v + 1);
    cv(s).notify_one();
    RET(v);
}
HLE_EXPORT(coreinit, OSGetSemaphoreCount) {
    std::lock_guard<std::mutex> l(g_klock);
    RET(mem::r32(ARG(0) + 0x0C));
}

// ---- rendezvous: core[3] arrival flags ---------------------------------------------------------
HLE_EXPORT(coreinit, OSInitRendezvous) {
    memset(mem::host(ARG(0)), 0, 0x10);
}
// BOOL OSWaitRendezvous(OSRendezvous*, uint32_t coreMask)
HLE_EXPORT(coreinit, OSWaitRendezvous) {
    uint32_t r = ARG(0), mask = ARG(1);
    int core = c->core;
    threads::WaitMark w("rendezvous", r);
    std::unique_lock<std::mutex> l(g_klock);
    mem::w32(r + 4 * core, 1);
    cv(r).notify_all();
    cv(r).wait(l, [&] {
        for (int k = 0; k < 3; k++)
            if ((mask & (1u << k)) && !mem::r32(r + 4 * k)) return false;
        return true;
    });
    RET(1);
}

// ---- message queues --------------------------------------------------------------------------
// +0x2C messages, +0x30 capacity, +0x34 first, +0x38 used; message = 16 bytes
namespace {
constexpr uint32_t Q_MSGS = 0x2C, Q_CAP = 0x30, Q_FIRST = 0x34, Q_USED = 0x38;
}
HLE_EXPORT(coreinit, OSInitMessageQueue) {
    uint32_t q = ARG(0);
    memset(mem::host(q), 0, 0x3C);
    mem::w32(q, 0x6D536751);  // 'mSgQ'
    mem::w32(q + Q_MSGS, ARG(1));
    mem::w32(q + Q_CAP, ARG(2));
}
// BOOL OSSendMessage(q, msg, flags): flags 1 = block, 2 = jam (front)
HLE_EXPORT(coreinit, OSSendMessage) {
    uint32_t q = ARG(0), msg = ARG(1), flags = ARG(2);
    std::unique_lock<std::mutex> l(g_klock);
    uint32_t cap = mem::r32(q + Q_CAP);
    if (mem::r32(q + Q_USED) >= cap) {
        if (!(flags & 1)) {
            RET(0);
            return;
        }
        threads::WaitMark w("msgq send", q);
        cv(q).wait(l, [&] { return mem::r32(q + Q_USED) < cap; });
    }
    uint32_t first = mem::r32(q + Q_FIRST), used = mem::r32(q + Q_USED);
    uint32_t slot;
    if (flags & 2) {
        first = (first + cap - 1) % cap;
        mem::w32(q + Q_FIRST, first);
        slot = first;
    } else {
        slot = (first + used) % cap;
    }
    memcpy(mem::host(mem::r32(q + Q_MSGS) + slot * 16), mem::host(msg), 16);
    mem::w32(q + Q_USED, used + 1);
    cv(q).notify_all();
    RET(1);
}
// BOOL OSReceiveMessage(q, msg, flags): flags 1 = block
HLE_EXPORT(coreinit, OSReceiveMessage) {
    uint32_t q = ARG(0), msg = ARG(1), flags = ARG(2);
    std::unique_lock<std::mutex> l(g_klock);
    if (mem::r32(q + Q_USED) == 0) {
        if (!(flags & 1)) {
            RET(0);
            return;
        }
        threads::WaitMark w("msgq receive", q);
        cv(q).wait(l, [&] { return mem::r32(q + Q_USED) != 0; });
    }
    uint32_t first = mem::r32(q + Q_FIRST), cap = mem::r32(q + Q_CAP);
    memcpy(mem::host(msg), mem::host(mem::r32(q + Q_MSGS) + first * 16), 16);
    mem::w32(q + Q_FIRST, (first + 1) % cap);
    mem::w32(q + Q_USED, mem::r32(q + Q_USED) - 1);
    cv(q).notify_all();
    RET(1);
}
HLE_EXPORT(coreinit, OSPeekMessage) {
    uint32_t q = ARG(0), msg = ARG(1);
    std::lock_guard<std::mutex> l(g_klock);
    if (mem::r32(q + Q_USED) == 0) {
        RET(0);
        return;
    }
    if (msg) memcpy(mem::host(msg), mem::host(mem::r32(q + Q_MSGS) + mem::r32(q + Q_FIRST) * 16), 16);
    RET(1);
}
