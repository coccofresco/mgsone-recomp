#include "runtime/cpu_context.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/memory.h"
#include <array>
#include <gtest/gtest.h>
#include <limits>

TEST(MgsProjection, CompressesCameraHorizontalAxisForRotatedViews) {
  ps1::Memory memory;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &memory;
  ctx.r4 = 0x80001000;
  ps1::games::mgs::configure(true, true);
  for (const auto &row : std::array<std::array<int16_t, 3>, 3>{
           {{4096, 0, 0}, {0, 0, -4096}, {2048, -2048, 2048}}}) {
    for (unsigned i = 0; i < 3; ++i)
      memory.write16(0x1000 + i * 2, row[i]);
    memory.write16(0x1006, 4096); // original vertical row must be untouched
    memory.write32(0x1014, uint32_t(-400));
    memory.write32(0x1018, 800);
    mgsAdjustProjection(&ctx);
    // Project the same world point before/after, including translation.
    const std::array<int, 3> point{16, 8, -12};
    int64_t before = -400, after = int32_t(memory.read32(0x1014));
    for (unsigned i = 0; i < 3; ++i) {
      before += int64_t(row[i]) * point[i];
      after += int64_t(int16_t(memory.read16(0x1000 + i * 2))) * point[i];
    }
    EXPECT_EQ(after * 4, before * 3);
    EXPECT_EQ(memory.read16(0x1006), 4096u);
    EXPECT_EQ(memory.read32(0x1018), 800u);
  }
  ps1::games::mgs::configure(false, false);
}

TEST(MgsProjection, DisabledProfileIsIdentityAndTranslationDoesNotOverflow) {
  ps1::Memory memory;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &memory;
  ctx.r4 = 0x1000;
  memory.write16(0x1000, 4096);
  memory.write32(0x1014, 0x80000000);
  ps1::games::mgs::configure(false, true);
  mgsAdjustProjection(&ctx);
  EXPECT_EQ(memory.read16(0x1000), 4096u);
  EXPECT_EQ(memory.read32(0x1014), 0x80000000u);
  ps1::games::mgs::configure(true, true);
  mgsAdjustProjection(&ctx);
  EXPECT_EQ(int32_t(memory.read32(0x1014)), -1610612736);
  ps1::games::mgs::configure(false, false);
}
