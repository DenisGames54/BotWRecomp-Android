// Lean PM4 decoder for the native backend: interprets exactly the packet set this runtime's GX2
// emits (pm4.h) into a register file plus typed renderer calls. Runs on the GPU thread.
#pragma once
#include <stdint.h>

#include <memory>

#include "pm4.h"

namespace gpu_native {
class Decoder {
public:
    Decoder();
    void execute(uint32_t addr, uint32_t ndw);
    const uint32_t* regs() const { return m_regs.get(); }

private:
    void run(uint32_t addr, uint32_t ndw, int depth);
    void set_regs(uint32_t first, uint32_t addr_of_values, uint32_t n);
    void load_regs(uint32_t base, uint32_t addr, uint32_t n);
    std::unique_ptr<uint32_t[]> m_regs, m_shadow;  // values; guest shadow address per register (0 = none)
    uint32_t m_ctx_control0 = 0;
    bool m_predicated = false, m_skip_predicated = false;
    uint32_t m_instances = 1;
};
}  // namespace gpu_native
