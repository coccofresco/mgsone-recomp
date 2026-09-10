#include "runtime/games/mgs_profile.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <cstdint>

namespace ps1::games::mgs {
namespace {
bool wide = false;
bool enabledProfile = false;
}
void configure(bool enabled, bool widescreen) noexcept {
  enabledProfile = enabled;
  wide = enabled && widescreen;
}
bool active() noexcept { return enabledProfile; }
bool widescreen16x9() noexcept { return wide; }
} // namespace ps1::games::mgs

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
