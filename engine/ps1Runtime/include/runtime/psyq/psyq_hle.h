#pragma once
/**
 * @file psyq_hle.h
 * @brief PsyQ SDK High-Level Emulation (HLE) interface.
 *
 * When ps1Recomp identifies PsyQ SDK functions by name (via `[stubs]` in the
 * game TOML), it emits calls into this module instead of translating the MIPS.
 * This gives VSync, DrawSync, DrawOTag, and display-environment functions
 * correct behaviour without requiring per-game address hacks.
 *
 * **Setup:** call `ps1::psyq::configure()` once from `main_host.cpp` so the
 * HLE layer can reach back into the BIOS callback drain and the host GPU's
 * GP0/GP1 ports.  All per-game state lives in `psyq_state()`.
 */

#include "runtime/cpu_context.h"
#include <cstdint>
#include <functional>

namespace ps1::psyq {

// Global per-game configuration

/**
 * @brief Per-game configuration for the PsyQ HLE layer.
 *
 * Populated by `main_host.cpp` at startup and passed to `psyq::configure()`.
 * Phase 2 (2.1..2.4) moved every per-game BSS address into the
 * `psyq_state()` singleton, so the HLE config now only carries GP0/GP1
 * writers and a hook to drain pending BIOS callbacks.
 */
struct HleConfig {
  /// Drain pending BIOS callbacks (must run on game thread at yield points).
  /// Set to Bios::drainPendingCallbacks via a lambda in main_host.cpp.
  std::function<void()> drainCallbacks;

  /// GPU command port (GP0) write -- used by DrawOTag and PutDrawEnv.
  std::function<void(uint32_t)> writeGP0;

  /// GPUREAD drain for StoreImage; memory-mapped reads remain the fallback.
  std::function<uint32_t()> readGPUREAD;

  /// Ordering-table GP0 write with guest source/provenance. DrawOTag prefers
  /// this path so PGXP metadata attached to packet words is not discarded by
  /// the HLE traversal. The final three arguments are normalized physical DMA
  /// list start, node address and traversal ordinal. The next pair is the
  /// last zero-word ordering-table anchor plus an exactness bit (address zero
  /// is valid). Other HLE commands continue to use writeGP0.
  std::function<void(uint32_t, uint32_t, const PGXPValue *, uint32_t, uint32_t,
                     uint32_t, uint32_t, bool)>
      writeGP0Tracked;

  /// Bracket one complete linked-list submission. When provided as a pair,
  /// VBlank scanout cannot latch a partially consumed ordering table.
  std::function<void()> beginGP0Batch;
  std::function<void()> endGP0Batch;

  /// Diagnostic identity of the batch opened above. This lets the HLE model
  /// expose the same DMA/OT-node provenance as hardware DMA without changing
  /// command semantics.
  std::function<uint64_t()> activeGP0SubmissionId;

  /// Optional presentation observer for a complete list. The returned closure
  /// sees zero-word OT anchors while the batch owns the GPU timeline.
  std::function<std::function<void(uint32_t)>(uint32_t, const Memory &)>
      observeGP0List;

  /// GPU control port (GP1) write -- used by PutDispEnv.
  std::function<void(uint32_t)> writeGP1;

  /// Game-thread VBlank delivery (Phase 3.2).  Runs `Bios::triggerVBlankEvent`
  /// which writes non-atomic state (`drawSync.status[]`, event-system flags,
  /// queued swap callback).  The host VBlank thread no longer touches that
  /// state directly -- it just increments `psyq_state().pendingVblanks`, and
  /// `hle_VSync` consumes the count at the end of its wait, calling this
  /// callback once per pending edge on the game thread. The platform binding
  /// must finish callbacks queued by `triggerVBlankEvent` before returning:
  /// guest code after a blocking VSync observes the completed IRQ boundary,
  /// not the previous frame's callback state.
  std::function<void()> deliverVBlankEvent;

  /// Host save-state rendezvous. Called only after VSync has produced its
  /// architectural result and delivered callbacks, so r31 is a valid resume
  /// address for a fresh recomp_dispatch after a timeline load.
  std::function<void(recomp_context *, uint32_t)> stateCheckpoint;
  // Clock rendezvous at a clock query and inside a blocking HLE wait. The
  // caller cannot save a portable continuation with this HLE stack suspended.
  std::function<void(recomp_context *)> clockWaitCheckpoint;
};

/// Configure the HLE layer for the current game.  Call once from main_host
/// at startup.
void configure(const HleConfig &cfg);

/// Read-only access to the current HleConfig -- used by sibling libgpu/libcd
/// stubs that need writeGP0/writeGP1 callbacks without re-binding them.
const HleConfig &getConfig();

// PsyQ SDK HLE implementations

/// PsyQ VSync(mode): negative modes return the absolute VBlank count; mode 1
/// queries elapsed HSync time; mode 0 waits for the next generated edge and
/// n > 1 waits relative to the previous completion. Blocking modes return
/// elapsed HSync time measured between completed blocking calls.
void hle_VSync(recomp_context *ctx);

/// MMIO-backed PsyQ root-counter controls.
void hle_SetRCnt(recomp_context *ctx);
void hle_StartRCnt(recomp_context *ctx);
void hle_StopRCnt(recomp_context *ctx);

/// DrawSync(mode) -- wait for GPU drawing to complete.
/// Since the runtime GPU is fully synchronous, this always returns 0
/// immediately (mode 0) or the current command-queue depth (mode 1).
void hle_DrawSync(recomp_context *ctx);

/// ResetGraph(mode) -- reset GPU to a known state.
/// mode 0 -> flush + clear; mode 3 -> flush only.
/// Implemented as a NOP here because GPU reset is handled by the runtime.
void hle_ResetGraph(recomp_context *ctx);

/// ClearOTag(ot, n) -- fill an ordering-table with end-of-list terminators.
/// a0 = ot pointer, a1 = n (number of entries, each 4 bytes).
void hle_ClearOTag(recomp_context *ctx);

/// ClearOTagR(ot, n) -- same as ClearOTag but fills in reverse order.
void hle_ClearOTagR(recomp_context *ctx);

/// DrawOTag(ot) -- traverse ordering-table linked list and submit GP0 commands.
/// a0 = pointer to the tail of the OT (highest priority node).
/// Each node: bits[31:24]=word_count, bits[23:0]=next_ptr; followed by GP0
/// words.
void hle_DrawOTag(recomp_context *ctx);

/// SetDefDispEnv(env, x, y, w, h) -- initialise a DispEnv struct in PS1 RAM.
/// a0=*DispEnv, a1=x, a2=y, a3=w, stack+16=h
void hle_SetDefDispEnv(recomp_context *ctx);

/// PutDispEnv(env) -- apply a DispEnv to the GPU via GP1 commands.
/// a0=*DispEnv
void hle_PutDispEnv(recomp_context *ctx);

/// SetDefDrawEnv(env, x, y, w, h) -- initialise a DrawEnv struct in PS1 RAM.
/// a0=*DrawEnv, a1=x, a2=y, a3=w, stack+16=h
void hle_SetDefDrawEnv(recomp_context *ctx);

/// PutDrawEnv(env) -- apply a DrawEnv to the GPU via GP0 commands.
/// a0=*DrawEnv
void hle_PutDrawEnv(recomp_context *ctx);

/// GOOL bytecode VM entry (Crash Bandicoot, native 0x800201DC).
/// a0=*goolobj, a1=flags, a2=*goolstateref. Wired through [hle_overrides]
/// so direct JAL callers reach it. Implemented in src/gool/gool_interp.cpp.
void hle_gool_InterpretObject(recomp_context *ctx);

} // namespace ps1::psyq
