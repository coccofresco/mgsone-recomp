// Tests for the libcd HLE (Sessao 1.B).
//
// Strategy: build a real Bios + CdromController + Memory; reset the PsyQ
// state singleton in SetUp; call each HLE entry and verify both the
// `psyq_state()` updates (sector counters, callbacks, sync atomics) and the
// controller-side state changes (motor on, command FIFO drained, etc.).
// Phase 2.4 retired the per-game BSS slot configuration -- every libcd HLE
// now talks to `ps1::psyq::psyq_state()`.

#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqCdTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  cdrom::CdromController cdrom;
  std::unique_ptr<bios::Bios> bios;
  std::filesystem::path discPath;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<bios::Bios>(ctx, fs, mem);
    bios->setCdromController(&cdrom);
    ctx.bios = bios.get();

    // Wire the controller's interrupt callback exactly like main_host.cpp:
    // cross-thread enqueue, drained game-thread-side via Bios::drainCdrom-
    // EventQueue (called from drainPendingCallbacks and hle_libcd_CdSync).
    // Phase 3.3 -- direct triggerCdromEvent from this lambda would race on
    // event-system state when the SDL render thread fires it concurrently.
    cdrom.setInterruptCallback(
        [this](uint8_t intType) { bios->queueCdromEvent(intType); });

    psyq::psyq_state().reset();
  }

  void TearDown() override {
    psyq::psyq_state().reset();
    if (!discPath.empty())
      std::filesystem::remove(discPath);
  }

  // Build a minimal BIN disc (unique temp file per test) with the ISO9660
  // PVD marker at LBA 16 and a recognizable pattern at LBA 17, then attach
  // it to the controller.  Mirrors the layout Crash Bandicoot's boot reads.
  void attachTestDisc() {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    discPath = std::filesystem::temp_directory_path() /
               (std::string("psyq_cd_") + info->name() + ".bin");

    std::vector<uint8_t> data(cdrom::SECTOR_SIZE_RAW * 20, 0);
    size_t pvd = 16 * cdrom::SECTOR_SIZE_RAW + 24;
    data[pvd] = 1;
    std::memcpy(&data[pvd + 1], "CD001", 5);
    size_t s17 = 17 * cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&data[s17], "SECT17!!", 8);

    std::ofstream out(discPath, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), data.size());
    out.close();

    ASSERT_TRUE(fs.loadDisc(discPath));
    cdrom.attachVirtualFs(&fs);
  }

  // CdlSetloc via the register interface (M:S:F, BCD), ACK'd like the
  // recompiled kernel would.
  void setloc(uint8_t m, uint8_t s, uint8_t f) {
    cdrom.writeRegister(0x1F801800, 0);
    cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(m));
    cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(s));
    cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(f));
    cdrom.writeRegister(0x1F801801, 0x02);
    cdrom.ackInterrupt(0x1F);
    cdrom.clearWaitingForAck();
  }
};

} // namespace

// CdInit

TEST_F(PsyqCdTest, CdInitReturnsSuccessAndDrivesStateAndController) {
  // Pre-mutate so we can prove CdInit zeroes them.
  psyq::psyq_state().cdRemaining = 0xDEADBEEFu;
  psyq::psyq_state().cdDestPtr   = 0xDEADBEEFu;
  psyq::psyq_state().cdWordCount = 0xDEADBEEFu;

  hle_libcd_CdInit(&ctx);

  // libcd CdInit returns 1 on success (the path that doesn't print
  // "Init failed" -- the whole point of the HLE replacement).
  EXPECT_EQ(ctx.r[V0], 1u);

  // BIOS event chain mapped INT3 + INT2 to psyq_state().cdSyncByte = 2.
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 2u);

  // Read-state slots cleared in psyq_state().
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0u);
  EXPECT_EQ(psyq::psyq_state().cdWordCount, 0u);
  EXPECT_FALSE(psyq::psyq_state().cdClockDemand.load());

  // Controller transitioned through CdlInit -> state is Idle, motor on.
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Idle);
  // Mode is reset to 0 by cmdInit.
  EXPECT_EQ(cdrom.getMode(), 0u);
}

TEST_F(PsyqCdTest, CdInitToleratesMissingBiosGracefully) {
  ctx.bios = nullptr;
  ctx.r[V0] = 0xCAFEu;
  hle_libcd_CdInit(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u); // hard failure when bios is unwired
}

// CdRead

TEST_F(PsyqCdTest, CdReadWritesReadStateAndIssuesCommands) {
  ctx.r[A0] = 8;             // sectors
  ctx.r[A1] = 0x80020000u;   // dest buffer
  ctx.r[A2] = 0x80;          // mode bit 7 = double-speed, bit 5 unset -> 2048 sectors

  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 8u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0x80020000u);
  EXPECT_EQ(psyq::psyq_state().cdWordCount, 512u); // 2048 bytes / 4
  EXPECT_TRUE(psyq::psyq_state().cdClockDemand.load());

  // CdlSetmode + CdlReadN landed on the controller -- check the side effects.
  EXPECT_EQ(cdrom.getMode(), 0x80u);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);
}

TEST_F(PsyqCdTest, CdReadHonoursWholeSectorMode) {
  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x20; // bit 5 set -> 2340-byte sectors

  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(psyq::psyq_state().cdWordCount, 585u); // 2340 / 4
}

TEST_F(PsyqCdTest, CdReadWithZeroSectorsPreservesRemaining) {
  // Crash Bandicoot calls libcd_CdRead twice back-to-back: first with
  // sectors=N to start the read, then with sectors=0.  If the second call
  // zeroes `cdRemaining`, the in-flight sector is discarded when INT1
  // arrives.  We preserve the prior count when sectors==0 (dest/wordcount
  // still get the fresh values from the call).
  ctx.r[A0] = 3;             // start: read 3 sectors
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 3u);

  ctx.r[A0] = 0;             // follow-up call with sectors=0
  ctx.r[A1] = 0x80030000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(psyq::psyq_state().cdRemaining, 3u) << "remaining must be preserved";
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0x80030000u)
      << "dest pointer follows the latest call";

  // CdReadBreak is the supported way to cancel a read.
  hle_libcd_CdReadBreak(&ctx);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
}

TEST_F(PsyqCdTest, CdReadWithZeroSectorsDoesNotResetCurrentLba) {
  // Re-issuing `CdlReadN` from CdRead(sectors=0) would reset the controller's
  // `currentLba_` to whatever `seekTarget_` was last set to -- Crash Bandicoot
  // does an intervening CdlSetloc with a different MSF, so re-emitting ReadN
  // would yank the in-flight read to the wrong sector.  Skip controller
  // commands entirely when sectors==0.

  // Start: Setloc to LBA 16 (ISO9660 PVD) + CdRead(1).
  cdrom.writeRegister(0x1F801800, 0);
  cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(0));   // M
  cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(2));   // S
  cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(16));  // F  -> LBA 16 (== M:S:F 0:2:16 - 150 pregap)
  cdrom.writeRegister(0x1F801801, 0x02);       // Setloc
  cdrom.ackInterrupt(0x1F);
  cdrom.clearWaitingForAck();

  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);

  // Game now issues Setloc to a different MSF (0:2:0 = LBA 0).
  cdrom.writeRegister(0x1F801800, 0);
  cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(0));
  cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(2));
  cdrom.writeRegister(0x1F801802, cdrom::CdromController::toBcd(0));
  cdrom.writeRegister(0x1F801801, 0x02);
  cdrom.ackInterrupt(0x1F);
  cdrom.clearWaitingForAck();

  // CdRead(sectors=0) follow-up.  Must NOT issue another ReadN -- that would
  // overwrite `currentLba_` with the latest seekTarget (LBA 0).
  ctx.r[A0] = 0;
  ctx.r[A1] = 0x80030000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  // Controller state should still reflect the first read.
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u);
}

TEST_F(PsyqCdTest, CdReadWithoutDataCallbackWaitsForDriveClock) {
  // CdRead itself is asynchronous even without a callback. The sector must
  // not appear until the drive receives its startup + sector cycles.
  attachTestDisc();
  setloc(0, 2, 16); // LBA 16 = ISO9660 PVD

  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u);
  EXPECT_EQ(mem.read8(0x80020000u), 0u);

  const uint32_t sectorCycles = cdrom.getCyclesPerSector();
  cdrom.tick(sectorCycles * 6u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u)
      << "seek plus first-sector latency must still be pending";
  cdrom.tick(sectorCycles / 2u);
  bios->drainCdromEventQueue();
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);

  // The PVD marker the game memcmps must already be in RAM.
  EXPECT_EQ(mem.read8(0x80020000u), 1u);
  char cd001[6] = {};
  for (int i = 0; i < 5; ++i)
    cd001[i] = static_cast<char>(mem.read8(0x80020001u + i));
  EXPECT_STREQ(cd001, "CD001");

  // INT1 still flowed through triggerCdromEvent.
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 1u);
}

TEST_F(PsyqCdTest, CdReadWithoutDataCallbackClocksMultipleSectors) {
  // Each sector needs drive time and an ACK before the next one can arrive.
  attachTestDisc();
  setloc(0, 2, 16);

  ctx.r[A0] = 2;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  const uint32_t sectorCycles = cdrom.getCyclesPerSector();
  cdrom.tick(sectorCycles * 6u + sectorCycles / 2u);
  bios->drainCdromEventQueue();
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u);
  bios->drainPendingCallbacks();
  cdrom.tick(sectorCycles);
  bios->drainCdromEventQueue();

  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr, 0x80020000u + 2u * 2048u);

  // Sector 17's payload landed right after sector 16's.
  char tag[9] = {};
  for (int i = 0; i < 8; ++i)
    tag[i] = static_cast<char>(mem.read8(0x80020800u + i));
  EXPECT_STREQ(tag, "SECT17!!");
}

TEST_F(PsyqCdTest, HostClockCreditIsConsumedOnlyByGameThreadService) {
  // Regression for Crash 3's async page streamer. The host/render thread
  // must never publish into the controller's single sector buffer: it only
  // contributes atomic clock credit. The game-thread service may consume
  // multiple matured sectors, but only as a sequence of complete
  // publish-copy-ACK transactions on that single owner.
  attachTestDisc();
  setloc(0, 2, 16);

  ctx.r[A0] = 2;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  const uint32_t sectorCycles = cdrom.getCyclesPerSector();
  cdrom.accrueClockCycles(sectorCycles * 7u + sectorCycles / 2u);

  // Accrual is intentionally side-effect-free even when enough time for the
  // seek and two sectors has elapsed.
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 2u);
  EXPECT_EQ(mem.read8(0x80020000u), 0u);

  cdrom.serviceClock();
  bios->drainCdromEventQueue();
  // This fixture intentionally queues controller IRQs instead of inline-
  // draining them as production does. The first service therefore stops at
  // the INT1 latch; after the explicit queue drain copied+ACKed sector 16,
  // the next owned service may publish sector 17.
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u);
  cdrom.serviceMaturedSector();
  bios->drainCdromEventQueue();
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr, 0x80021000u);

  char tag[9] = {};
  for (int i = 0; i < 8; ++i)
    tag[i] = static_cast<char>(mem.read8(0x80020800u + i));
  EXPECT_STREQ(tag, "SECT17!!");
}

TEST_F(PsyqCdTest, CdReadWithDataCallbackStaysAsynchronous) {
  // Games that registered a data callback consume sectors from the INT1
  // dispatch path (drainPendingCallbacks); CdRead must not eat the sectors
  // synchronously out from under it.
  attachTestDisc();
  setloc(0, 2, 16);
  psyq::psyq_state().cdDataCb = 0x80001000u;

  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u)
      << "read must stay armed for the callback path";
}

// CdSync / CdReady

TEST_F(PsyqCdTest, CdSyncPollReturnsCompleteUnconditionally) {
  // mode != 0 (poll): synchronous CD command dispatch guarantees the prior
  // command already finished, so we report Complete regardless of the atomic.
  HleConfig cfg{};
  configure(cfg);

  ctx.r[A0] = 1; // mode = poll
  ctx.r[A1] = 0; // no result struct
  hle_libcd_CdSync(&ctx);
  EXPECT_EQ(ctx.r[V0], 2u); // CdlComplete

  HleConfig empty{};
  configure(empty);
}

TEST_F(PsyqCdTest, CdSyncFillsResultStructWhenProvided) {
  HleConfig cfg{};
  configure(cfg);

  uint32_t resultPtr = 0x80100100u;
  // Pre-fill with garbage to verify CdSync overwrites all 8 bytes.
  for (int i = 0; i < 8; ++i) mem.write8(resultPtr + i, 0xAA);

  ctx.r[A0] = 1;
  ctx.r[A1] = resultPtr;
  hle_libcd_CdSync(&ctx);

  EXPECT_EQ(mem.read8(resultPtr + 0), 2u); // CdlComplete
  for (int i = 1; i < 8; ++i)
    EXPECT_EQ(mem.read8(resultPtr + i), 0u) << "byte " << i;
}

TEST_F(PsyqCdTest, CdReadyPollReturnsAtomicValue) {
  HleConfig cfg{};
  configure(cfg);

  // Polling mode reads psyq_state().cdReadyByte.
  psyq::psyq_state().cdReadyByte.store(1); // CdlDataReady
  ctx.r[A0] = 1;
  ctx.r[A1] = 0;
  hle_libcd_CdReady(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(PsyqCdTest, CdReadyPollWithZeroAtomicReturnsDataReadySentinel) {
  HleConfig cfg{};
  configure(cfg);

  // When nothing has arrived, polling returns CdlDataReady so callers that
  // poll non-blocking and then hit CdGetSector don't deadlock (matches
  // pre-2.3 behaviour preserved by hle_libcd_CdReady).
  psyq::psyq_state().cdReadyByte.store(0);
  ctx.r[A0] = 1;
  ctx.r[A1] = 0;
  hle_libcd_CdReady(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u); // CdlDataReady sentinel
}

// CdReadSync

TEST_F(PsyqCdTest, CdReadSyncReturnsZeroWhenNoReadPending) {
  HleConfig cfg{};
  configure(cfg);

  psyq::psyq_state().cdRemaining = 0;
  ctx.r[A0] = 0;
  ctx.r[A1] = 0;
  hle_libcd_CdReadSync(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqCdTest, CdReadSyncPollReturnsRemainingWithoutBlocking) {
  HleConfig cfg{};
  configure(cfg);

  psyq::psyq_state().cdRemaining = 3;
  ctx.r[A0] = 1;
  ctx.r[A1] = 0;
  hle_libcd_CdReadSync(&ctx);
  EXPECT_EQ(ctx.r[V0], 3u);
}

TEST_F(PsyqCdTest, CdReadSyncPollConsumesQueuedSectorIrq) {
  attachTestDisc();
  setloc(0, 2, 16);

  ctx.r[A0] = 1;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);

  const uint32_t sectorCycles = cdrom.getCyclesPerSector();
  cdrom.tick(sectorCycles * 6u + sectorCycles / 2u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 1u);

  ctx.r[A0] = 1; // poll
  ctx.r[A1] = 0;
  hle_libcd_CdReadSync(&ctx);

  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(mem.read8(0x80020000u), 1u);
  EXPECT_EQ(mem.read8(0x80020001u), 'C');
  EXPECT_FALSE(psyq::psyq_state().cdClockDemand.load());
}

TEST_F(PsyqCdTest, CdReadSyncBlocksUntilDrainCompletesRead) {
  // The INT1 sector copy that decrements cdRemaining runs inside callback
  // drains on the game thread; model it with a drain hook.
  HleConfig cfg{};
  cfg.drainCallbacks = [] {
    auto &st = psyq::psyq_state();
    if (st.cdRemaining > 0)
      st.cdRemaining -= 1;
  };
  configure(cfg);

  psyq::psyq_state().cdRemaining = 2;
  ctx.r[A0] = 0;
  ctx.r[A1] = 0;
  hle_libcd_CdReadSync(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
}

TEST_F(PsyqCdTest, CdReadSyncFillsResultStructWhenProvided) {
  HleConfig cfg{};
  configure(cfg);

  psyq::psyq_state().cdRemaining = 0;
  ctx.r[A0] = 0;
  ctx.r[A1] = 0x80100000;
  hle_libcd_CdReadSync(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(mem.read8(0x80100000), 2u); // CdlComplete
}

TEST_F(PsyqCdTest, TriggerCdromEventInt2WritesAtomicToComplete) {
  // Direct integration check: bios::triggerCdromEvent(2) should set
  // psyq_state().cdSyncByte to 2 (no BSS write happens -- Phase 2.3).
  psyq::psyq_state().cdSyncByte.store(0);
  bios->triggerCdromEvent(2);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 2u);
}

TEST_F(PsyqCdTest, TriggerCdromEventInt5WritesBothAtomicsToError) {
  psyq::psyq_state().cdSyncByte.store(0);
  psyq::psyq_state().cdReadyByte.store(0);
  bios->triggerCdromEvent(5);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 5u);
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 5u);
}

// CdControl / CdControlF

TEST_F(PsyqCdTest, CdControlPushesParamsAndIssuesCommand) {
  // CdlSetloc(0x02) -- 3 BCD bytes M:S:F.
  uint32_t paramPtr = 0x80100200u;
  mem.write8(paramPtr + 0, 0x00); // M
  mem.write8(paramPtr + 1, 0x05); // S = 5
  mem.write8(paramPtr + 2, 0x00); // F

  ctx.r[A0] = 0x02; // CdlSetloc
  ctx.r[A1] = paramPtr;
  ctx.r[A2] = 0;    // no result
  hle_libcd_CdControl(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(cdrom.snapshot().seekTarget, 225u); // 00:05:00 minus pregap
  // The HLE driver owns and consumes the command ACK. Leaving INT3 pending
  // would prevent the controller from publishing the next ReadN sector.
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(PsyqCdTest, CdControlFiresCommandWithoutParamsWhenParamPtrIsNull) {
  ctx.r[A0] = 0x09; // CdlPause -- 0 params
  ctx.r[A1] = 0;
  ctx.r[A2] = 0;
  hle_libcd_CdControl(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
  // cmdPause sets state_ = Paused.
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Paused);
}

TEST_F(PsyqCdTest, CdControlReturnsZeroWithoutController) {
  bios->setCdromController(nullptr);
  ctx.r[A0] = 0x01; // GetStat
  hle_libcd_CdControl(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqCdTest, CdControlFIsFireAndForget) {
  // CdlSetmode(0x0E) with one param byte.
  uint32_t paramPtr = 0x80100210u;
  mem.write8(paramPtr, 0x80); // double-speed
  ctx.r[A0] = 0x0E;
  ctx.r[A1] = paramPtr;
  hle_libcd_CdControlF(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(cdrom.getMode(), 0x80u);
}

// CdGetSector

TEST_F(PsyqCdTest, CdGetSectorReturnsZero) {
  ctx.r[V0] = 0xCAFEu;
  hle_libcd_CdGetSector(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// Callbacks

TEST_F(PsyqCdTest, CdReadCallbackStoresAndReturnsPrev) {
  psyq::psyq_state().cdDataCb = 0xDEADBEEFu;
  ctx.r[A0] = 0x80055000u;
  hle_libcd_CdReadCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0xDEADBEEFu);
  EXPECT_EQ(psyq::psyq_state().cdDataCb, 0x80055000u);
}

TEST_F(PsyqCdTest, CdReadyCallbackStoresAndReturnsPrev) {
  psyq::psyq_state().cdDataCb = 0x80055000u;
  ctx.r[A0] = 0x80056000u;
  hle_libcd_CdReadyCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80055000u);
  EXPECT_EQ(psyq::psyq_state().cdDataCb, 0x80056000u);
}

TEST_F(PsyqCdTest, CdDataCallbackIsAliasForCdReadyCallback) {
  psyq::psyq_state().cdDataCb = 0u;
  ctx.r[A0] = 0x80057000u;
  hle_libcd_CdDataCallback(&ctx);
  EXPECT_EQ(psyq::psyq_state().cdDataCb, 0x80057000u);
}

// CdMix / CdReadBreak

TEST_F(PsyqCdTest, CdMixNullVolumeReturnsOne) {
  ctx.r[V0] = 0;
  hle_libcd_CdMix(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(PsyqCdTest, CdReadBreakStopsControllerAndZeroesState) {
  // Put the controller into a reading state so we have something to break.
  ctx.r[A0] = 4;
  ctx.r[A1] = 0x80020000u;
  ctx.r[A2] = 0x80;
  hle_libcd_CdRead(&ctx);
  ASSERT_EQ(cdrom.getState(), cdrom::CdromState::ReadingData);

  hle_libcd_CdReadBreak(&ctx);

  EXPECT_EQ(ctx.r[V0], 1u);
  EXPECT_EQ(cdrom.getState(), cdrom::CdromState::Idle);
  EXPECT_EQ(psyq::psyq_state().cdRemaining, 0u);
  EXPECT_EQ(psyq::psyq_state().cdDestPtr,   0u);
  EXPECT_EQ(psyq::psyq_state().cdWordCount, 0u);
  EXPECT_FALSE(psyq::psyq_state().cdClockDemand.load());
}

// Phase 2.4 -- direct PsyqState integration

TEST_F(PsyqCdTest, TriggerVBlankStampsDrawSyncAllSlotsComplete) {
  // PsyQ DrawSync polls status == 2 for "drawing complete".  triggerVBlankEvent
  // must mirror that on every slot in psyq_state().drawSync.
  auto &draw = psyq::psyq_state().drawSync;
  draw.count = 4;
  for (auto &s : draw.status) s = 0;

  bios->triggerVBlankEvent();

  for (uint32_t i = 0; i < draw.count; ++i)
    EXPECT_EQ(draw.status[i], 2u) << "slot " << i;
  // Slots beyond count are untouched (default-constructed = 0).
  for (uint32_t i = draw.count; i < psyq::GpuDrawSync::kMaxSlots; ++i)
    EXPECT_EQ(draw.status[i], 0u) << "slot " << i;
}

TEST_F(PsyqCdTest, TriggerVBlankSkipsSwapCbWhenZero) {
  // Default psyq_state().gpuSwapCb is 0 -- VBlank must not crash queuing it.
  ASSERT_EQ(psyq::psyq_state().gpuSwapCb, 0u);
  EXPECT_NO_FATAL_FAILURE(bios->triggerVBlankEvent());
}

// Phase 3.3 -- cross-thread CDROM event queue

// queueCdromEvent must NOT touch psyq_state or the event system itself --
// those side effects only run when the game thread later drains the queue.
// This is what makes the call cross-thread-safe from the SDL render thread.
TEST_F(PsyqCdTest, QueueCdromEventDefersAllSideEffectsUntilDrain) {
  psyq::psyq_state().cdSyncByte.store(0);
  psyq::psyq_state().cdReadyByte.store(0);

  bios->queueCdromEvent(2); // INT2 Complete -> would set cdSyncByte=2 if direct
  bios->queueCdromEvent(1); // INT1 DataReady -> would set cdReadyByte=1

  // Nothing should have run yet.
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 0u);
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 0u);

  // Drain on the game thread (this thread).  Both events fire in FIFO order.
  std::size_t drained = bios->drainCdromEventQueue();
  EXPECT_EQ(drained, 2u);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(),  2u);
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 1u);

  // Re-draining is a no-op -- the queue is empty.
  EXPECT_EQ(bios->drainCdromEventQueue(), 0u);
}

// hle_libcd_CdSync must drain the IRQ queue before reading status, so a
// poll-mode caller (mode != 0) sees post-IRQ state.  Without the leading
// drain, the cdSyncByte atomic would lag the queued IRQ until the next
// drainPendingCallbacks call.
TEST_F(PsyqCdTest, CdSyncDrainsQueueBeforeReadingStatus) {
  HleConfig cfg{};
  configure(cfg);

  // Simulate a cross-thread IRQ that arrived just before the game thread
  // entered CdSync -- the controller's tick() callback would have queued
  // an INT5 (DiskError) which maps to cdSyncByte = 5.
  psyq::psyq_state().cdSyncByte.store(0);
  bios->queueCdromEvent(5);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 0u); // not drained yet

  // Poll mode (mode != 0): hle_libcd_CdSync should drain the queue and
  // return CDL_COMPLETE per the existing contract; the side effect we
  // assert is that cdSyncByte was updated to 5 by the drained INT5.
  ctx.r[A0] = 1; // poll
  ctx.r[A1] = 0; // no result struct
  hle_libcd_CdSync(&ctx);

  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 5u)
      << "CdSync must drain the queued IRQ before checking status";
}

// drainPendingCallbacks owns the same queue drain -- so any code path that
// reaches a yield point picks up cross-thread IRQs.  This is the "general
// drain" hook for sites that don't go through CdSync.
TEST_F(PsyqCdTest, DrainPendingCallbacksDrainsCdromEventQueue) {
  psyq::psyq_state().cdSyncByte.store(0);
  bios->queueCdromEvent(2);
  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 0u);

  bios->drainPendingCallbacks();

  EXPECT_EQ(psyq::psyq_state().cdSyncByte.load(), 2u);
}

// FIFO ordering is preserved -- the queue is std::queue<uint8_t> under the
// hood and drain pops front-to-back.  Use the cdReadyByte atomic which
// records the LAST INT1/INT4 to land (overwrites): if INT4 was queued
// after INT1, the atomic should reflect 4.
TEST_F(PsyqCdTest, QueuePreservesFifoOrderOnDrain) {
  psyq::psyq_state().cdReadyByte.store(0);

  bios->queueCdromEvent(1); // first -> readyByte=1
  bios->queueCdromEvent(4); // last  -> readyByte=4 (overwrites)

  bios->drainCdromEventQueue();
  EXPECT_EQ(psyq::psyq_state().cdReadyByte.load(), 4u);
}

// Registry coverage

TEST_F(PsyqCdTest, RegisterLibcdExposesAll12FunctionsViaDispatch) {
  psyq_register_libcd();

  const char *names[] = {
      "libcd_CdInit",          "libcd_CdRead",         "libcd_CdSync",
      "libcd_CdReady",         "libcd_CdReadSync",     "libcd_CdControl",
      "libcd_CdControlF",      "libcd_CdGetSector",    "libcd_CdReadCallback",
      "libcd_CdReadyCallback", "libcd_CdDataCallback", "libcd_CdMix",
      "libcd_CdReadBreak",     "libcd_StSetMask",
  };

  HleConfig cfg{};
  configure(cfg);

  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.bios = bios.get();
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx)) << "dispatch failed: " << n;
  }
}

TEST_F(PsyqCdTest, CdMixAppliesRetailMatrixWithoutAcknowledgingInterrupt) {
  cdrom.writeRegister(0x1f801800, 0);
  cdrom.writeRegister(0x1f801801, 1);
  ctx.r[A0] = 0x80011000;
  for (uint8_t i = 0; i < 4; ++i) mem.write8(ctx.r[A0] + i, i * 20 + 3);
  hle_libcd_CdMix(&ctx);
  const auto snapshot = cdrom.snapshot();
  EXPECT_EQ(snapshot.audioVolume, (std::array<uint8_t,4>{3, 23, 63, 43}));
  EXPECT_EQ(snapshot.interruptFlag, cdrom::INT_ACKNOWLEDGE);
  EXPECT_EQ(ctx.r[V0], 1u);
}
