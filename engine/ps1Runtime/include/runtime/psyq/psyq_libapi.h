#pragma once
/**
 * @file psyq_libapi.h
 * @brief HLE for PsyQ libapi entries detected on the boot path (Sessao 0.7).
 *
 * Most libapi routines are thin wrappers around BIOS A0/B0/C0 syscalls.  Each
 * HLE here loads the syscall index into `ctx->r[T1]` and invokes the matching
 * `Bios::executeA0/B0/C0`, so v0 / a-args propagate exactly as if the original
 * MIPS wrapper had run.  The few libapi routines that issue MIPS `syscall`
 * (Enter/ExitCriticalSection) are stubbed locally -- they manipulate kernel
 * state we do not model.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

// Event manager (B0)
void hle_libapi_OpenEvent(recomp_context *ctx);
void hle_libapi_CloseEvent(recomp_context *ctx);
void hle_libapi_DeliverEvent(recomp_context *ctx);
void hle_libapi_EnableEvent(recomp_context *ctx);
void hle_libapi_DisableEvent(recomp_context *ctx);
void hle_libapi_TestEvent(recomp_context *ctx);

// Critical section (SYSCALL -- stubbed)
void hle_libapi_EnterCriticalSection(recomp_context *ctx);
void hle_libapi_ExitCriticalSection(recomp_context *ctx);

// Exception/IRQ chain
void hle_libapi_ReturnFromException(recomp_context *ctx);
void hle_libapi_HookEntryInt(recomp_context *ctx);
void hle_libapi_ChangeClearRCnt(recomp_context *ctx);

// File / Memory Card I/O (B0)
void hle_libapi_open(recomp_context *ctx);
void hle_libapi_close(recomp_context *ctx);
void hle_libapi_read(recomp_context *ctx);
void hle_libapi_write(recomp_context *ctx);
void hle_libapi_erase(recomp_context *ctx);
void hle_libapi_format(recomp_context *ctx);
void hle_libapi__bu_init(recomp_context *ctx);
void hle_libapi_firstfile2(recomp_context *ctx);
void hle_libapi_nextfile(recomp_context *ctx);
void hle_libapi__96_remove(recomp_context *ctx);

// Pad (B0)
void hle_libapi_InitPAD2(recomp_context *ctx);
void hle_libapi_StartPAD2(recomp_context *ctx);
void hle_libapi_StopPAD2(recomp_context *ctx);
void hle_libapi_ChangeClearPAD(recomp_context *ctx);

// Heap (A0)
void hle_libapi_InitHeap(recomp_context *ctx);

// GPU command (A0)
void hle_libapi_GPU_cw(recomp_context *ctx);

/// Register every libapi HLE in this header into the runtime registry.
/// Called from `psyq_register_rayman_boot()` (and any other registration point
/// that needs libapi coverage).  Idempotent across repeated calls -- the global
/// registry simply overwrites the slot.
void psyq_register_libapi();

} // namespace ps1::psyq
