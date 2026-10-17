#include "input.h"

#include <deque>
#include <mutex>

namespace input {
namespace {
std::mutex g_lock;
PadState g_pad;
}
PadState gamepad() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_pad;
}
void set_gamepad(const PadState& s) {
    std::lock_guard<std::mutex> l(g_lock);
    g_pad = s;
}

namespace {
std::mutex g_rumble_lock;
std::deque<bool> g_rumble;  // 60 Hz steps
}  // namespace
bool push_rumble(const uint8_t* pattern, int bits) {
    std::lock_guard<std::mutex> l(g_rumble_lock);
    if (g_rumble.size() >= 5 * 60) return false;  // Cemu queues at most 5 one-second patterns
    for (int i = 0; i < bits; i += 2)  // two 120 Hz bits per step
        g_rumble.push_back((pattern[i / 8] >> (i % 8)) & 3);
    return true;
}
void clear_rumble() {
    std::lock_guard<std::mutex> l(g_rumble_lock);
    g_rumble.clear();
}
bool rumble_step() {
    std::lock_guard<std::mutex> l(g_rumble_lock);
    if (g_rumble.empty()) return false;
    bool on = g_rumble.front();
    g_rumble.pop_front();
    return on;
}
}  // namespace input
