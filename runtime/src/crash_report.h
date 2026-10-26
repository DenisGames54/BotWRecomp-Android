// Fatal-signal report for Linux/Android: faulting PC/LR/SP/FP, x0-x29, and a symbolised backtrace
// (module + file offset, read from /proc/self/maps) written through LOG(), so it lands in runtime.log
// and in logcat. Header-only; include it after the header that defines LOG(). Elsewhere it is a no-op.
//
// Symbolise on the PC: llvm-addr2line -f -C -e <unstripped libmain.so> <offset printed after '+0x'>
#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>

namespace crash_report {
namespace detail {

struct Line {
    char buf[384];
    size_t n = 0;
    void str(const char* s) { while (*s && n < sizeof(buf) - 1) buf[n++] = *s++; }
    void hex(uint64_t v) {
        static const char d[] = "0123456789abcdef";
        str("0x");
        bool started = false;
        for (int i = 60; i >= 0; i -= 4) {
            int x = (int)((v >> i) & 0xf);
            if (x || started || i == 0) { started = true; if (n < sizeof(buf) - 1) buf[n++] = d[x]; }
        }
    }
    void dec(long v) {
        char t[24]; int i = 0;
        bool neg = v < 0;
        unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
        do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u);
        if (neg) str("-");
        while (i && n < sizeof(buf) - 1) buf[n++] = t[--i];
    }
    void emit() { buf[n] = 0; LOG("%s", buf); n = 0; }
};

inline bool parse_hex(const char*& p, uint64_t& out) {
    out = 0; bool any = false;
    for (;; ++p) {
        char c = *p; int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
        else break;
        out = (out << 4) | (uint64_t)v; any = true;
    }
    return any;
}
inline void skip_field(const char*& p) { while (*p && *p != ' ') ++p; while (*p == ' ') ++p; }

// "lo-hi perms offset dev inode   path"  ->  "path+0xFILEOFFSET"
inline bool match_line(const char* p, uintptr_t addr, char* out, size_t outsz) {
    uint64_t lo, hi, off;
    if (!parse_hex(p, lo) || *p != '-') return false;
    ++p;
    if (!parse_hex(p, hi) || addr < lo || addr >= hi) return false;
    skip_field(p); skip_field(p);
    if (!parse_hex(p, off)) return false;
    skip_field(p); skip_field(p); skip_field(p);
    const char* name = *p ? p : "[anon]";
    size_t k = 0;
    while (name[k] && k < outsz - 24) { out[k] = name[k]; ++k; }
    static const char d[] = "0123456789abcdef";
    out[k++] = '+'; out[k++] = '0'; out[k++] = 'x';
    uint64_t rel = off + (addr - lo);
    bool started = false;
    for (int s = 60; s >= 0; s -= 4) {
        int x = (int)((rel >> s) & 0xf);
        if (x || started || s == 0) { started = true; out[k++] = d[x]; }
    }
    out[k] = 0;
    return true;
}

inline bool resolve(uintptr_t addr, char* out, size_t outsz) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char chunk[1024], line[512];
    size_t ll = 0;
    bool found = false;
    for (;;) {
        ssize_t r = read(fd, chunk, sizeof(chunk));
        if (r <= 0) break;
        for (ssize_t i = 0; i < r && !found; ++i) {
            if (chunk[i] == '\n') { line[ll] = 0; found = match_line(line, addr, out, outsz); ll = 0; }
            else if (ll < sizeof(line) - 1) line[ll++] = chunk[i];
        }
        if (found) break;
    }
    close(fd);
    return found;
}

inline void frame(int idx, uintptr_t pc) {
    char m[320];
    Line l;
    l.str("  #"); l.dec(idx); l.str(" pc "); l.hex(pc); l.str("  ");
    l.str(resolve(pc, m, sizeof(m)) ? m : "(unmapped)");
    l.emit();
}

struct Bt { uintptr_t pcs[40]; int n; };
inline _Unwind_Reason_Code bt_cb(struct _Unwind_Context* c, void* arg) {
    Bt* s = static_cast<Bt*>(arg);
    uintptr_t pc = _Unwind_GetIP(c);
    if (pc && s->n < 40) s->pcs[s->n++] = pc;
    return s->n >= 40 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

inline std::atomic<int>& busy() { static std::atomic<int> b{0}; return b; }

}  // namespace detail

// Call from inside a SA_SIGINFO handler. guest_size == 0 disables the guest-range note.
inline void dump(int sig, siginfo_t* si, void* uctx, uintptr_t guest_base, size_t guest_size) {
    using namespace detail;
    if (busy().exchange(1) != 0) return;  // another thread is already reporting, or we re-entered
    Line l;
    l.str("=== crash report: signal "); l.dec(sig);
    if (si) {
        l.str(" code "); l.dec(si->si_code);
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
            uintptr_t a = (uintptr_t)si->si_addr;
            l.str(" fault_addr "); l.hex(a);
            if (guest_size && a >= guest_base && a < guest_base + guest_size) l.str(" (inside guest memory)");
            else if (a < 0x10000) l.str(" (null + member offset)");
        }
    }
    l.str(" tid "); l.dec((long)syscall(SYS_gettid)); l.str(" ===");
    l.emit();

    uintptr_t pc = 0, lr = 0, sp = 0, fp = 0;
#if defined(__aarch64__)
    if (uctx) {
        const mcontext_t& m = static_cast<ucontext_t*>(uctx)->uc_mcontext;
        pc = (uintptr_t)m.pc; sp = (uintptr_t)m.sp; lr = (uintptr_t)m.regs[30]; fp = (uintptr_t)m.regs[29];
        l.str("pc "); l.hex(pc); l.str(" lr "); l.hex(lr); l.str(" sp "); l.hex(sp); l.str(" fp "); l.hex(fp); l.emit();
        for (int i = 0; i < 30; i += 3) {
            for (int j = 0; j < 3; ++j) { l.str(" x"); l.dec(i + j); l.str("="); l.hex(m.regs[i + j]); }
            l.emit();
        }
    }
#elif defined(__x86_64__)
    if (uctx) {  // host-side tests only
        const mcontext_t& m = static_cast<ucontext_t*>(uctx)->uc_mcontext;
        pc = (uintptr_t)m.gregs[REG_RIP]; sp = (uintptr_t)m.gregs[REG_RSP]; fp = (uintptr_t)m.gregs[REG_RBP];
        l.str("pc "); l.hex(pc); l.str(" sp "); l.hex(sp); l.emit();
    }
#endif
    int idx = 0;
    if (pc) { LOG("faulting frame:"); frame(idx++, pc); }
    if (lr) frame(idx++, lr);

    Bt bt; bt.n = 0;
    _Unwind_Backtrace(bt_cb, &bt);
    l.str("unwinder frames ("); l.dec(bt.n); l.str("):"); l.emit();
    for (int i = 0; i < bt.n; ++i) frame(i, bt.pcs[i]);

    if (fp && sp && fp >= sp && fp - sp < (1u << 20)) {
        LOG("frame-pointer walk:");
        uintptr_t cur = fp;
        for (int i = 0; i < 24; ++i) {
            if ((cur & 7) || cur < sp || cur - sp > (8u << 20)) break;
            uintptr_t* f = reinterpret_cast<uintptr_t*>(cur);
            uintptr_t next = f[0], ret = f[1];
            if (!ret) break;
            frame(i, ret);
            if (next <= cur) break;
            cur = next;
        }
    }
    LOG("=== end of crash report ===");
    busy().store(0);
}

}  // namespace crash_report
#else
namespace crash_report {
template <class A, class B>
inline void dump(int, A, B, uintptr_t, size_t) {}
}  // namespace crash_report
#endif
