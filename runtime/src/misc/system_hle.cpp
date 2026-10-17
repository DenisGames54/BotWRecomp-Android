// proc_ui, sysapp, nn_act, nn_boss, nn_nfp, nn_aoc: the app is always in the foreground, one local
// account (slot 1), no network services, no amiibo, no DLC.
#include <algorithm>

#include "../hle.h"
#include "../os.h"

// ---- proc_ui -------------------------------------------------------------------------------------
HLE_EXPORT(proc_ui, ProcUIInit) {}
HLE_EXPORT(proc_ui, ProcUIShutdown) {}
HLE_EXPORT(proc_ui, ProcUIRegisterCallback) {}
HLE_EXPORT(proc_ui, ProcUIDrawDoneRelease) {}
// ProcUIStatus ProcUIProcessMessages(bool block): 0 = foreground
HLE_EXPORT(proc_ui, ProcUIProcessMessages) { RET(0); }

// ---- sysapp ----------------------------------------------------------------------------------------
// SYSGetArguments(SysArgs* out): no launch arguments
HLE_EXPORT(sysapp, SYSGetArguments) {
    if (ARG(0)) memset(mem::host(ARG(0)), 0, 0x10);
    RET(0);
}
HLE_EXPORT(sysapp, SYSLaunchMenu) {
    LOG("SYSLaunchMenu: game asked to return to the Wii U menu");
    os::request_exit(0);
}
HLE_EXPORT(sysapp, SYSLaunchSettings) { LOG("SYSLaunchSettings (ignored)"); }

// ---- nn::act (accounts) -------------------------------------------------------------------------------
HLE_EXPORT(nn_act, Initialize__Q2_2nn3actFv) { RET(0); }
HLE_EXPORT(nn_act, Finalize__Q2_2nn3actFv) { RET(0); }
HLE_EXPORT(nn_act, GetSlotNo__Q2_2nn3actFv) { RET(1); }

// ---- nn::boss (SpotPass / play reports): accept and do nothing -------------------------------------
namespace {
constexpr uint32_t kResultOk = 0;
constexpr uint32_t kBossNotRegistered = 0xA0220D00;  // error result for "task not registered"
}  // namespace
HLE_EXPORT(nn_boss, Initialize__Q2_2nn4bossFv) { RET(kResultOk); }
HLE_EXPORT(nn_boss, Finalize__Q2_2nn4bossFv) {}
// Task::Task(): zero the object, return this
HLE_EXPORT(nn_boss, __ct__Q3_2nn4boss4TaskFv) {
    memset(mem::host(ARG(0)), 0, 0x20);
    RET(ARG(0));
}
HLE_EXPORT(nn_boss, __dt__Q3_2nn4boss4TaskFv) { RET(ARG(0)); }
HLE_EXPORT(nn_boss, Initialize__Q3_2nn4boss4TaskFPCc) { RET(kResultOk); }
HLE_EXPORT(nn_boss, IsRegistered__Q3_2nn4boss4TaskCFv) { RET(0); }
HLE_EXPORT(nn_boss, Register__Q3_2nn4boss4TaskFRQ3_2nn4boss11TaskSetting) { RET(kResultOk); }
HLE_EXPORT(nn_boss, Unregister__Q3_2nn4boss4TaskFv) { RET(kResultOk); }
HLE_EXPORT(nn_boss, Run__Q3_2nn4boss4TaskFb) { RET(kResultOk); }
// uint32 Task::GetTurnState(uint32* executionCount): 0 = TASK_STOPPED
HLE_EXPORT(nn_boss, GetTurnState__Q3_2nn4boss4TaskCFPUi) {
    if (ARG(1)) mem::w32(ARG(1), 0);
    RET(0);
}
HLE_EXPORT(nn_boss, GetResult__Q3_2nn4boss4TaskCFPUi) {
    if (ARG(1)) mem::w32(ARG(1), 0);
    RET(0);
}
// TaskResult::TaskResult(nn::Result): store the result
HLE_EXPORT(nn_boss, __ct__Q3_2nn4boss10TaskResultFQ2_2nn6Result) {
    mem::w32(ARG(0), ARG(1));
    RET(ARG(0));
}
HLE_EXPORT(nn_boss, ErrorCode__Q3_2nn4boss10TaskResultCFv) { RET(0); }
HLE_EXPORT(nn_boss, __ct__Q3_2nn4boss17PlayReportSettingFv) {
    memset(mem::host(ARG(0)), 0, 0x20);
    RET(ARG(0));
}
HLE_EXPORT(nn_boss, __dt__Q3_2nn4boss17PlayReportSettingFv) { RET(ARG(0)); }
HLE_EXPORT(nn_boss, Initialize__Q3_2nn4boss17PlayReportSettingFPvUi) {}
HLE_EXPORT(nn_boss, Set__Q3_2nn4boss17PlayReportSettingFPCcUi) { RET(1); }

// ---- nn::nfp (amiibo): initialized, nothing ever detected ---------------------------------------
HLE_EXPORT(nn_nfp, Initialize__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, Finalize__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, GetNfpState__Q2_2nn3nfpFv) { RET(1); }  // NFP_STATE_INIT
HLE_EXPORT(nn_nfp, StartDetection__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, StopDetection__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, SetActivateEvent__Q2_2nn3nfpFP7OSEvent) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, SetDeactivateEvent__Q2_2nn3nfpFP7OSEvent) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, Mount__Q2_2nn3nfpFv) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, MountRom__Q2_2nn3nfpFv) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, Unmount__Q2_2nn3nfpFv) { RET(kResultOk); }
HLE_EXPORT(nn_nfp, GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetNfpRomInfo__Q2_2nn3nfpFPQ3_2nn3nfp7RomInfo) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, OpenApplicationArea__Q2_2nn3nfpFUi) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, ReadApplicationArea__Q2_2nn3nfpFPvUi) { RET(0xC0000000 | 0x1000); }
HLE_EXPORT(nn_nfp, GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result) { RET(0); }
HLE_EXPORT(nn_nfp, InitializeAmiiboSettingsArgsIn__Q2_2nn3nfpFPQ3_2nn3nfp20AmiiboSettingsArgsIn) {
    memset(mem::host(ARG(0)), 0, 0x60);
    RET(kResultOk);
}
HLE_EXPORT(nn_nfp, SwitchToAmiiboSettings__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsArgsInPCcUi) { RET(kResultOk); }

// ---- nn::aoc (DLC): no add-on content installed ---------------------------------------------------
HLE_EXPORT(nn_aoc, AOC_Initialize) { RET(0); }
HLE_EXPORT(nn_aoc, AOC_DebugRealDeviceAccess) { RET(0); }
HLE_EXPORT(nn_aoc, AOC_CalculateWorkBufferSize) { RET(0x80 + std::min(ARG(0), 256u) * 0x61); }
// AOC_ListTitle(u32* countOut, AOCTitle* list, maxCount, work, workSize)
HLE_EXPORT(nn_aoc, AOC_ListTitle) {
    if (ARG(0)) mem::w32(ARG(0), 0);
    RET(0);
}
// AOC_OpenTitle(char* pathOut, AOCTitle* title, work, workSize): path is at AOCTitle+0x0E
HLE_EXPORT(nn_aoc, AOC_OpenTitle) {
    strcpy((char*)mem::host(ARG(0)), mem::str(ARG(1) + 0x0E));
    RET(0);
}
HLE_EXPORT(nn_aoc, AOC_CloseTitle) { RET(0); }
// AOC_GetPurchaseInfo(u32* out, u64 titleId (r5:r6), u16* ids, count, work, workSize): nothing owned
HLE_EXPORT(nn_aoc, AOC_GetPurchaseInfo) {
    for (uint32_t i = 0; i < ARG(5); i++) mem::w32(ARG(0) + i * 4, 0);
    RET(0);
}

HLE_EXPORT(sysapp, SYSSwitchToEShop) { LOG("SYSSwitchToEShop (ignored)"); }
