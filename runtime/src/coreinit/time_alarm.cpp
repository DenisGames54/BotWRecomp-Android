// coreinit time (OSGetTime ticks since 2000-01-01, OSGetSystemTime since boot) and alarms.
#include <time.h>

#include <map>
#include <thread>
#include <unordered_map>

#include "hle.h"
#include "os.h"
#include "thread.h"

namespace {
constexpr uint64_t kHz = 62156250;
int64_t g_epoch_offset_ticks = 0;  // OSGetTime() - OSGetSystemTime()

int64_t os_time() { return (int64_t)rt_timebase() + g_epoch_offset_ticks; }

// ---- alarms ----------------------------------------------------------------------------------
struct Alarm {
    uint64_t deadline = 0;  // system-time ticks
    uint64_t period = 0;
    uint32_t handler = 0;
    uint64_t gen = 0;
    bool armed = false;
};
std::mutex g_alarm_lock;
std::condition_variable g_alarm_cv;
std::unordered_map<uint32_t, Alarm> g_alarms;
std::multimap<uint64_t, std::pair<uint32_t, uint64_t>> g_queue;  // deadline -> (alarm, gen)
uint64_t g_gen = 0;
uint32_t g_alarm_context = 0;

void arm(uint32_t a, uint64_t deadline, uint64_t period, uint32_t handler) {
    std::lock_guard<std::mutex> l(g_alarm_lock);
    Alarm& al = g_alarms[a];
    al.deadline = deadline;
    al.period = period;
    al.handler = handler;
    al.gen = ++g_gen;
    al.armed = true;
    g_queue.emplace(deadline, std::make_pair(a, al.gen));
    mem::w32(a + 0x0C, handler);
    mem::w64(a + 0x18, deadline + g_epoch_offset_ticks);
    mem::w64(a + 0x28, period);
    g_alarm_cv.notify_all();
}

void alarm_thread() {
    threads::GuestThread* t = threads::adopt_host_thread("alarms", 0x10000, 1);
    for (;;) {
        uint32_t a = 0, handler = 0;
        {
            std::unique_lock<std::mutex> l(g_alarm_lock);
            for (;;) {
                if (g_queue.empty()) {
                    g_alarm_cv.wait(l);
                    continue;
                }
                auto it = g_queue.begin();
                uint64_t now = rt_timebase();
                if (it->first > now) {
                    uint64_t ns = (it->first - now) * 1000000000ull / kHz;
                    g_alarm_cv.wait_for(l, std::chrono::nanoseconds(ns));
                    continue;
                }
                uint32_t aa = it->second.first;
                uint64_t gen = it->second.second;
                g_queue.erase(it);
                Alarm& al = g_alarms[aa];
                if (!al.armed || al.gen != gen) continue;
                a = aa;
                handler = al.handler;
                if (al.period) {
                    al.deadline += al.period;
                    if (al.deadline < now) al.deadline = now + al.period;  // don't spiral after a stall
                    g_queue.emplace(al.deadline, std::make_pair(aa, gen));
                } else {
                    al.armed = false;
                }
                break;
            }
        }
        guest_call(&t->ctx, handler, {a, g_alarm_context});
    }
}
}  // namespace

namespace os {
void init_time() {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    // seconds since 2000-01-01 in local time (Cafe clocks are local)
    int64_t local = (int64_t)now + lt.tm_gmtoff - 946684800;
    g_epoch_offset_ticks = local * (int64_t)kHz - (int64_t)rt_timebase();
    g_alarm_context = mem::sys_alloc(0x320, 8);
    std::thread(alarm_thread).detach();
}
}  // namespace os

HLE_EXPORT(coreinit, OSGetTime) { RET64(os_time()); }
HLE_EXPORT(coreinit, OSGetSystemTime) { RET64(rt_timebase()); }

// void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* out)
HLE_EXPORT(coreinit, OSTicksToCalendarTime) {
    int64_t ticks = (int64_t)ARG64(0);
    uint32_t out = ARG(2);
    int64_t secs = ticks / (int64_t)kHz, sub = ticks % (int64_t)kHz;
    time_t t = (time_t)(secs + 946684800);
    struct tm g;
    gmtime_r(&t, &g);
    mem::w32(out + 0x00, g.tm_sec);
    mem::w32(out + 0x04, g.tm_min);
    mem::w32(out + 0x08, g.tm_hour);
    mem::w32(out + 0x0C, g.tm_mday);
    mem::w32(out + 0x10, g.tm_mon);
    mem::w32(out + 0x14, g.tm_year + 1900);
    mem::w32(out + 0x18, g.tm_wday);
    mem::w32(out + 0x1C, g.tm_yday);
    mem::w32(out + 0x20, (uint32_t)(sub * 1000 / (int64_t)kHz));
    mem::w32(out + 0x24, (uint32_t)(sub * 1000000 / (int64_t)kHz % 1000));
}

HLE_EXPORT(coreinit, OSCreateAlarm) {
    uint32_t a = ARG(0);
    memset(mem::host(a), 0, 0x58);
    mem::w32(a, 0x614C724D);  // 'aLrM'
}
// BOOL OSSetAlarm(OSAlarm*, OSTime delay, handler)
HLE_EXPORT(coreinit, OSSetAlarm) {
    uint32_t a = ARG(0);
    uint64_t delay = ARG64(2);
    arm(a, rt_timebase() + delay, 0, ARG(4));
    RET(1);
}
// BOOL OSSetPeriodicAlarm(OSAlarm*, OSTime start (absolute), OSTime period, handler)
HLE_EXPORT(coreinit, OSSetPeriodicAlarm) {
    uint32_t a = ARG(0);
    int64_t start = (int64_t)ARG64(2);
    uint64_t period = ARG64(4);
    int64_t sys = start - g_epoch_offset_ticks;
    if (sys < (int64_t)rt_timebase()) {
        // start in the past: next multiple of the period from now
        uint64_t now = rt_timebase();
        uint64_t k = period ? (now - sys) / period + 1 : 0;
        sys += (int64_t)(k * period);
    }
    arm(a, (uint64_t)sys, period, ARG(6));
    RET(1);
}
HLE_EXPORT(coreinit, OSCancelAlarm) {
    std::lock_guard<std::mutex> l(g_alarm_lock);
    auto it = g_alarms.find(ARG(0));
    bool was = it != g_alarms.end() && it->second.armed;
    if (it != g_alarms.end()) it->second.armed = false;
    RET(was);
}
HLE_EXPORT(coreinit, OSSetAlarmUserData) { mem::w32(ARG(0) + 0x38, ARG(1)); }
HLE_EXPORT(coreinit, OSGetAlarmUserData) { RET(mem::r32(ARG(0) + 0x38)); }
