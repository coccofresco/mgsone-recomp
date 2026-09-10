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

} // namespace

// ResetCallback starts the interrupt subsystem. Repeated calls must preserve
// installed handlers: CD, sound and graphics each initialize it independently.
// The runtime's PsyqState::reset owns clearing handlers on a machine reset.
void hle_libetc_ResetCallback(recomp_context *ctx) {
  auto &s = psyq_state();
  s.callbacksEnabled = true;
  ctx->r[V0] = 1;
}

// StopCallback / RestartCallback: gate the cooperative callback path.
// The flag is retained, but the runtime currently drains anyway --
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

// SDK CD control chooses its nonblocking path inside interrupt callbacks.
void hle_libetc_CheckCallback(recomp_context *ctx) {
  ctx->r[V0] = ctx->callbackDrainBlockDepth != 0 ? 1u : 0u;
}

// InterruptCallback(n, fn): store the per-IRQ-line callback, return prev.
// Hardware IRQ indices are distinct from DMA channels; SPU is slot 9.
// Invalid indices must not overwrite the VBlank handler.
void hle_libetc_InterruptCallback(recomp_context *ctx) {
  const std::size_t n = ctx->r[A0];
  if (n >= PsyqState::kIntrSlots) { ctx->r[V0] = 0; return; }
  auto &s = psyq_state();
  uint32_t prev = s.intrCallback[n];
  s.intrCallback[n] = ctx->r[A1];
  static constexpr const char *kMetricNames[PsyqState::kIntrSlots] = {
      "libetc.intr_callback_0", "libetc.intr_callback_1",
      "libetc.intr_callback_2", "libetc.intr_callback_3",
      "libetc.intr_callback_4", "libetc.intr_callback_5",
      "libetc.intr_callback_6", "libetc.intr_callback_7",
      "libetc.intr_callback_8", "libetc.intr_callback_9",
      "libetc.intr_callback_10"};
  ps1::metrics::setState(kMetricNames[n], ctx->r[A1]);
  ctx->r[V0] = prev;
}

void hle_libetc_DMACallback(recomp_context *ctx) {
  const std::size_t n = ctx->r[A0];
  if (n >= PsyqState::kDmaSlots) { ctx->r[V0] = 0; return; }
  auto &s = psyq_state();
  uint32_t prev = s.dmaCallback[n];
  s.dmaCallback[n] = ctx->r[A1];
  static constexpr const char *kMetricNames[PsyqState::kDmaSlots] = {
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
