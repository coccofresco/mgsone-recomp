#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/dma/dma.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_state.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

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
