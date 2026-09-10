#pragma once
/**
 * @file psyq_libetc.h
 * @brief Interrupt and callback management HLEs from PSY-Q libetc.
 *
 * On real hardware these routines route through `struct intr* D_800B7080`
 * -- the libetc interrupt HAL (see `psyz/decomp/src/libetc/intr.c`).  The
 * struct holds function pointers for `start`/`stop`/`restart`/`set`/`cb`/
 * `unk14`, and the high-level entries (`ResetCallback`, `InterruptCallback`,
 * `DMACallback`, `VSyncCallback`, etc.) dispatch through it.
 *
 * In the recompiled runtime there is no real CPU IRQ to mask, so most of
 * this surface collapses to: store the user callback into `PsyqState`,
 * return the previous value.  Actual dispatch happens cooperatively on the
 * game thread (`hle_VSync` -> `Bios::triggerVBlankEvent` ->
 * `drainPendingCallbacks`), so registering a callback here is enough to
 * make PSY-Q's "register a 60 Hz callback" idiom work.
 *
 * Without these stubs the recompiler's permissive mode NOPs them, and the
 * game's interrupt-init sequence (`ResetCallback` -> install handlers ->
 * `VSyncCallback(my_cb)`) silently fails -- every byte of state still reads
 * zero, so callers that gate on a non-NULL return treat it as init failure
 * and never reach the main loop.  Symptom: `vsync_calls == 0` despite the
 * game running long enough to emit a handful of GPU primitives.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

// Interrupt subsystem init
void hle_libetc_ResetCallback(recomp_context *ctx);
void hle_libetc_StopCallback(recomp_context *ctx);
void hle_libetc_RestartCallback(recomp_context *ctx);
void hle_libetc_CheckCallback(recomp_context *ctx);

// Per-IRQ-line callback registration
void hle_libetc_InterruptCallback(recomp_context *ctx);
void hle_libetc_DMACallback(recomp_context *ctx);

// I_MASK (0x1F801074) round-trip
void hle_libetc_SetIntrMask(recomp_context *ctx);
void hle_libetc_GetIntrMask(recomp_context *ctx);

// Low-level entries used internally by ResetCallback / Stop / Restart.
// PSY-Q exposes them; some games call them directly.  Same semantics as the
// matching high-level entry -- we collapse the indirection.
void hle_libetc_startIntr(recomp_context *ctx);
void hle_libetc_stopIntr(recomp_context *ctx);
void hle_libetc_restartIntr(recomp_context *ctx);

/// Register every libetc HLE in this header into the runtime registry.
/// Called from the boot-path registration in `psyq_registry.cpp`.
void psyq_register_libetc_intr();

} // namespace ps1::psyq
