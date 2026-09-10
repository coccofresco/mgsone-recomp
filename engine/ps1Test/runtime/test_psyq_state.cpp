// Tests for PsyqState (Phase 2, Session 2.1).
//
// Scope is intentionally small: this session only introduces the struct +
// singleton accessor.  Behaviour migration (HLE sites reading/writing the
// new fields instead of BSS) lands in Sessions 2.2 .. 2.5.
//
// We verify:
//   - psyq_state() returns the same instance across calls
//   - default-constructed values are zeroed (and drawSync.count == 2)
//   - reset() restores defaults after mutation
//   - atomic fields tolerate concurrent fetch_add / load

#include "runtime/psyq/psyq_state.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using ps1::psyq::psyq_state;
using ps1::psyq::PsyqState;
using ps1::psyq::GpuDrawSync;

namespace {

class PsyqStateTest : public ::testing::Test {
protected:
  void SetUp() override { psyq_state().reset(); }
  void TearDown() override { psyq_state().reset(); }
};

TEST_F(PsyqStateTest, SingletonReturnsSameInstance) {
  PsyqState &a = psyq_state();
  PsyqState &b = psyq_state();
  EXPECT_EQ(&a, &b);
}

TEST_F(PsyqStateTest, DefaultsAreZeroExceptDrawSyncCount) {
  PsyqState &s = psyq_state();
  EXPECT_EQ(s.vsyncCounter.load(), 0u);
  EXPECT_EQ(s.pendingVblanks.load(), 0u);
  EXPECT_FALSE(s.blockingVSyncPacing.load());
  EXPECT_EQ(s.blockingVSyncTarget.load(), 0u);
  EXPECT_EQ(s.autonomousClockWaiters.load(), 0u);
  EXPECT_FALSE(s.cdClockDemand.load());
  EXPECT_EQ(s.cdSyncByte.load(), 0u);
  EXPECT_EQ(s.cdReadyByte.load(), 0u);
  EXPECT_EQ(s.cdRemaining, 0u);
  EXPECT_EQ(s.cdDestPtr, 0u);
  EXPECT_EQ(s.cdWordCount, 0u);
  EXPECT_EQ(s.cdDataCb, 0u);
  EXPECT_EQ(s.cdNotifyCb, 0u);
  EXPECT_EQ(s.gpuSwapCb, 0u);

  EXPECT_EQ(s.drawSync.index, 0u);
  EXPECT_EQ(s.drawSync.count, 2u);
  for (std::size_t i = 0; i < GpuDrawSync::kMaxSlots; ++i) {
    EXPECT_EQ(s.drawSync.status[i], 0u) << "slot " << i;
  }
}

TEST_F(PsyqStateTest, ResetRestoresDefaultsAfterMutation) {
  PsyqState &s = psyq_state();
  s.vsyncCounter.store(123);
  s.pendingVblanks.store(3);
  s.blockingVSyncPacing.store(true);
  s.blockingVSyncTarget.store(124);
  s.autonomousClockWaiters.store(2);
  s.cdClockDemand.store(true);
  s.cdSyncByte.store(0xAB);
  s.cdReadyByte.store(0xCD);
  s.cdRemaining = 7;
  s.cdDestPtr   = 0x80100000;
  s.cdWordCount = 585;
  s.cdDataCb    = 0x80012345;
  s.cdNotifyCb  = 0x80067890;
  s.gpuSwapCb   = 0x800ABCDE;
  s.drawSync.index = 3;
  s.drawSync.count = 4;
  s.drawSync.status[0] = 0xDEADBEEF;
  s.drawSync.status[7] = 0xCAFEBABE;

  s.reset();

  EXPECT_EQ(s.vsyncCounter.load(), 0u);
  EXPECT_EQ(s.pendingVblanks.load(), 0u);
  EXPECT_FALSE(s.blockingVSyncPacing.load());
  EXPECT_EQ(s.blockingVSyncTarget.load(), 0u);
  EXPECT_EQ(s.autonomousClockWaiters.load(), 0u);
  EXPECT_FALSE(s.cdClockDemand.load());
  EXPECT_EQ(s.cdSyncByte.load(), 0u);
  EXPECT_EQ(s.cdReadyByte.load(), 0u);
  EXPECT_EQ(s.cdRemaining, 0u);
  EXPECT_EQ(s.cdDestPtr, 0u);
  EXPECT_EQ(s.cdWordCount, 0u);
  EXPECT_EQ(s.cdDataCb, 0u);
  EXPECT_EQ(s.cdNotifyCb, 0u);
  EXPECT_EQ(s.gpuSwapCb, 0u);
  EXPECT_EQ(s.drawSync.index, 0u);
  EXPECT_EQ(s.drawSync.count, 2u);
  EXPECT_EQ(s.drawSync.status[0], 0u);
  EXPECT_EQ(s.drawSync.status[7], 0u);
}

TEST_F(PsyqStateTest, AtomicFieldsAreThreadSafeUnderContention) {
  PsyqState &s = psyq_state();
  constexpr int kThreads = 4;
  constexpr int kIters   = 10000;
  std::vector<std::thread> ts;
  ts.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        s.vsyncCounter.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &th : ts) th.join();
  EXPECT_EQ(s.vsyncCounter.load(), uint32_t(kThreads * kIters));
}

TEST_F(PsyqStateTest, NonCopyableNonAssignable) {
  static_assert(!std::is_copy_constructible_v<PsyqState>,
                "PsyqState must not be copy-constructible");
  static_assert(!std::is_copy_assignable_v<PsyqState>,
                "PsyqState must not be copy-assignable");
}

} // namespace
