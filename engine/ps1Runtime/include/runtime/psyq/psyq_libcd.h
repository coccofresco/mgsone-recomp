#pragma once
/**
 * @file psyq_libcd.h
 * @brief HLE for PsyQ libcd entries.
 *
 * Sessao 1.B coverage: CdInit / CdRead / CdSync / CdReady / CdControl /
 * CdControlF / CdGetSector / CdMix / CdReadBreak and the three callback
 * setters (CdReadCallback / CdReadyCallback / CdDataCallback).
 *
 * All entry points delegate to `cdrom::CdromController` (already wired to the
 * VirtualFs disc image) for hardware behaviour, and update the read-state /
 * callback fields on `psyq_state()` that bios.cpp's interrupt HLE reads
 * (cdRemaining / cdDestPtr / cdWordCount / cdDataCb / cdNotifyCb).  Phase 2.4
 * eliminated the corresponding BSS slots in favour of the C++ singleton.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// CdInit() -- replace the native PsyQ CdInit entirely.  Sends CdlInit to the
/// hardware controller, fires INT3+INT2 synchronously through the BIOS event
/// system (so cdSyncByte ends up at 2), and clears the read-state slots in
/// `psyq_state()`.  Returns 1 (success) -- never the "Init failed" path.
void hle_libcd_CdInit(recomp_context *ctx);

/// CdRead(sectors, *buf, mode) -- start an asynchronous read of `sectors`
/// 2048-byte sectors into `buf`.  Updates the `psyq_state()` read state and
/// issues CdlSetmode + CdlReadN to the controller.  Returns 1.
void hle_libcd_CdRead(recomp_context *ctx);

/// CdSync(mode, *result) -- wait (mode=0) or poll (mode=1) for command
/// completion.  Returns the libcd response code (CdlComplete=2 on success).
void hle_libcd_CdSync(recomp_context *ctx);

/// CdReady(mode, *result) -- wait (mode=0) or poll (mode=1) for sector data.
/// Returns the libcd response code (CdlDataReady=1 / CdlDataEnd=4 / etc.).
void hle_libcd_CdReady(recomp_context *ctx);

/// CdReadSync(mode, *result) -- wait (mode=0) or poll (mode!=0) for the
/// CdRead started earlier to finish.  Returns sectors remaining (0 = done)
/// or -1 on timeout.
void hle_libcd_CdReadSync(recomp_context *ctx);

/// CdControl(com, *param, *result) -- send a synchronous CD command.
/// Pushes the right number of parameter bytes for `com`, drains the response
/// FIFO into `*result` if non-null.  Returns 1.
void hle_libcd_CdControl(recomp_context *ctx);

/// CdControlF(com, *param) -- send a fire-and-forget CD command (no result
/// drain, no synchronous wait).  Returns 1.
void hle_libcd_CdControlF(recomp_context *ctx);

/// CdGetSector(*madr, size): copy the current sector into RAM.
/// `size` is in 32-bit words.  Returns 1 if a sector was copied, 0 otherwise.
void hle_libcd_CdGetSector(recomp_context *ctx);

/// CdReadCallback(func) -- set the "read complete" (INT4 DataEnd) callback.
/// Stored in `psyq_state().cdDataCb`; bios.cpp dispatches it for INT1/INT4.
/// Returns the previous callback function pointer.
void hle_libcd_CdReadCallback(recomp_context *ctx);

/// CdReadyCallback(func) -- set the "data ready" (INT1) callback.  Aliased to
/// the same `psyq_state().cdDataCb` slot as CdReadCallback because bios.cpp
/// dispatches a single dataCb for both INT1 and INT4.  Returns the previous
/// pointer.
void hle_libcd_CdReadyCallback(recomp_context *ctx);

/// CdDataCallback(func) -- older alias of CdReadyCallback.  Same behaviour.
void hle_libcd_CdDataCallback(recomp_context *ctx);

/// CdMix(*vol) -- set CD audio volume.  Stubbed (NOP, returns 1) until SPU
/// CD-mix is modeled.
void hle_libcd_CdMix(recomp_context *ctx);

/// CdReadBreak() -- abort an in-progress CdRead.  Stops the controller and
/// zeroes the `psyq_state()` read-state fields.  Returns 1.
void hle_libcd_CdReadBreak(recomp_context *ctx);

/// StSetMask(table, n): set the XA streaming sector filter.  XA streaming is
/// not modeled, so this is a NOP that returns 0 (success).
void hle_libcd_StSetMask(recomp_context *ctx);

void psyq_register_libcd();

} // namespace ps1::psyq
