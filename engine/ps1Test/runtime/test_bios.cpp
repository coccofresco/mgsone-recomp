#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cpu_context.h"
#include "runtime/dma/dma.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/psyq/psyq_libetc.h"
#include "runtime/spu/spu.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

using namespace ps1::bios;
using namespace ps1::cdrom;
using namespace ps1;

extern void (*g_testRecompDispatchHook)(recomp_context *ctx, uint32_t addr);

namespace {
std::vector<uint32_t> g_dispatchedCallbacks;

void recordDispatchedCallback(recomp_context *, uint32_t addr) {
  g_dispatchedCallbacks.push_back(addr);
}
} // namespace

class BiosTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Set up execution context
    ctx.mem = &mem;

    fs = std::make_unique<VirtualFs>();
    bios = std::make_unique<Bios>(ctx, *fs, mem);
  }

  // Write a string to emulated memory
  void writeString(uint32_t addr, const std::string &str) {
    for (size_t i = 0; i < str.length(); ++i) {
      mem.write8(addr + i, str[i]);
    }
    mem.write8(addr + str.length(), '\0');
  }

  // Read a string from emulated memory
  std::string readString(uint32_t addr) {
    std::string str;
    char c;
    while ((c = mem.read8(addr++)) != '\0') {
      str += c;
    }
    return str;
  }

  Memory mem;
  recomp_context ctx{};
  std::unique_ptr<VirtualFs> fs;
  std::unique_ptr<Bios> bios;
};

TEST_F(BiosTest, ShutdownUnwindsPollingWithCallbacksBlockedWithoutChangingGuestMemory) {
  ctx.callbackDrainBlockDepth = 1;
  mem.write32(0x1000, 0x12345678);
  std::atomic<bool> started{false}, stopped{false};
  std::thread worker([&] {
    struct Unwound { std::atomic<bool> &flag; ~Unwound() { flag = true; } } guard{stopped};
    started = true;
    try { for (;;) bios->drainPendingCallbacks(0x80012340); }
    catch (const ShutdownRequested &) {}
  });
  while (!started) std::this_thread::yield();
  bios->requestShutdown(); worker.join();
  EXPECT_TRUE(stopped); EXPECT_EQ(mem.read32(0x1000), 0x12345678u);
}

TEST_F(BiosTest, ShutdownCrossesNativeFiberBackToRootAndRunsActiveDestructors) {
  mem.write32(0x108, 0x80001000); mem.write32(0x110, 0x80001100);
  mem.write32(0x114, 2 * 0xc0); mem.write32(0x1000, 0x80001100);
  mem.write32(0x1100, 0x4000); mem.write32(0x11c0, 0x1000);
  ctx.bios = bios.get();
  KernelThreads threads(ctx);
  struct Reset { ~Reset() { g_testRecompDispatchHook = nullptr; } } reset;
  static bool unwound; unwound = false;
  g_testRecompDispatchHook = [](recomp_context *c, uint32_t) {
    struct Guard { ~Guard() { unwound = true; } } guard;
    c->bios->requestShutdown(); c->bios->drainPendingCallbacks();
  };
  const auto handle = threads.open(0x80012340, 0x801e0000, 0x800abc00);
  EXPECT_THROW(threads.change(handle), ShutdownRequested);
  EXPECT_TRUE(unwound);
}

TEST_F(BiosTest, StrcmpReturnsCorrectly) {
  writeString(0x80000000, "apple");
  writeString(0x80000100, "apple");
  writeString(0x80000200, "banana");

  // Test equal
  ctx.r[T1] = 0x17; // strcmp A0
  ctx.r[A0] = 0x80000000;
  ctx.r[A1] = 0x80000100;
  bios->executeA0();
  EXPECT_EQ(ctx.r[V0], 0);

  // Test not equal
  ctx.r[T1] = 0x17;
  ctx.r[A0] = 0x80000000;
  ctx.r[A1] = 0x80000200;
  bios->executeA0();
  EXPECT_LT(static_cast<int32_t>(ctx.r[V0]), 0);
}

TEST_F(BiosTest, StrcpyCopiesString) {
  writeString(0x80000000, "hello world");

  ctx.r[T1] = 0x19;       // strcpy A0
  ctx.r[A0] = 0x80000100; // dest
  ctx.r[A1] = 0x80000000; // src
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000100);
  EXPECT_EQ(readString(0x80000100), "hello world");
}

TEST_F(BiosTest, StrlenCountsCorrectly) {
  writeString(0x80000000, "test length");

  ctx.r[T1] = 0x1B; // strlen A0
  ctx.r[A0] = 0x80000000;
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 11);
}

TEST_F(BiosTest, MemcpyCopiesBytes) {
  mem.write8(0x80000000, 0xAA);
  mem.write8(0x80000001, 0xBB);
  mem.write8(0x80000002, 0xCC);

  ctx.r[T1] = 0x2A;       // memcpy A0
  ctx.r[A0] = 0x80000100; // dst
  ctx.r[A1] = 0x80000000; // src
  ctx.r[A2] = 3;          // len
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000100);
  EXPECT_EQ(mem.read8(0x80000100), 0xAA);
  EXPECT_EQ(mem.read8(0x80000101), 0xBB);
  EXPECT_EQ(mem.read8(0x80000102), 0xCC);
}

TEST_F(BiosTest, MemsetFillsBytes) {
  ctx.r[T1] = 0x2B;       // memset A0
  ctx.r[A0] = 0x80000000; // dst
  ctx.r[A1] = 0x55;       // val (only lower 8 bits used)
  ctx.r[A2] = 4;          // len
  bios->executeA0();

  EXPECT_EQ(ctx.r[V0], 0x80000000);
  EXPECT_EQ(mem.read8(0x80000000), 0x55);
  EXPECT_EQ(mem.read8(0x80000001), 0x55);
  EXPECT_EQ(mem.read8(0x80000002), 0x55);
  EXPECT_EQ(mem.read8(0x80000003), 0x55);
}

TEST_F(BiosTest, VBlankInterruptCallbackDispatchesOncePerFrame) {
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[0] = 0x80055624; // Crash 3 PAL libsnd VBlank wrapper
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;

  state.vsyncCounter.store(1, std::memory_order_release);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks, (std::vector<uint32_t>{0x80055624}));

  // Re-entering a polling loop during the same frame must not double-tick.
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 1u);

  state.vsyncCounter.store(2, std::memory_order_release);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks,
            (std::vector<uint32_t>{0x80055624, 0x80055624}));

  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, VBlankBoundaryCompletesSwapCallbackBeforeReturning) {
  auto &state = psyq::psyq_state();
  state.reset();
  state.gpuSwapCb = 0x80017B88u;
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;

  bios->deliverVBlankBoundary();

  EXPECT_EQ(g_dispatchedCallbacks,
            (std::vector<uint32_t>{0x80017B88u}));
  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, RootCounterEventsPreserveEveryHardwareIrqEdge) {
  auto &state = psyq::psyq_state();
  state.reset();
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;

  // OpenEvent(RCntCNT2, EvSpINT, EvMdINTR, callback), then EnableEvent.
  ctx.r[A0] = 0xF2000002u;
  ctx.r[A1] = 0x0002u;
  ctx.r[A2] = 0x1000u;
  ctx.r[A3] = 0x8003AFD4u;
  ctx.r[T1] = 0x08u;
  bios->executeB0();
  const uint32_t eventId = ctx.r[V0];

  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x0Cu;
  bios->executeB0();
  ASSERT_EQ(ctx.r[V0], 1u);

  bios->triggerRootCounterEvents(2, 20);
  bios->drainPendingCallbacks();

  ASSERT_EQ(g_dispatchedCallbacks.size(), 20u);
  for (uint32_t callback : g_dispatchedCallbacks)
    EXPECT_EQ(callback, 0x8003AFD4u);

  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, CallbackDrainWaitsForARealGuestStack) {
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[0] = 0x80055624;
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;
  state.vsyncCounter.store(1, std::memory_order_release);

  {
    recomp_callback_drain_guard outer(&ctx);
    EXPECT_EQ(ctx.callbackDrainBlockDepth, 1u);
    {
      recomp_callback_drain_guard inner(&ctx);
      EXPECT_EQ(ctx.callbackDrainBlockDepth, 2u);
      bios->drainPendingCallbacks();
    }
    EXPECT_EQ(ctx.callbackDrainBlockDepth, 1u);
    EXPECT_TRUE(g_dispatchedCallbacks.empty());
  }

  EXPECT_EQ(ctx.callbackDrainBlockDepth, 0u);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks,
            (std::vector<uint32_t>{0x80055624}));

  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, DmaCallbacksDispatchOnceForEachCompletedTransfer) {
  DMA dma;
  dma.setMemory(&mem);
  bios->setDma(&dma);

  auto &state = psyq::psyq_state();
  state.reset();
  state.dmaCallback[DMA::SPU_CH] = 0x8004FFB8;
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;

  dma.writeRegister(0x1F8010F0, 1u << 19); // enable DMA4
  dma.writeRegister(0x1F8010F4, (1u << 23) | (1u << 20));
  dma.writeRegister(0x1F8010C0, 0x00001000);
  dma.writeRegister(0x1F8010C4, 0x00000001);
  dma.writeRegister(0x1F8010C8,
                    (1u << 28) | (1u << 24) | 1u); // one RAM -> SPU word

  // Delivery happens at the safe cooperative boundary, not recursively from
  // the CHCR write.
  EXPECT_TRUE(g_dispatchedCallbacks.empty());
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks,
            (std::vector<uint32_t>{0x8004FFB8}));

  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 1u);

  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, StrHeaderDmaDoesNotDuplicatePayloadReadyCallback) {
  DMA dma;
  cdrom::CdromController cd;
  dma.setMemory(&mem);
  dma.setCDROM(&cd);
  mem.setDMA(&dma);
  bios->setDma(&dma);
  auto sector = cd.snapshot();
  sector.sectorReady = true;
  sector.sectorSize = 2048;
  cd.restore(sector);
  auto &state = psyq::psyq_state();
  state.reset();
  ctx.r[A0] = DMA::CDROM_CH;
  ctx.r[A1] = 0x800DB530;
  psyq::hle_libetc_DMACallback(&ctx);
  EXPECT_EQ(mem.read8(0x1F8010F6), 0x88);
  dma.writeRegister(0x1F8010F0, 1u << 15);
  auto transfer = [&](uint32_t address, uint32_t words) {
    dma.writeRegister(0x1F8010B0, address);
    dma.writeRegister(0x1F8010B4, words);
    dma.writeRegister(0x1F8010B8, 0x11000000);
  };
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;
  mem.write8(0x1F8010F6, 0x80); // header is polled synchronously
  transfer(0x1000, 8);
  EXPECT_FALSE(dma.hasInterrupt());
  mem.write8(0x1F8010F6, 0x88); // payload completion publishes the ring entry
  transfer(0x2000, 504);
  EXPECT_EQ(cd.snapshot().dataReadOffset, 2048u);
  EXPECT_TRUE(dma.hasInterrupt());
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks, (std::vector<uint32_t>{0x800DB530}));
  EXPECT_FALSE(dma.hasInterrupt());
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 1u);
  ctx.r[A1] = 0;
  psyq::hle_libetc_DMACallback(&ctx);
  EXPECT_EQ(mem.read8(0x1F8010F6), 0x80);
  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, SpuInterruptRegistrationCannotAliasVblank) {
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[0] = 0x80010000;
  ctx.r[A0] = 9;
  ctx.r[A1] = 0x80085150;
  psyq::hle_libetc_InterruptCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_EQ(state.intrCallback[0], 0x80010000u);
  EXPECT_EQ(state.intrCallback[9], 0x80085150u);
  ctx.r[A0] = 11;
  ctx.r[A1] = 0x80020000;
  psyq::hle_libetc_InterruptCallback(&ctx);
  EXPECT_EQ(state.intrCallback[0], 0x80010000u);
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;
  state.vsyncCounter.store(1);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks, (std::vector<uint32_t>{0x80010000}));
  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, SpuCallbackIsDeliveredFromAudioEdgeWithoutVblank) {
  spu::SPU audio;
  audio.enableGuestClock();
  bios->setSpu(&audio);
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[9] = 0x80085150;
  auto sound = audio.snapshot();
  sound.irqPending = true;
  audio.restore(sound);
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks, (std::vector<uint32_t>{0x80085150}));
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 1u);
  EXPECT_EQ(state.vsyncCounter.load(), 0u);
  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, MgsBusySoundReceiverRetainsAudioCreditAndPendingInterrupt) {
  spu::SPU audio;
  audio.enableGuestClock();
  bios->setSpu(&audio);
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[9] = 0x80085150;
  auto sound = audio.snapshot();
  sound.irqPending = true;
  sound.clockSamplesPending = 735;
  audio.restore(sound);
  games::mgs::configure(true, false);
  mem.write8(0x800C33B0, 3); // SdInt still handling its previous wakeup
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = &recordDispatchedCallback;

  for (int i = 0; i < 3; ++i)
    bios->drainPendingCallbacks();
  EXPECT_TRUE(g_dispatchedCallbacks.empty());
  EXPECT_TRUE(audio.snapshot().irqPending);
  EXPECT_EQ(audio.snapshot().clockSamplesPending, 735u);
  EXPECT_EQ(audio.snapshot().generatedSampleCount, sound.generatedSampleCount);

  // The original receiver finishes. Deliver the held edge once, then spend
  // the retained credit; repeated cooperative polls must not duplicate it.
  mem.write8(0x800C33B0, 2);
  mem.write8(0x800C33B3, 0xff);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks, (std::vector<uint32_t>{0x80085150}));
  EXPECT_EQ(audio.snapshot().clockSamplesPending, 735u);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 1u);
  EXPECT_EQ(audio.snapshot().clockSamplesPending, 0u);
  EXPECT_EQ(audio.snapshot().generatedSampleCount, sound.generatedSampleCount + 735u);
  EXPECT_EQ(mem.read32(0x800C33B0), 0xff000002u);

  g_testRecompDispatchHook = nullptr;
  games::mgs::configure(false, false);
  state.reset();
}

TEST_F(BiosTest, ChainedDmaSlicesCompleteWithoutAdvancingVblank) {
  DMA dma;
  dma.setMemory(&mem);
  mem.setDMA(&dma);
  bios->setDma(&dma);
  auto &state = psyq::psyq_state();
  state.reset();
  state.dmaCallback[DMA::SPU_CH] = 0x8004FFB8;
  dma.writeRegister(0x1F8010F0, 1u << 19);
  dma.writeRegister(0x1F8010F4, (1u << 23) | (1u << 20));
  dma.writeRegister(0x1F8010C0, 0x1000);
  dma.writeRegister(0x1F8010C4, 1);
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = [](recomp_context *context, uint32_t address) {
    g_dispatchedCallbacks.push_back(address);
    if (g_dispatchedCallbacks.size() < 20)
      context->mem->write32(0x1F8010C8, 0x11000001);
  };
  dma.writeRegister(0x1F8010C8, 0x11000001);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 20u);
  EXPECT_EQ(state.vsyncCounter.load(), 0u);
  EXPECT_FALSE(dma.hasInterrupt());
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 20u);
  g_testRecompDispatchHook = nullptr;
  state.reset();
}

TEST_F(BiosTest, LowLevelCdDriverDrainsMaturedSectorsAndPayloadCallbacksBeforeNextVblank) {
  struct Disc : cdrom::VirtualFs {
    std::optional<cdrom::Sector> readSector(uint32_t lba) override {
      cdrom::Sector s{};
      s.raw[24] = static_cast<uint8_t>(lba + 1);
      return s;
    }
  } disc;
  CdromController cd;
  DMA dma;
  dma.setMemory(&mem);
  dma.setCDROM(&cd);
  mem.setDMA(&dma);
  bios->setDma(&dma);
  bios->setCdromController(&cd);
  cd.attachVirtualFs(&disc);
  cd.setInterruptCallback([&](uint8_t kind) { bios->queueCdromEvent(kind); });
  ctx.bios = bios.get();
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[2] = 0x80010000;
  state.dmaCallback[3] = 0x80010004;
  dma.writeRegister(0x1f8010f0, 1u << 15);
  dma.writeRegister(0x1f8010f4, (1u << 23) | (1u << 19));
  auto cs = cd.snapshot();
  cs.state = CdromState::ReadingData;
  cd.restore(cs);
  g_dispatchedCallbacks.clear();
  g_testRecompDispatchHook = [](recomp_context *ctx, uint32_t address) {
    if (address == 0x80010000) {
      auto *cd = ctx->bios->cdromController();
      g_dispatchedCallbacks.push_back(100 + cd->getSectorBuffer()[24]);
      ctx->mem->write32(0x1f8010b0, 0x1000);
      ctx->mem->write32(0x1f8010b4, 512);
      ctx->mem->write32(0x1f8010b8, 0x11000000);
      cd->writeRegister(0x1f801800, 1);
      cd->writeRegister(0x1f801803, 31);
    } else {
      g_dispatchedCallbacks.push_back(200 + ctx->mem->read8(0x1000));
    }
  };
  cd.accrueClockCycles(cd.getCyclesPerSector() * 3);
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks, (std::vector<uint32_t>{101,201,102,202,103,203}));
  EXPECT_EQ(cd.snapshot().currentLba, 3u);
  EXPECT_EQ(state.vsyncCounter.load(), 0u);
  EXPECT_FALSE(cd.hasInterrupt());
  EXPECT_FALSE(dma.hasInterrupt());
  bios->drainPendingCallbacks();
  EXPECT_EQ(g_dispatchedCallbacks.size(), 6u);
  g_testRecompDispatchHook = nullptr;
  state.reset();
}
