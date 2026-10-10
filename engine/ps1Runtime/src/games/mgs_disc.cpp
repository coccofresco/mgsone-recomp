#include "runtime/games/mgs_disc.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <fmt/format.h>
#include <stdexcept>

namespace {
ps1::cdrom::VirtualFs *media = nullptr;
ps1::cdrom::CdromController *controller = nullptr;
constexpr const char *bootNames[] = {"SLUS_005.94", "SLUS_007.76"};
}

namespace ps1::games::mgs {
void configureDiscChange(cdrom::VirtualFs *fs, cdrom::CdromController *drive) {
  media = fs;
  controller = drive;
}

bool restoreLegacyDisc(std::istream &state, uint32_t,
                       cdrom::CdromController &drive) {
  if (!active() || !media || &drive != controller) return true;
  // The original filesystem's zero-based disc number is authoritative even
  // between FS_ResetCdFilePosition and the next GM_Disk update. The frozen
  // CPU/RAM prefix starts RAM at byte 476 in versions 1-7.
  uint32_t disc = UINT32_MAX;
  state.seekg(476 + (UsaFsDiskNum & 0x1fffffff));
  state.read(reinterpret_cast<char *>(&disc), sizeof(disc));
  const auto id = disc < 2 ? media->identityForBoot(bootNames[disc]) : std::string{};
  if (!state || id.empty() || !drive.restoreMedia(id)) {
    fmt::print(stderr, "[SaveState] Legacy MGS state requires unavailable disc {}\n", disc + 1);
    return false;
  }
  return true;
}
}

bool mgsTryDiscChange(recomp_context *ctx, uint32_t entry) {
  using namespace ps1;
  if (!games::mgs::active() || !media || !controller || !ctx || !ctx->mem ||
      ctx->nativeResumePc) return false;
  // Remove only the physical-disc UI, including its logo and sound cue.
  // Restored interior continuations retain their original AOT bodies.
  if (entry == 0x800C76A0) { // NewMetalGearLogo: optional cosmetic actor
    ctx->r2 = 0;
    ctx->pgxpGpr[2].invalidate();
    return true;
  }
  if (entry == 0x800C5FD0 || entry == 0x800C6E18 || entry == 0x800C7324)
    return true; // disc text / legacy logo drawing; actor lifetime stays native
  if (entry != 0x800C699C && entry != 0x800C63A0) return false;
  auto &ram = *ctx->mem;
  const uint32_t work = ctx->r4;
  if ((work & 3) || (work & 0x1fffffff) < 0x10000 ||
      (work & 0x1fffffff) > Memory::RAM_SIZE - 0x6c0 ||
      ram.read32(work + 8) != 0x800C699C)
    throw std::runtime_error("MGS disc-change actor does not match the verified USA ABI");

  if (entry == 0x800C699C) {
    // A legacy state may already own a logo. Retire it through the original
    // GV_DestroyActor protocol before eliding its fade: otherwise its callback
    // survives until FS_ResetCdFilePosition overwrites the change overlay.
    // The native scheduler still runs Die (restoring DG_FrameRate) and free.
    const uint32_t logo = ram.read32(work + 0x69c);
    const auto actorAddress = [](uint32_t p) {
      return !(p & 3) && p >= 0x80010000 && p <= 0x801fffe0;
    };
    if (actorAddress(logo) && ram.read32(logo + 8) == 0x800C7404 &&
        ram.read32(logo + 12) == 0x800C7598) {
      const uint32_t prev = ram.read32(logo), next = ram.read32(logo + 4);
      if (!actorAddress(prev) || !actorAddress(next) ||
          ram.read32(prev + 4) != logo || ram.read32(next) != logo)
        throw std::runtime_error("MGS legacy disc logo has an invalid actor chain");
      ram.write32(logo + 8, 0x80015350); // USA GV_DestroyActorQuick
      ram.write32(work + 0x69c, 0);
      metrics::count("mgs.disc.legacy_logo_retired");
    }
    // Only the logo's fade delay is elided. The original Act still executes
    // its GCL continuation and schedules its destructor after native success.
    if (ram.read32(work + 0x6ac)) ram.write32(work + 0x6a4, 71);
    return false;
  }
  const auto phase = ram.read32(work + 0x6b0);
  metrics::trace("mgs.disc.phase", {{"phase", phase},
                 {"fs_disc", ram.read32(games::mgs::UsaFsDiskNum)}});
  if (phase >= 2 && phase <= 4) {
    const auto second = media->identityForBoot(bootNames[1]);
    if (second.empty() || !controller->changeMedia(second))
      throw std::runtime_error("MGS seamless transition requires configured, readable USA Disc 2");
    // change.c states 0/1 have already set mode and stopped the drive. Replace
    // only shell-open/shell-close/Start (2-4). States 5-9 still read the retail
    // disc and rebuild FS_STAGE/FS_MOVIE tables; never synthesize FS_DiskNum,
    // GM_Disk, story progress or the GCL callback.
    ram.write32(work + 0x6a8, 0);
    ram.write32(work + 0x6b0, 5);
    fmt::print("[MGS Disc] Seamless Disc 1 -> Disc 2; original validation resumes\n");
    metrics::count("mgs.disc.automatic_switch");
  } else if (phase == 10) {
    throw std::runtime_error("MGS original disc validation rejected Disc 2");
  } else if (phase == 9 && !ram.read32(work + 0x6ac)) {
    fmt::print("[MGS Disc] Original file tables rebuilt; continuing the game\n");
    metrics::count("mgs.disc.original_validation_complete");
  }
  return false;
}
