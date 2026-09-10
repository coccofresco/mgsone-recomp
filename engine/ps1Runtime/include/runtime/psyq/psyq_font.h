#pragma once
/**
 * @file psyq_font.h
 * @brief HLE for PsyQ libgpu debug-font routines (Group 1.D).
 *
 * The PsyQ font routines (`FntOpen`, `FntPrint`, `FntFlush`, `FntLoad`,
 * `FntSystem`) live in `LIBGPU/FONT.OBJ` and dispatch through a per-stream
 * vtable in BSS that the original code initialises lazily.  When the
 * recompiler executes that init path natively against an *uninitialised*
 * GPU dispatch struct (e.g. Crash Bandicoot's `0x800549BC`) the indirect
 * branch lands on a hardware MMIO address (typically `0x1F801080`, the DMA
 * window) and the runtime aborts.
 *
 * HLE'ing these five entry points sidesteps that path entirely.  We keep
 * the rendering work minimal -- slot bookkeeping plus an optional GP0 fill
 * for the window background -- because none of the games we currently
 * target rely on the debug font for gameplay; the goal is to keep boot
 * sequences from blowing up when they call `FntOpen` for instrumentation.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// FntOpen(x, y, w, h, isbg, n) -- allocate a debug-text stream slot.
/// Returns the slot id (0..7) in `$v0`.  Writes nothing to PS1 RAM.
void hle_libgpu_FntOpen(recomp_context *ctx);

/// FntPrint(id, fmt, ...) -- append text to the slot's accumulating buffer.
/// Reads `fmt` from PS1 RAM and supports a small printf subset (`%s`, `%d`,
/// `%x`, `%c`, `%%`).  Variadic args come from `$a2..$a3` and the caller's
/// stack spill area (sp+0x10/0x14).  Returns 0 in `$v0`.
void hle_libgpu_FntPrint(recomp_context *ctx);

/// FntFlush(id) -- emit the slot's queued text via GP0.  Currently emits at
/// most a window fillrect (when `isbg=1`) and warns once that glyph
/// rasterisation is not wired up.  Returns the slot id in `$v0`.
void hle_libgpu_FntFlush(recomp_context *ctx);

/// FntLoad(tx, ty) -- record the VRAM coordinates the font texture *would*
/// be loaded at.  No actual VRAM upload is performed.
void hle_libgpu_FntLoad(recomp_context *ctx);

/// FntSystem(n) -- select the system default stream id.  Returns the
/// previous default in `$v0`.  Also registered for `SetDumpFnt` since the
/// two share semantics in PsyQ (and our handler is a strict superset).
void hle_libgpu_FntSystem(recomp_context *ctx);

/// Reset module state (slot table + system default).  Test-only hook.
void psyq_font_reset_for_tests();

/// Read-only accessor for the per-slot text buffer.  Returns an empty
/// string if `id` is out of range or the slot is unused.  Test-only hook.
const char *psyq_font_slot_buffer(int id);

/// Register every Fnt* HLE in this header into the runtime registry.
void psyq_register_libgpu_font();

} // namespace ps1::psyq
