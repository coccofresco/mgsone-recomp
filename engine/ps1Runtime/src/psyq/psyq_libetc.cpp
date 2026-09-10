#include "runtime/psyq/psyq_libetc.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <cstdint>

namespace ps1::psyq {

namespace {

// I_MASK / I_STAT live in the PS1 IO segment.  Real address is 0x1F801074;
// our memory_init / runtime model exposes it as a normal read/write target.
constexpr uint32_t kIMaskAddr = 0x1F801074;

// PSY-Q caps `n` for InterruptCallback/DMACallback at 8 slots.  Matches our
// PsyqState arrays (kIntrSlots).
inline std::size_t clampSlot(uint32_t n) {
  return (n < PsyqState::kIntrSlots) ? static_cast<std::size_t>(n) : 0;
}

} // namespace

// ResetCallback: re-initialises the interrupt subsystem.  Real PSY-Q
// allocates state, installs entry-point handlers and returns the address
// of an internal sentinel ("non-zero == initialised").  We have no IRQs
// to install -- callbacks are drained cooperatively from `hle_VSync` -- so
// reset the soft state and hand back a non-zero token.  A non-zero return
// is load-bearing: PSY-Q boilerplate checks it to detect init failure and
// aborts the main loop when it sees zero.
void hle_libetc_ResetCallback(recomp_context *ctx) {
  auto &s = psyq_state();
  for (auto &cb : s.intrCallback) cb = 0;
  for (auto &cb : s.dmaCallback)  cb = 0;
  s.callbacksEnabled = true;
  ctx->r[V0] = 1;
}

// StopCallback / RestartCallback: gate the cooperative callback path.
// We honour the flag for `CheckCallback` but the runtime drains anyway --
// real IRQ masking would suppress dispatch, which we cannot model without
// a full COP0 emulation.  Returning the previous state matches PSY-Q.
void hle_libetc_StopCallback(recomp_context *ctx) {
  auto &s = psyq_state();
  uint32_t prev = s.callbacksEnabled ? 1u : 0u;
  s.callbacksEnabled = false;
  ctx->r[V0] = prev;
}

void hle_libetc_RestartCallback(recomp_context *ctx) {
  auto &s = psyq_state();
  uint32_t prev = s.callbacksEnabled ? 1u : 0u;
  s.callbacksEnabled = true;
  ctx->r[V0] = prev;
}

// CheckCallback: returns non-zero iff currently executing inside a
// callback context.  In the cooperative model we are never inside one
// from the game thread's perspective.  Some PSY-Q routines branch on
// this to choose between sync and async paths; returning 0 selects the
// sync path which matches our runtime.
void hle_libetc_CheckCallback(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

// InterruptCallback(n, fn): store the per-IRQ-line callback, return prev.
// Slot 0..7 follow the PSY-Q convention (VBlank, GPU, CDROM, DMA, RTC0/1/2,
// reserved).  Bios::drainPendingCallbacks cooperatively dispatches VBlank
// and root-counter slots once per host VBlank.
void hle_libetc_InterruptCallback(recomp_context *ctx) {
  std::size_t n = clampSlot(ctx->r[A0]);
  auto &s = psyq_state();
  uint32_t prev = s.intrCallback[n];
  s.intrCallback[n] = ctx->r[A1];
  static constexpr const char *kMetricNames[PsyqState::kIntrSlots] = {
      "libetc.intr_callback_0", "libetc.intr_callback_1",
      "libetc.intr_callback_2", "libetc.intr_callback_3",
      "libetc.intr_callback_4", "libetc.intr_callback_5",
      "libetc.intr_callback_6", "libetc.intr_callback_7"};
  ps1::metrics::setState(kMetricNames[n], ctx->r[A1]);
  ctx->r[V0] = prev;
}

void hle_libetc_DMACallback(recomp_context *ctx) {
  std::size_t n = clampSlot(ctx->r[A0]);
  auto &s = psyq_state();
  uint32_t prev = s.dmaCallback[n];
  s.dmaCallback[n] = ctx->r[A1];
  static constexpr const char *kMetricNames[PsyqState::kIntrSlots] = {
      "libetc.dma_callback_0", "libetc.dma_callback_1",
      "libetc.dma_callback_2", "libetc.dma_callback_3",
      "libetc.dma_callback_4", "libetc.dma_callback_5",
      "libetc.dma_callback_6", "libetc.dma_callback_7"};
  ps1::metrics::setState(kMetricNames[n], ctx->r[A1]);
  ctx->r[V0] = prev;
}

// SetIntrMask / GetIntrMask: round-trip the I_MASK hardware register.
// Real silicon (0x1F801074) gates which IRQs the CPU sees; we maintain
// a software mirror in PsyqState plus a write-through to the IO address
// so other subsystems (cdrom_controller, dma) read the same value the
// game wrote.
void hle_libetc_SetIntrMask(recomp_context *ctx) {
  uint32_t mask = ctx->r[A0] & 0xFFFFu;
  auto &s = psyq_state();
  uint32_t prev = s.intrMask;
  s.intrMask = static_cast<uint16_t>(mask);
  ctx->mem->write32(kIMaskAddr, mask);
  ctx->r[V0] = prev;
}

void hle_libetc_GetIntrMask(recomp_context *ctx) {
  ctx->r[V0] = psyq_state().intrMask;
}

// startIntr / stopIntr / restartIntr: low-level entries that PSY-Q's
// ResetCallback / StopCallback / RestartCallback delegate to via the
// `D_800B7080->start/stop/restart` indirection.  Since the runtime has
// no IRQs to actually start/stop, these have the same observable effect
// as their high-level counterparts.  Aliasing keeps direct callers
// honest without duplicating logic.
void hle_libetc_startIntr(recomp_context *ctx)   { hle_libetc_ResetCallback(ctx); }
void hle_libetc_stopIntr(recomp_context *ctx)    { hle_libetc_StopCallback(ctx); }
void hle_libetc_restartIntr(recomp_context *ctx) { hle_libetc_RestartCallback(ctx); }

void psyq_register_libetc_intr() {
  psyq_register("libetc_ResetCallback",     &hle_libetc_ResetCallback);
  psyq_register("libetc_StopCallback",      &hle_libetc_StopCallback);
  psyq_register("libetc_RestartCallback",   &hle_libetc_RestartCallback);
  psyq_register("libetc_CheckCallback",     &hle_libetc_CheckCallback);
  psyq_register("libetc_InterruptCallback", &hle_libetc_InterruptCallback);
  psyq_register("libetc_DMACallback",       &hle_libetc_DMACallback);
  psyq_register("libetc_SetIntrMask",       &hle_libetc_SetIntrMask);
  psyq_register("libetc_GetIntrMask",       &hle_libetc_GetIntrMask);
  psyq_register("libetc_startIntr",         &hle_libetc_startIntr);
  psyq_register("libetc_stopIntr",          &hle_libetc_stopIntr);
  psyq_register("libetc_restartIntr",       &hle_libetc_restartIntr);
}

} // namespace ps1::psyq
