#include "runtime/games/mgs_decomp.h"
#include "mgs_decomp/texture.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace ps1::games::mgs {
namespace {
bool enabled = false;
bool range(uint32_t p, uint32_t n) {
  const uint32_t physical = p & 0x1fffffff;
  return p && physical < Memory::RAM_SIZE && n <= Memory::RAM_SIZE - physical;
}
const uint8_t *records(const Memory &memory) {
  static_assert(std::endian::native == std::endian::little);
  return memory.ramPtr() + (UsaTextureTable & 0x1fffffff);
}
uint32_t guestRecord(uint32_t index) {
  return index == UINT32_MAX ? 0
                             : UsaTextureTable + index * sizeof(MgsDcTexture);
}
} // namespace
void configureDecompiled(bool active) noexcept {
  const char *mode = std::getenv("PS1_MGS_DECOMP");
  enabled = active && (!mode || std::string_view(mode) != "0");
}
uint32_t findSourceTexture(const Memory &memory, uint16_t id) {
  if (!id)
    return 0;
  uint32_t index;
  return mgs_dc_find_texture(records(memory), id, &index) ? guestRecord(index)
                                                          : 0;
}
bool tryDecompiledCall(recomp_context *ctx, uint32_t entry) {
  if (!enabled || !ctx || !ctx->mem || ctx->nativeResumePc)
    return false;
  auto &memory = *ctx->mem;
  const auto decline = [&] {
    metrics::count("mgs.decomp.static_fallback");
    metrics::trace("mgs.decomp.fallback",
                   {{"entry", entry}, {"arg0", ctx->r4}});
    return false;
  };
  uint32_t index = UINT32_MAX;
  const auto result = [&](uint32_t value) {
    ctx->r2 = value;
    ctx->pgxpGpr[2].invalidate();
  };
  switch (entry) {
  case 0x8001E254: { // FindTexture: pointer-to-pointer ABI
    if (ctx->r4 > 0xffff || !range(ctx->r5, 4) || (ctx->r5 & 3))
      return decline();
    const int found = mgs_dc_find_texture(records(memory), ctx->r4, &index);
    memory.write32(ctx->r5, guestRecord(index));
    result(found);
    metrics::count("mgs.decomp.FindTexture");
    break;
  }
  case 0x8001E2E8: { // DG_InitTextureSystem
    std::array<MgsDcTexture, 512> initialized;
    std::memcpy(initialized.data(), records(memory), sizeof(initialized));
    mgs_dc_init_textures(initialized.data());
    for (uint32_t i = 0; i < initialized.size(); ++i) {
      memory.write16(UsaTextureTable + i * 12, initialized[i].id);
      memory.write8(UsaTextureTable + i * 12 + 2, initialized[i].used);
    }
    metrics::count("mgs.decomp.DG_InitTextureSystem");
    break;
  }
  case 0x8001E310: { // USA missing-texture printf remains an explicit fallback.
    if (ctx->r4 > 0xffff ||
        !mgs_dc_find_texture(records(memory), ctx->r4, &index))
      return decline();
    result(guestRecord(index));
    metrics::count("mgs.decomp.DG_GetTexture");
    break;
  }
  case 0x8001E378: { // DG_SetTexture: args 5/6 live on the MIPS stack.
    if (ctx->r4 == 0 || ctx->r4 > 0xffff || ctx->r5 > 1 || ctx->r6 > 3 ||
        !range(ctx->r7, 8) || !range(ctx->r29, 24))
      return decline();
    const uint32_t paletteAddress = memory.read32(ctx->r29 + 16);
    if (!range(paletteAddress, 8))
      return decline();
    MgsDcRect img, pal;
    std::memcpy(&img, memory.ramPtr() + (ctx->r7 & 0x1fffffff), sizeof(img));
    std::memcpy(&pal, memory.ramPtr() + (paletteAddress & 0x1fffffff),
                sizeof(pal));
    if (img.x < 0 || img.y < 0 || img.w <= 0 || img.h <= 0 || pal.x < 0 ||
        pal.y < 0)
      return decline();
    const int found = mgs_dc_find_texture(records(memory), ctx->r4, &index);
    if (index == UINT32_MAX || (found && !memory.read8(guestRecord(index) + 2)))
      return decline(); // Preserve USA assert/log behavior before any mutation.
    MgsDcTexture texture;
    std::memcpy(&texture, records(memory) + index * sizeof(texture),
                sizeof(texture));
    mgs_dc_set_texture(&texture, ctx->r4, ctx->r5, ctx->r6, &img, &pal,
                       memory.read32(ctx->r29 + 20));
    const auto *bytes = reinterpret_cast<const uint8_t *>(&texture);
    for (uint32_t k = 0; k < sizeof(texture); ++k)
      memory.write8(guestRecord(index) + k, bytes[k]);
    metrics::count("mgs.decomp.DG_SetTexture");
    break;
  }
  default:
    return false;
  }
  metrics::count("mgs.decomp.calls");
  return true;
}
} // namespace ps1::games::mgs
bool mgsTryDecompiled(recomp_context *ctx, uint32_t entry) {
  return ps1::games::mgs::tryDecompiledCall(ctx, entry);
}
