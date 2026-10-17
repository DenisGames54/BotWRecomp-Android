// Loads the main RPX into guest memory at its link addresses and applies its relocations.
#pragma once
#include <stdint.h>

#include <string>

namespace loader {
struct Module {
    std::string path;
    uint32_t entry = 0;
    uint32_t sda_base = 0, sda2_base = 0;
    uint32_t stack_size = 0x40000;
    uint32_t text_start = 0, text_end = 0;
    uint32_t data_end = 0;  // end of .bss: the MEM2 heap starts after this
    uint32_t tls_module = 0, tls_size = 0, tls_image = 0, tls_image_size = 0;
};
bool load_main(const std::string& rpx_path);
const Module& main_module();
// Storage for a data import (variable exported by an OS library); called by the HLE libraries.
void define_data_import(const char* lib, const char* name, uint32_t size, void (*init)(uint32_t addr));
uint32_t data_import_addr(const char* lib, const char* name);
}  // namespace loader
