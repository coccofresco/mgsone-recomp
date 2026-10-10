#include "runtime/cpu_context.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/memory.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/psyq/psyq_state.h"
#include <array>
#include <gtest/gtest.h>
#include <limits>

extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);
namespace {
ps1::cdrom::CdromController *fastTestDrive;
unsigned fastTestRemaining, fastTestConsumed;
unsigned fastTestScratchFreed;
bool fastTestBufferPending;
void fastLoadingDispatch(recomp_context *ctx, uint32_t address) {
  if (address == 0x800248A0u) {
    EXPECT_EQ(ctx->r4, 0x80100000u);
    EXPECT_NE(ctx->callbackDrainBlockDepth, 0u);
    if (fastTestBufferPending) {
      // Several PCX files can fit in one compressed sector. Retire each
      // synchronous upload, not just once per sector or once per frame.
      const auto before = static_cast<ps1::CPUContext>(*ctx);
      ctx->r29 = 0x80110000u;
      ctx->mem->write32(ctx->r29 + 24, 0x80120000u);
      ctx->mem->write32(0x800AE098u, fastTestConsumed & 1);
      for (unsigned i = 0; i < 3; ++i) {
        mgsReleaseFastPcxScratch(ctx);
        EXPECT_EQ(ctx->r4, before.r4);
        EXPECT_EQ(ctx->r29, 0x80110000u);
      }
      static_cast<ps1::CPUContext &>(*ctx) = before;
      ++fastTestConsumed; fastTestBufferPending = false;
    }
    ctx->r2 = fastTestRemaining != 0;
  } else if (address == 0x80023A20u) {
    ctx->r2 = fastTestRemaining;
  } else if (address == 0x80010000u) {
    EXPECT_FALSE(fastTestBufferPending) << "scratch overwritten before asset consumption";
    EXPECT_EQ(fastTestDrive->readDataByte(), (fastTestConsumed & 255));
    fastTestBufferPending = true;
    --fastTestRemaining;
    fastTestDrive->ackInterrupt(0x1f);
    fastTestDrive->clearWaitingForAck();
  } else if (address == 0x800164DCu) {
    EXPECT_EQ(ctx->r4, fastTestConsumed & 1);
    EXPECT_EQ(ctx->r5, 0x80120000u);
    EXPECT_NE(ctx->callbackDrainBlockDepth, 0u);
    ++fastTestScratchFreed;
    ctx->r4 = 0xdeadbeefu; // Callee scratch registers must be restored.
    ctx->r5 = 0;
  } else {
    ADD_FAILURE() << "unexpected extra guest work at " << std::hex << address;
  }
}
}

TEST(MgsFastLoading, InterleavesReusableScratchAndBoundsWorkWithoutAdvancingGuestTime) {
  struct DataDisc : ps1::cdrom::VirtualFs {
    bool allowsUnpacedData(uint32_t lba) const override {
      return lba < 4100 && ps1::games::mgs::fastDataAllowed("/MGS/STAGE.DIR");
    }
    std::optional<ps1::cdrom::Sector> readSector(uint32_t lba) override {
      ps1::cdrom::Sector sector{}; sector.raw[24] = lba & 255; return sector;
    }
  } fs;
  ps1::Memory memory;
  recomp_context ctx;
  ctx.reset(); ctx.mem = &memory; ctx.r4 = 0x80100000; ctx.r16 = 0x13572468;
  ps1::bios::Bios bios(ctx, fs, memory);
  ctx.bios = &bios;
  ps1::cdrom::CdromController drive;
  drive.attachVirtualFs(&fs); bios.setCdromController(&drive);
  drive.setInterruptCallback([&](uint8_t kind) { bios.queueCdromEvent(kind); });
  auto state = drive.snapshot(); state.state = ps1::cdrom::CdromState::ReadingData;
  drive.restore(state);
  auto &psyq = ps1::psyq::psyq_state();
  const auto oldCallback = psyq.intrCallback[2];
  const auto oldHook = g_testRecompDispatchHook;
  psyq.intrCallback[2] = 0x80010000;
  g_testRecompDispatchHook = fastLoadingDispatch;
  fastTestDrive = &drive; fastTestRemaining = 4100;
  fastTestConsumed = 0; fastTestBufferPending = false;
  fastTestScratchFreed = 0;
  ps1::games::mgs::configure(true, false);
  ps1::games::mgs::configureFastLoading(true, &drive);
  const auto vblank = psyq.vsyncCounter.load();
  EXPECT_TRUE(mgsTryFastStageSync(&ctx));
  EXPECT_EQ(ctx.r2, 1u);
  EXPECT_EQ(fastTestConsumed, 4096u);
  EXPECT_FALSE(fastTestBufferPending);
  EXPECT_FALSE(drive.unpacedDataRead()); // Burst permission cannot escape call.
  EXPECT_TRUE(mgsTryFastStageSync(&ctx));
  EXPECT_EQ(ctx.r2, 0u);
  EXPECT_EQ(fastTestConsumed, 4100u);
  EXPECT_EQ(fastTestScratchFreed, 4100u * 3);
  mgsReleaseFastPcxScratch(&ctx); // Other lifetimes keep retail deferred free.
  EXPECT_EQ(fastTestScratchFreed, 4100u * 3);
  EXPECT_EQ(ctx.r4, 0x80100000u);
  EXPECT_EQ(ctx.r16, 0x13572468u);
  EXPECT_EQ(ctx.callbackDrainBlockDepth, 0u);
  EXPECT_EQ(psyq.vsyncCounter.load(), vblank);
  EXPECT_EQ(drive.snapshot().readCycleCounter, 0);
  psyq.intrCallback[2] = oldCallback;
  g_testRecompDispatchHook = oldHook;
  ps1::games::mgs::configure(false, false);
}

TEST(MgsFastLoading, StageScratchBuffersRequireConsumerAndResumeStaysRetail) {
  ps1::games::mgs::configure(true, false);
  EXPECT_FALSE(ps1::games::mgs::fastDataAllowed("/MGS/STAGE.DIR"));
  EXPECT_TRUE(ps1::games::mgs::fastDataAllowed("/MGS/FACE.DAT"));
  EXPECT_TRUE(ps1::games::mgs::fastDataAllowed("/MGS/RADIO.DAT"));
  recomp_context ctx;
  ctx.reset();
  ctx.r2 = 0x12345678;
  ctx.nativeResumePc = 0x80024A78;
  EXPECT_FALSE(mgsTryFastStageSync(&ctx));
  EXPECT_EQ(ctx.r2, 0x12345678u);
  EXPECT_EQ(ctx.nativeResumePc, 0x80024A78u);
  ps1::games::mgs::configure(false, false);
}

TEST(MgsAudioClock, LetsRetailReceiverFinishBeforeAdvancingAnotherIrq) {
  ps1::Memory memory;
  constexpr uint32_t soundTask = 0x800C33B0;
  ps1::games::mgs::configure(true, false);
  memory.write8(soundTask, 0); // startup, no live receiver yet
  EXPECT_TRUE(ps1::games::mgs::audioClockReady(memory, 0x80085150));
  memory.write8(soundTask, 2); // receiving its next interrupt
  memory.write8(soundTask + 3, 0xff);
  EXPECT_TRUE(ps1::games::mgs::audioClockReady(memory, 0x80085150));
  memory.write8(soundTask, 3); // awakened/running: mts_isend would overrun
  EXPECT_FALSE(ps1::games::mgs::audioClockReady(memory, 0x80085150));
  EXPECT_EQ(memory.read32(soundTask), 0xff000003u); // read-only scheduling
  EXPECT_TRUE(ps1::games::mgs::audioClockReady(memory, 0x80010000));
  ps1::games::mgs::configure(false, false);
  EXPECT_TRUE(ps1::games::mgs::audioClockReady(memory, 0x80085150));
}

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
