#include "runtime/vblank_scheduler.h"

#include <gtest/gtest.h>

using ps1::timing::planVblankSlice;
using ps1::timing::guestVblankRequested;
using ps1::timing::planPacingCreditRebase;

TEST(VblankSchedulerTest, NoDebtKeepsProcessedBoundary) {
  const auto slice = planVblankSlice(12, 12, 1);
  EXPECT_EQ(slice.debt, 0u);
  EXPECT_EQ(slice.count, 0u);
  EXPECT_EQ(slice.end, 12u);
  EXPECT_EQ(slice.coalesced, 0u);
}

TEST(VblankSchedulerTest, ClassifiesExcessHostPulsesForCoalescing) {
  const auto slice = planVblankSlice(10, 19, 4);
  EXPECT_EQ(slice.debt, 9u);
  EXPECT_EQ(slice.count, 4u);
  EXPECT_EQ(slice.end, 14u);
  EXPECT_EQ(slice.coalesced, 5u);
}

TEST(VblankSchedulerTest, ZeroLimitStillMakesOneTickOfProgress) {
  const auto slice = planVblankSlice(3, 9, 0);
  EXPECT_EQ(slice.count, 1u);
  EXPECT_EQ(slice.end, 4u);
  EXPECT_EQ(slice.coalesced, 5u);
}

TEST(VblankSchedulerTest, StaleDueValueCannotUnderflow) {
  const auto slice = planVblankSlice(9, 3, 2);
  EXPECT_EQ(slice.debt, 0u);
  EXPECT_EQ(slice.count, 0u);
  EXPECT_EQ(slice.end, 9u);
  EXPECT_EQ(slice.coalesced, 0u);
}

TEST(VblankSchedulerTest, FreeRunsUntilTheFirstBlockingFrameWait) {
  EXPECT_TRUE(guestVblankRequested(false, 0, 100, 0));
}

TEST(VblankSchedulerTest, BlockingPacingRequiresAnOutstandingFutureTarget) {
  EXPECT_FALSE(guestVblankRequested(true, 0, 100, 0));
  EXPECT_FALSE(guestVblankRequested(true, 100, 100, 0));
  EXPECT_TRUE(guestVblankRequested(true, 101, 100, 0));
}

TEST(VblankSchedulerTest, AsynchronousHardwareWaitCanRequestClockProgress) {
  EXPECT_TRUE(guestVblankRequested(true, 0, 100, 1));
}

TEST(VblankSchedulerTest, IdleGameRetainsExactlyOnePendingClockCredit) {
  const auto one = planPacingCreditRebase(100, 101);
  EXPECT_EQ(one.retainedDue, 101u);
  EXPECT_EQ(one.coalesced, 0u);

  const auto backlog = planPacingCreditRebase(100, 107);
  EXPECT_EQ(backlog.retainedDue, 101u);
  EXPECT_EQ(backlog.coalesced, 6u);
}

TEST(VblankSchedulerTest, IdleRebaseHandlesStaleAndMaximalCounters) {
  const auto stale = planPacingCreditRebase(100, 90);
  EXPECT_EQ(stale.retainedDue, 100u);
  EXPECT_EQ(stale.coalesced, 0u);

  const auto maximal = planPacingCreditRebase(UINT64_MAX, UINT64_MAX);
  EXPECT_EQ(maximal.retainedDue, UINT64_MAX);
  EXPECT_EQ(maximal.coalesced, 0u);
}
