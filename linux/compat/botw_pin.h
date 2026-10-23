#pragma once
#include <sched.h>
#include <stdlib.h>
static inline void botw_pin_from_env(const char* var) {
    const char* e = getenv(var);
    if (!e || !*e) return;
    cpu_set_t set; CPU_ZERO(&set);
    for (const char* p = e; *p;) {
        char* end; long c = strtol(p, &end, 10);
        if (end == p) break;
        if (c >= 0 && c < CPU_SETSIZE) CPU_SET((int)c, &set);
        p = (*end == ',') ? end + 1 : end;
    }
    sched_setaffinity(0, sizeof(set), &set);
}
