#include "runtime/games/mgs_profile.h"
#include "runtime/cpu_context.h"
#include "runtime/games/mgs_decomp.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include <cstdint>

extern void recomp_dispatch(uint8_t *, recomp_context *, uint32_t);

namespace ps1::games::mgs {
namespace {
bool wide = false;
bool enabledProfile = false;
bool fastLoading = false;
bool stageConsumer = false;
cdrom::CdromController *dataDrive = nullptr;
}
void configure(bool enabled, bool widescreen) noexcept {
  enabledProfile = enabled;
  wide = enabled && widescreen;
  fastLoading = false;
  stageConsumer = false;
  dataDrive = nullptr;
  configureDecompiled(enabled);
}
bool active() noexcept { return enabledProfile; }
bool widescreen16x9() noexcept { return wide; }
void configureFastLoading(bool enabled, cdrom::CdromController *drive) noexcept {
  fastLoading = enabledProfile && enabled;
  dataDrive = drive;
}
bool fastDataAllowed(std::string_view path) noexcept {
  // STAGE reuses scratch buffers for resident/nocache DAR sections. Only the
  // interleaved retail consumer below may remove its sector deadlines.
  return path != "/MGS/STAGE.DIR" || stageConsumer;
}
bool audioClockReady(const Memory &memory, uint32_t irqCallback) noexcept {
  if (!enabledProfile || irqCallback != 0x80085150u)
    return true;
  // USA mts_isend (8008C590) addresses mts_tasks at 800C3390 with a
  // 32-byte stride. SOUND_INT=1, state=3 means READY/running. Another
  // interrupt in this state is deliberately discarded by the retail MTS
  // receiver (src=-1); it is not a queued notification. Native cooperative
  // polls must therefore let SdInt finish before spending more SPU credit.
  // Otherwise the playback cursor advances but str_tick_count loses ticks,
  // eventually deadlocking audio against subtitles in the shared FS ring.
  return memory.read8(0x800C33B0u) != 3;
}
} // namespace ps1::games::mgs

void mgsReleaseFastPcxScratch(recomp_context *ctx) {
  using namespace ps1::games::mgs;
  if (!stageConsumer || !ctx || !ctx->mem) return;
  // Verified USA DG_LoadInitPcx boundary, after both synchronous LoadImage
  // uploads and GV_FreeMemory2. The latter only marks this allocation VOID:
  // retail normally reclaims it on the next frame. A whole-stage burst can
  // otherwise exhaust the packet heap and silently skip later PCX files.
  // Release only this completed upload, through the original GV_FreeMemory;
  // do not clear whole heaps, move live packets, or advance game time.
  const auto saved = static_cast<ps1::CPUContext>(*ctx);
  const uint32_t heap = ctx->mem->read32(0x800AE098u); // GV_Clock
  if (heap > 1) return;
  const uint32_t allocation = ctx->mem->read32(ctx->r29 + 24);
  ctx->r4 = heap;
  ctx->r5 = allocation;
  recomp_dispatch(ctx->mem->ramPtr(), ctx, 0x800164DCu); // GV_FreeMemory
  static_cast<ps1::CPUContext &>(*ctx) = saved;
  ps1::metrics::count("mgs.fast_loading.pcx_scratch_reclaimed");
}

bool mgsTryFastStageSync(recomp_context *ctx) {
  using namespace ps1::games::mgs;
  if (!fastLoading || !dataDrive || !ctx || !ctx->bios || !ctx->mem ||
      ctx->nativeResumePc != 0 || ctx->callbackDrainBlockDepth || stageConsumer)
    return false;
  // USA FS_LoadStageSync(info), verified at generation against the retail EXE.
  // Keep the original LoadStageFiles and CDBIOS_ReadSync implementations.
  // All IRQs/DMA and the asset consumer run on this game thread. No actor,
  // simulation tick, audio clock or script is run an extra time.
  const auto saved = static_cast<ps1::CPUContext>(*ctx);
  const uint32_t info = ctx->r4;
  const auto call = [&](uint32_t address) {
    static_cast<ps1::CPUContext &>(*ctx) = saved;
    ctx->r4 = info;
    recomp_callback_drain_guard guard(ctx);
    recomp_dispatch(ctx->mem->ramPtr(), ctx, address);
    return ctx->r2;
  };
  stageConsumer = true;
  uint32_t loading = 1;
  unsigned sectors = 0;
  for (; sectors < 4096; ++sectors) {
    loading = call(0x800248A0u); // LoadStageFiles: consume before scratch reuse.
    if (!ctx->bios->serviceUnpacedDataSector()) break;
  }
  if (sectors == 4096) loading = call(0x800248A0u);
  stageConsumer = false;
  const auto remaining = static_cast<int32_t>(call(0x80023A20u));
  static_cast<ps1::CPUContext &>(*ctx) = saved;
  ctx->r2 = loading != 0 || remaining > 0;
  ps1::metrics::count("mgs.fast_loading.stage_sector", sectors);
  return true;
}

void mgsAdjustProjection(recomp_context *ctx) {
  if (!ps1::games::mgs::widescreen16x9())
    return;
  // DG_AdjustOverscan receives a freshly composed render matrix. Compress
  // its entire horizontal row AND translation, preserving the vertical FOV
  // for every camera orientation. Forcing one camera basis element to 3072
  // (the published AR patch) only works for some camera angles.
  const uint32_t matrix = ctx->r4;
  for (uint32_t offset : {0u, 2u, 4u}) {
    const int32_t value =
        static_cast<int16_t>(ctx->mem->read16(matrix + offset));
    ctx->mem->write16(matrix + offset, static_cast<uint16_t>(value * 3 / 4));
  }
  const int64_t translation =
      static_cast<int32_t>(ctx->mem->read32(matrix + 20));
  ctx->mem->write32(matrix + 20, static_cast<uint32_t>(translation * 3 / 4));
  ps1::metrics::count("mgs.widescreen.render_matrix");
}
