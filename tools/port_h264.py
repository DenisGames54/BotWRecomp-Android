#!/usr/bin/env python3
"""Port Cemu's H.264 decoder HLE (h264.rpl) into this runtime.

usage: port_h264.py <Cemu checkout at a53cf0e> <BotWRecomp root>

Copies Cemu's h264_avc sources and the vendored ih264d decoder (Apache-2.0 / MPL-2.0) and rewrites
them for the runtime's Cemu compat layer (runtime/src/compat). Idempotent: rerun to regenerate.
"""
import os, re, shutil, sys

ref, root = sys.argv[1], sys.argv[2]
src = f"{ref}/src/Cafe/OS/libs/h264_avc"
dst = f"{root}/runtime/src/h264"
if not os.path.exists(f"{ref}/dependencies/ih264d/CMakeLists.txt"):
    sys.exit("ih264d missing in the Cemu checkout; try: git -C %s submodule update --init dependencies/ih264d" % ref)
os.makedirs(f"{dst}/parser", exist_ok=True)
shutil.copytree(f"{ref}/dependencies/ih264d", f"{root}/runtime/third_party/ih264d", dirs_exist_ok=True)


def rd(p): return open(p, encoding="utf-8-sig").read()
def wr(p, s): open(p, "w").write(s)
def sub1(pat, rep, s, flags=re.S):
    n, k = re.subn(pat, lambda m: rep, s, count=1, flags=flags)
    assert k == 1, "pattern not found: " + pat[:60]
    return n
def rep1(old, new, s):
    assert s.count(old) >= 1, "text not found: " + old[:60]
    return s.replace(old, new)


# ---- helpers the Cemu code expects -------------------------------------------------------------
wr(f"{dst}/h264_compat.h", r'''// BotWRecomp: glue for the H.264 decoder ported from Cemu (MPL-2.0).
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
''')

# ---- parser (verbatim, include fix) ------------------------------------------------------------
h = rd(f"{src}/parser/H264Parser.h")
wr(f"{dst}/parser/H264Parser.h", h.replace("#pragma once", '#pragma once\n#include "../h264_compat.h"', 1))
c = rd(f"{src}/parser/H264Parser.cpp")
wr(f"{dst}/parser/H264Parser.cpp", c.replace('#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"', '#include "H264Parser.h"'))

# ---- shared decoder interface (guest OSEvents -> host events) ---------------------------------
t = rd(f"{src}/H264DecInternal.h")
t = sub1(r'#include "util/helpers/Semaphore.h".*?#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"\n',
         '#include "h264_compat.h"\n#include "parser/H264Parser.h"\n', t)
t = sub1(r'\t\tH264DecoderBackend\(\)\n\t\t\{.*?\t\t\};\n\n\t\tvirtual ~H264DecoderBackend\(\)\n\t\t\{.*?\t\t\};\n',
         '\t\tH264DecoderBackend() = default;\n\t\tvirtual ~H264DecoderBackend() = default;\n', t)
t = rep1("coreinit::OSEvent& GetFrameOutputEvent()\n\t\t{\n\t\t\treturn *m_displayQueueEvt;",
         "H264Event& GetFrameOutputEvent()\n\t\t{\n\t\t\treturn m_displayQueueEvt;", t)
t = rep1("coreinit::OSEvent& GetFlushEvent()\n\t\t{\n\t\t\treturn *m_flushEvt;",
         "H264Event& GetFlushEvent()\n\t\t{\n\t\t\treturn m_flushEvt;", t)
t = rep1("coreinit::OSEvent* m_displayQueueEvt;", "H264Event m_displayQueueEvt;", t)
t = rep1("coreinit::OSEvent* m_flushEvt;", "H264Event m_flushEvt;", t)
wr(f"{dst}/H264DecInternal.h", t)

# ---- ih264d backend ---------------------------------------------------------------------------
b = rd(f"{src}/H264DecBackendAVC.cpp")
b = sub1(r'#include "H264DecInternal.h"\n#include "util/highresolutiontimer/HighResolutionTimer.h"\n',
         '#include "H264DecInternal.h"\n', b)
b = rep1('#include "../dependencies/ih264d/common/ih264_typedefs.h"', '#include "common/ih264_typedefs.h"', b)
b = rep1('#include "../dependencies/ih264d/decoder/ih264d.h"', '#include "decoder/ih264d.h"', b)
b = rep1("coreinit::OSSignalEvent(m_displayQueueEvt);", "m_displayQueueEvt.Signal();", b)
b = rep1("coreinit::OSSignalEvent(m_flushEvt);", "m_flushEvt.Signal();", b)
wr(f"{dst}/H264DecBackendAVC.cpp", b)

# ---- HLE layer --------------------------------------------------------------------------------
d = rd(f"{src}/H264Dec.cpp")
d = sub1(r'#include "Cafe/OS/common/OSCommon.h".*?#include "h264dec.h"\n',
         '#include "h264_compat.h"\n#include "H264DecInternal.h"\n#include "parser/H264Parser.h"\n', d)
d = sub1(r'bool H264_IsBotW\(\)\n\t\{.*?\n\t\}\n', 'bool H264_IsBotW()\n\t{\n\t\t// this runtime only runs Breath of the Wild\n\t\treturn true;\n\t}\n', d)
d = rep1("coreinit::OSEvent* flushEvt = &session->GetFlushEvent();\n\t\tcoreinit::OSResetEvent(flushEvt);\n\t\tsession->QueueFlush();\n\t\tcoreinit::OSWaitEvent(flushEvt);",
         "H264Event& flushEvt = session->GetFlushEvent();\n\t\tflushEvt.Reset();\n\t\tsession->QueueFlush();\n\t\tflushEvt.Wait();", d)
d = rep1("coreinit::OSEvent& evt = session->GetFrameOutputEvent();\n\t\t\t\tcoreinit::OSWaitEvent(&evt);",
         "H264Event& evt = session->GetFrameOutputEvent();\n\t\t\t\tevt.Wait();", d)
cb = '''void H264DoFrameOutputCallback(H264Context* ctx, H264DecoderBackend::DecodeResult& decodeResult)
	{
		if (ctx->Param.outputFunc.IsNull())
			return;
		// the callback arguments live in guest memory (Cemu used the guest stack); allocate per call
		constexpr uint32 kArrayBytes = 32;
		constexpr uint32 kTotal = kArrayBytes + sizeof(H264DECFrameOutput) + sizeof(H264OutputCBStruct) + 16;
		uint32 base = mem::sys_alloc(kTotal, 16);
		memset(memory_getPointerFromVirtualOffset(base), 0, kTotal);
		uint32 frameAddr = base + kArrayBytes;
		uint32 cbAddr = frameAddr + sizeof(H264DECFrameOutput);
		auto* resultPtrArray = (MEMPTR<void>*)memory_getPointerFromVirtualOffset(base);
		auto* frameOutput = (H264DECFrameOutput*)memory_getPointerFromVirtualOffset(frameAddr);
		auto* cbData = (H264OutputCBStruct*)memory_getPointerFromVirtualOffset(cbAddr);

		resultPtrArray[0] = MEMPTR<void>(frameAddr);
		frameOutput->imagePtr = (uint8*)decodeResult.imageOutput;
		frameOutput->result = 100;
		frameOutput->timestamp = decodeResult.timestamp;
		frameOutput->frameWidth = decodeResult.frameWidth;
		frameOutput->frameHeight = decodeResult.frameHeight;
		frameOutput->bytesPerRow = decodeResult.bytesPerRow;
		frameOutput->cropEnable = decodeResult.cropEnable;
		frameOutput->cropTop = decodeResult.cropTop;
		frameOutput->cropBottom = decodeResult.cropBottom;
		frameOutput->cropLeft = decodeResult.cropLeft;
		frameOutput->cropRight = decodeResult.cropRight;

		cbData->frameCount = 1;
		cbData->resultArray = MEMPTR<MEMPTR<H264DECFrameOutput>>(base);
		cbData->userParam = ctx->Param.userMemoryParam.GetBEValue();
		PPCCoreCallback(ctx->Param.outputFunc.GetMPTR(), cbAddr);
		mem::sys_free(base);
	}
'''
d = sub1(r'void H264DoFrameOutputCallback\(H264Context\* ctx, H264DecoderBackend::DecodeResult& decodeResult\)\n\t\{.*?\n\t\}\n(?=\n\t// find the first framedata slice)', cb, d)
reg = ["H264DECCheckMemSegmentation", "H264DECMemoryRequirement", "H264DECFindDecstartpoint", "H264DECFindIdrpoint",
       "H264DECGetImageSize", "H264DECInitParam", "H264DECOpen", "H264DECClose", "H264DECBegin", "H264DECEnd",
       "H264DECSetParam_FPTR_OUTPUT", "H264DECSetParam_OUTPUT_PER_FRAME", "H264DECSetParam_USER_MEMORY",
       "H264DECSetParam", "H264DECSetBitstream", "H264DECExecute", "H264DECCheckDecunitLength"]
d = sub1(r'\tclass : public COSModule\n.*\Z', "}  // namespace H264\n\n" +
         "".join(f"CAFE_EXPORT(h264, {n}, H264::{n});\n" for n in reg), d)
wr(f"{dst}/H264Dec.cpp", d)
print("h264 port written to", dst)
