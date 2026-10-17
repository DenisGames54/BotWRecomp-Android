// Tables emitted by tools/recompile.py (generated/table.cpp).
#pragma once
#include "ppc.h"

struct GuestFnEntry {
    uint32_t addr;
    GuestFn fn;
};
struct GuestImportEntry {
    uint32_t addr;  // stub/variable address in the .fimport_/.dimport_ section
    const char* lib;
    const char* name;
    bool is_data;
    GuestFn fn;  // HLE implementation (function imports)
};

extern const GuestFnEntry g_guest_functions[];
extern const uint32_t g_guest_function_count;
extern const GuestImportEntry g_guest_imports[];
extern const uint32_t g_guest_import_count;
