#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/gte.h"
#include "runtime/psyq/psyq_pad.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/save_state.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

using namespace ps1;

class SaveStateTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    // Use a unique path per test instance to avoid parallel-test races.
    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(info->test_case_name()) + "_" + info->name();
    tmpPath =
        (std::filesystem::temp_directory_path() / ("ps1_test_" + name + ".sav"))
            .string();
  }
  void TearDown() override { std::remove(tmpPath.c_str()); }
};

TEST_F(SaveStateTest, SaveAndLoadPreservesMemory) {
  // Set up initial state
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  // Write test pattern to RAM
  mem.write32(0x00000000, 0xDEADBEEF);
  mem.write32(0x00000004, 0xCAFEBABE);
  mem.write8(0x00001000, 0x42);

  // Set CPU registers
  ctx.r[1] = 100;
  ctx.r[31] = 0x80010000;
  ctx.pc = 0x80020000;
  input.setPadType(0, input::PadType::Analog);
  input.press(input::BTN_CROSS | input::BTN_START, 0);
  psyq::psyq_pad_restore({0x80120000u, 0x80120100u, true});

  // Save
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  // Verify file exists
  EXPECT_TRUE(std::filesystem::exists(tmpPath));
  EXPECT_TRUE(SaveState::isValid(tmpPath));

  // Reset everything
  mem.reset();
  ctx.reset();
  ctx.nativeResumePc = 0x8000DEADu;
  ctx.nativeEntryPc = 0x8000BEEFu;
  input.reset();
  psyq::psyq_pad_reset_for_tests();

  // Verify reset worked
  EXPECT_EQ(mem.read32(0x00000000), 0u);
  EXPECT_EQ(ctx.r[1], 0u);

  // Load
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  // Verify restored state
  EXPECT_EQ(mem.read32(0x00000000), 0xDEADBEEFu);
  EXPECT_EQ(mem.read32(0x00000004), 0xCAFEBABEu);
  EXPECT_EQ(mem.read8(0x00001000), 0x42u);
  EXPECT_EQ(ctx.r[1], 100u);
  EXPECT_EQ(ctx.r[31], 0x80010000u);
  EXPECT_EQ(ctx.pc, 0x80020000u);
  EXPECT_EQ(ctx.nativeResumePc, 0u);
  EXPECT_EQ(ctx.nativeEntryPc, 0u);
  EXPECT_EQ(input.getPadType(0), input::PadType::Analog);
  EXPECT_EQ(input.buttonState(0) & (input::BTN_CROSS | input::BTN_START), 0u);
  const auto padState = psyq::psyq_pad_snapshot();
  EXPECT_EQ(padState.buf1Addr, 0x80120000u);
  EXPECT_EQ(padState.buf2Addr, 0x80120100u);
  EXPECT_TRUE(padState.active);
  psyq::psyq_pad_reset_for_tests();
}

TEST_F(SaveStateTest, LoadInvalidatesPgxpStateFromReplacedTimeline) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  constexpr uint32_t address = 0x1000;
  constexpr uint32_t value = 0x0014000A;
  mem.write32(address, value);
  ctx.r[1] = value;
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  PGXPValue precision;
  precision.x = 10.25f;
  precision.y = 20.5f;
  precision.z = 100.0f;
  precision.value = value;
  precision.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z;
  mem.storePrecision(address, value, precision);
  ctx.pgxpGpr[1] = precision;
  ctx.pgxpGteData[GTE_SXY2] = precision;
  PGXPValue found;
  ASSERT_TRUE(mem.lookupPrecision(address, value, found));

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(mem.read32(address), value);
  EXPECT_EQ(ctx.r[1], value);
  EXPECT_FALSE(mem.lookupPrecision(address, value, found));
  EXPECT_EQ(ctx.pgxpGpr[1].flags, 0u);
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY2].flags, 0u);
}

TEST_F(SaveStateTest, InvalidFileRejected) {
  // Try loading a non-existent file
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  EXPECT_FALSE(SaveState::load("/nonexistent/path.sav", ctx, mem, gpu, spu, dma,
                               cdrom, timers, irq, input));
}

TEST_F(SaveStateTest, IsValidChecks) {
  EXPECT_FALSE(SaveState::isValid("/nonexistent.sav"));

  // Create a file with wrong magic
  {
    std::ofstream f(tmpPath, std::ios::binary);
    f << "NOT_A_SAVE_STATE";
  }
  EXPECT_FALSE(SaveState::isValid(tmpPath));
}

TEST_F(SaveStateTest, SavePreservesCOP0) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  ctx.cop0[12] = 0x10000000; // SR
  ctx.cop0[13] = 0x00000400; // Cause

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));
  ctx.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(ctx.cop0[12], 0x10000000u);
  EXPECT_EQ(ctx.cop0[13], 0x00000400u);
}

TEST_F(SaveStateTest, SavePreservesIRQState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  irq.writeIMask(IRQ_VBLANK | IRQ_CDROM);
  irq.raiseInterrupt(IRQ_VBLANK);

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  irq.reset();
  EXPECT_FALSE(irq.hasPendingInterrupt());

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(irq.readIMask(), (IRQ_VBLANK | IRQ_CDROM));
  EXPECT_TRUE(irq.readIStat() & IRQ_VBLANK);
}

TEST_F(SaveStateTest, ExtendedStateRestoresStreamAndTimingState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  mdec::MDEC mdec;
  recomp_context ctx;
  ctx.reset();

  gpu.writeGP1(0x05000000u | 320u | (64u << 10u));
  gpu.writeGP1(0x06000000u | 600u | (3000u << 12u));
  timers.writeRegister(0x1F801104u, 0x0058u);
  timers.writeRegister(0x1F801108u, 1234u);
  timers.writeRegister(0x1F801100u, 777u);

  auto cdState = cdrom.snapshot();
  cdState.state = cdrom::CdromState::ReadingData;
  cdState.currentLba = 4242;
  cdState.readCycleCounter = 98765;
  cdState.waitingForAck = true;
  cdState.sectorReady = true;
  cdState.sectorSize = 2048;
  cdState.sectorBuffer[17] = 0xA5;
  cdState.responseFifo = {0x12, 0x34};
  cdrom.restore(cdState);

  auto spuState = spu.snapshot();
  spuState.voices[3].currentAddr = 0x4560;
  spuState.voices[3].adsrVolume = 0x2345;
  spuState.xaBuffer = {1, -2, 3, -4};
  spuState.xaReadPos = 2;
  spu.restore(spuState);

  mdec.writeCommand((1u << 29u) | 4u);
  mdec.writeCommand(0xFE000001u);

  auto cardState = input.getMemoryCard(0).snapshot();
  cardState.data[0x1234] = 0x6D;
  input.getMemoryCard(0).restore(cardState);

  auto &psyqState = psyq::psyq_state();
  psyqState.reset();
  psyqState.vsyncCounter.store(321, std::memory_order_release);
  psyqState.lastVSyncCounter = 320;
  psyqState.cdRemaining = 19;
  psyqState.cdDestPtr = 0x80123400;
  psyqState.gpuSwapCb = 0x80022200;

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec));

  gpu.reset();
  timers.reset();
  cdrom.reset();
  spu.reset();
  mdec.reset();
  input.getMemoryCard(0).reset();
  psyqState.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec));

  EXPECT_EQ(gpu.guestState().displayVRAMXStart, 320u);
  EXPECT_EQ(timers.readRegister(0x1F801100u), 777u);
  EXPECT_EQ(timers.readRegister(0x1F801108u), 1234u);
  const auto restoredCd = cdrom.snapshot();
  EXPECT_EQ(restoredCd.state, cdrom::CdromState::ReadingData);
  EXPECT_EQ(restoredCd.currentLba, 4242u);
  EXPECT_EQ(restoredCd.sectorBuffer[17], 0xA5u);
  EXPECT_EQ(restoredCd.responseFifo, (std::vector<uint8_t>{0x12, 0x34}));
  const auto restoredSpu = spu.snapshot();
  EXPECT_EQ(restoredSpu.voices[3].currentAddr, 0x4560u);
  EXPECT_EQ(restoredSpu.voices[3].adsrVolume, 0x2345);
  EXPECT_EQ(restoredSpu.xaBuffer, (std::vector<int16_t>{1, -2, 3, -4}));
  EXPECT_EQ(restoredSpu.xaReadPos, 2u);
  EXPECT_TRUE(mdec.isBusy());
  EXPECT_EQ(mdec.snapshot().remainingWords, 3u);
  EXPECT_EQ(input.getMemoryCard(0).snapshot().data[0x1234], 0x6Du);
  EXPECT_EQ(psyqState.vsyncCounter.load(std::memory_order_acquire), 321u);
  EXPECT_TRUE(
      psyqState.blockingVSyncPacing.load(std::memory_order_acquire));
  EXPECT_EQ(psyqState.blockingVSyncTarget.load(std::memory_order_acquire), 0u);
  EXPECT_EQ(psyqState.autonomousClockWaiters.load(std::memory_order_acquire),
            0u);
  EXPECT_EQ(psyqState.cdRemaining, 19u);
  EXPECT_TRUE(psyqState.cdClockDemand.load(std::memory_order_acquire));
  EXPECT_EQ(psyqState.cdDestPtr, 0x80123400u);
  EXPECT_EQ(psyqState.gpuSwapCb, 0x80022200u);
  psyqState.reset();
}

TEST_F(SaveStateTest, VersionFourRestoresBiosCallbackTimeline) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  cdrom::VirtualFs fs;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  mdec::MDEC mdec;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  bios::Bios biosHle(ctx, fs, mem);
  biosHle.setDma(&dma);

  auto dmaState = dma.snapshot();
  dmaState.completionGeneration[DMA::SPU_CH] = 17;
  dma.restore(dmaState);

  auto biosState = biosHle.snapshot();
  biosState.eventSystem.events.push_back(
      {0xF2000002u, 0x0002u, 0x1000u, 0x80012340u, true, false, false});
  biosState.eventSystem.triggeredBits = 1;
  biosState.eventSystem.pendingCallbacks.push_back(
      {0x80012340u, 4, 0, true, false});
  biosState.cdIntPending = 1;
  biosState.cdEventQueue = {2, 3};
  biosState.cdExceptionPending = 4;
  biosState.lastIntrTickFrame = 300;
  biosState.lastDmaCompletion[DMA::SPU_CH] = 17;
  biosState.customExceptionBuffer = 0x80010000u;
  ASSERT_TRUE(biosHle.restore(biosState));

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec, &biosHle));

  biosHle.rebaseAfterTimelineLoad();
  auto cleared = biosHle.snapshot();
  ASSERT_TRUE(cleared.eventSystem.pendingCallbacks.empty());
  ASSERT_TRUE(cleared.cdEventQueue.empty());

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec, &biosHle));
  const auto restored = biosHle.snapshot();
  ASSERT_EQ(restored.eventSystem.events.size(), 1u);
  ASSERT_EQ(restored.eventSystem.pendingCallbacks.size(), 1u);
  EXPECT_EQ(restored.eventSystem.pendingCallbacks[0].handlerPc, 0x80012340u);
  EXPECT_EQ(restored.eventSystem.triggeredBits, 1u);
  EXPECT_EQ(restored.cdIntPending, 1u);
  EXPECT_EQ(restored.cdEventQueue, (std::vector<uint8_t>{2, 3}));
  EXPECT_EQ(restored.cdExceptionPending, 4u);
  EXPECT_EQ(restored.lastIntrTickFrame, 300u);
  EXPECT_EQ(restored.lastDmaCompletion[DMA::SPU_CH], 17u);
  EXPECT_EQ(restored.customExceptionBuffer, 0x80010000u);
}

TEST(SaveStateBiosCompatibilityTest, LegacyTimelineRebaseCannotWrapDmaEpoch) {
  Memory mem;
  cdrom::VirtualFs fs;
  DMA dma;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  bios::Bios biosHle(ctx, fs, mem);
  biosHle.setDma(&dma);

  auto dmaState = dma.snapshot();
  dmaState.completionGeneration[DMA::SPU_CH] = 16;
  dma.restore(dmaState);
  auto stale = biosHle.snapshot();
  stale.lastIntrTickFrame = 4000;
  stale.lastDmaCompletion[DMA::SPU_CH] = 17;
  stale.eventSystem.pendingCallbacks.push_back(
      {0x80012340u, 0, 0, false, false});
  stale.cdEventQueue = {1};
  ASSERT_TRUE(biosHle.restore(stale));

  psyq::psyq_state().vsyncCounter.store(3000, std::memory_order_release);
  biosHle.rebaseAfterTimelineLoad();
  const auto rebased = biosHle.snapshot();
  EXPECT_EQ(rebased.lastIntrTickFrame, 3000u);
  EXPECT_EQ(rebased.lastDmaCompletion[DMA::SPU_CH], 16u);
  EXPECT_TRUE(rebased.eventSystem.pendingCallbacks.empty());
  EXPECT_TRUE(rebased.cdEventQueue.empty());
  psyq::psyq_state().reset();
}

TEST(SaveStateCoordinatorTest, ParksAtResumePcAndContinuesAfterSave) {
  SaveStateCoordinator coordinator;
  recomp_context ctx;
  ctx.reset();
  ASSERT_TRUE(
      coordinator.request(SaveStateCoordinator::Action::Save, "slot.ps1s"));
  std::atomic<bool> returned{false};
  std::thread guest([&] {
    coordinator.checkpoint(ctx, 0x80012348u);
    returned.store(true, std::memory_order_release);
  });
  while (!coordinator.service([](const auto &request) {
    return request.action == SaveStateCoordinator::Action::Save &&
           request.path == "slot.ps1s";
  }))
    std::this_thread::yield();
  guest.join();
  EXPECT_TRUE(returned.load(std::memory_order_acquire));
  EXPECT_EQ(ctx.pc, 0x80012348u);
  EXPECT_FALSE(coordinator.pending());
}

TEST(SaveStateCoordinatorTest, SuccessfulLoadUnwindsOldTimeline) {
  SaveStateCoordinator coordinator;
  recomp_context ctx;
  ctx.reset();
  ASSERT_TRUE(
      coordinator.request(SaveStateCoordinator::Action::Load, "slot.ps1s"));
  std::atomic<bool> reloaded{false};
  std::thread guest([&] {
    try {
      coordinator.checkpoint(ctx, 0x80045678u);
    } catch (const SaveStateTimelineReload &) {
      reloaded.store(true, std::memory_order_release);
    }
  });
  while (!coordinator.service([](const auto &) { return true; }))
    std::this_thread::yield();
  guest.join();
  EXPECT_TRUE(reloaded.load(std::memory_order_acquire));
  EXPECT_EQ(ctx.pc, 0x80045678u);
}

TEST_F(SaveStateTest, CorruptPayloadIsRejectedBeforeChangingLiveState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();
  ctx.r[1] = 12;
  mem.write32(0x1000, 34);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  { // A damaged final byte used to go unnoticed by the RAM-only checksum.
    std::fstream file(tmpPath, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(-1, std::ios::end);
    char byte; file.get(byte); byte ^= 1;
    file.seekp(-1, std::ios::end); file.put(byte);
  }
  ctx.r[1] = 56;
  mem.write32(0x1000, 78);
  EXPECT_FALSE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  EXPECT_EQ(ctx.r[1], 56u);
  EXPECT_EQ(mem.read32(0x1000), 78u);
  std::filesystem::resize_file(tmpPath, 100);
  EXPECT_FALSE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  EXPECT_EQ(ctx.r[1], 56u);
  EXPECT_EQ(mem.read32(0x1000), 78u);
}
