// #### JO added (2026-10-17): build vtalib's HW tests (written for the current vtalib API) against the prebuilt
// etri/nest-data runtimes lib/VTA/zcu102/{v.0.7,v.0.71}, zcu102_dbg (TVM 0.7 style API).
// Those runtimes have no command-handle argument in the micro-op / push functions (checked by disassembly:
// VTAUopLoopBegin takes 4 args in w0..w3, VTAPushALUOp 4 args in x0..x3); every other function used by the
// tests (VTALoadBuffer2D, VTAStoreBuffer2D, VTADepPush/Pop, VTASynchronize, ...) has the same arguments.
// Force-included with -include old_api_shim.h; the calls below drop the first argument and go to the old symbol.
#pragma once
#include <cstdint>
#include "vta/runtime.h"

extern "C" {
void VTAUopPush_old(uint32_t mode, uint32_t reset_out, uint32_t dst_index, uint32_t src_index,
                    uint32_t wgt_index, uint32_t opcode, uint32_t use_imm, int32_t imm_val) __asm__("VTAUopPush");
void VTAUopLoopBegin_old(uint32_t extent, uint32_t dst_factor, uint32_t src_factor, uint32_t wgt_factor)
    __asm__("VTAUopLoopBegin");
void VTAUopLoopEnd_old() __asm__("VTAUopLoopEnd");
int VTAPushGEMMOp_old(void **uop_handle, int (*finit)(void *), void *signature, int nbytes) __asm__("VTAPushGEMMOp");
int VTAPushALUOp_old(void **uop_handle, int (*finit)(void *), void *signature, int nbytes) __asm__("VTAPushALUOp");
}

#define VTAUopPush(cmd, ...) VTAUopPush_old(__VA_ARGS__)
#define VTAUopLoopBegin(cmd, ...) VTAUopLoopBegin_old(__VA_ARGS__)
#define VTAUopLoopEnd(cmd) VTAUopLoopEnd_old()
#define VTAPushGEMMOp(cmd, h, f, sig, n, ...) VTAPushGEMMOp_old(h, (int (*)(void *))(f), sig, n)
#define VTAPushALUOp(cmd, h, f, sig, n) VTAPushALUOp_old(h, (int (*)(void *))(f), sig, n)
