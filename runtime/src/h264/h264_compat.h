// BotWRecomp: glue for the H.264 decoder ported from Cemu (MPL-2.0).
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <array>

#include "../compat/cemu.h"
#include "../compat/cemu_export.h"

namespace H264 {
// Host-side replacement for the guest OSEvent the Cemu backend signals from its decoder thread.
// Auto-reset: Wait() consumes the signal.
class H264Event {
   public:
    void Signal() {
        {
            std::lock_guard<std::mutex> l(m_);
            sig_ = true;
        }
        cv_.notify_one();
    }
    void Reset() {
        std::lock_guard<std::mutex> l(m_);
        sig_ = false;
    }
    void Wait() {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return sig_; });
        sig_ = false;
    }

   private:
    std::mutex m_;
    std::condition_variable cv_;
    bool sig_ = false;
};

class CounterSemaphore {
   public:
    void reset() {
        std::lock_guard<std::mutex> l(m_);
        c_ = 0;
    }
    void increment() {
        {
            std::lock_guard<std::mutex> l(m_);
            ++c_;
        }
        cv_.notify_one();
    }
    void decrementWithWait() {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return c_ > 0; });
        --c_;
    }

   private:
    std::mutex m_;
    std::condition_variable cv_;
    uint64 c_ = 0;
};

class BenchmarkTimer {
   public:
    void Start() { a_ = std::chrono::steady_clock::now(); }
    void Stop() { b_ = std::chrono::steady_clock::now(); }
    double GetElapsedMilliseconds() const { return std::chrono::duration<double, std::milli>(b_ - a_).count(); }

   private:
    std::chrono::steady_clock::time_point a_{}, b_{};
};

}  // namespace H264
using H264::BenchmarkTimer;
using H264::CounterSemaphore;
using H264::H264Event;

#ifndef cemu_assert
#define cemu_assert(x) ((void)0)
#define cemu_assert_debug(x) ((void)0)
#endif
