#include "runtime/timers/timers.h"
#include <gtest/gtest.h>

using namespace ps1;

// Interrupt Controller Tests

TEST(InterruptController, InitialState) {
  InterruptController ic;
  EXPECT_EQ(ic.readIStat(), 0u);
  EXPECT_EQ(ic.readIMask(), 0u);
  EXPECT_FALSE(ic.hasPendingInterrupt());
}

TEST(InterruptController, RaiseAndMask) {
  InterruptController ic;
  ic.writeIMask(IRQ_VBLANK);
  ic.raiseInterrupt(IRQ_VBLANK);
  EXPECT_TRUE(ic.hasPendingInterrupt());
  EXPECT_TRUE(ic.shouldSignalCPU());
}

TEST(InterruptController, MaskedInterruptNotPending) {
  InterruptController ic;
  ic.writeIMask(0); // Nothing enabled
  ic.raiseInterrupt(IRQ_VBLANK);
  EXPECT_FALSE(ic.hasPendingInterrupt());
}

TEST(InterruptController, AcknowledgeInterrupt) {
  InterruptController ic;
  ic.writeIMask(IRQ_VBLANK | IRQ_CDROM);
  ic.raiseInterrupt(IRQ_VBLANK);
  ic.raiseInterrupt(IRQ_CDROM);
  EXPECT_TRUE(ic.hasPendingInterrupt());

  // Acknowledge VBlank only (write 0 to bit 0, keep bit 2)
  ic.writeIStat(~IRQ_VBLANK);
  EXPECT_TRUE(ic.hasPendingInterrupt()); // CDROM still pending

  // Acknowledge CDROM
  ic.writeIStat(~IRQ_CDROM);
  EXPECT_FALSE(ic.hasPendingInterrupt());
}

TEST(InterruptController, MultipleIRQSources) {
  InterruptController ic;
  ic.writeIMask(0x7FF); // Enable all
  ic.raiseInterrupt(IRQ_TMR0);
  ic.raiseInterrupt(IRQ_TMR1);
  ic.raiseInterrupt(IRQ_DMA);
  EXPECT_EQ(ic.readIStat(), (IRQ_TMR0 | IRQ_TMR1 | IRQ_DMA));
}

// Timer Tests

TEST(Timer, InitialState) {
  Timers timers;
  EXPECT_EQ(timers.readRegister(0x1F801100), 0); // Counter 0
  EXPECT_EQ(timers.readRegister(0x1F801110), 0); // Counter 1
  EXPECT_EQ(timers.readRegister(0x1F801120), 0); // Counter 2
}

TEST(Timer, CounterIncrement) {
  Timers timers;
  timers.tick(100);
  uint16_t val = timers.readRegister(0x1F801100);
  EXPECT_EQ(val, 100);
}

TEST(Timer, TargetIRQ) {
  Timers timers;
  // Set TMR0 target to 50
  timers.writeRegister(0x1F801108, 50);
  // Set mode: IRQ on target, reset on target
  timers.writeRegister(0x1F801104, (1 << 3) | (1 << 4));

  uint32_t irqs = timers.tick(50);
  EXPECT_NE(irqs & IRQ_TMR0, 0u); // Should fire
}

TEST(Timer, PreservesRepeatedIrqCountWithinOneHardwareTick) {
  Timers timers;
  timers.writeRegister(0x1F801128, 10); // timer 2 target
  timers.writeRegister(0x1F801124,
                       (1 << 3) | (1 << 4) | (1 << 6));

  const uint32_t irqs = timers.tick(35);

  EXPECT_NE(irqs & IRQ_TMR2, 0u);
  EXPECT_EQ(timers.lastIrqCount(2), 3u)
      << "three counter-target IRQs must not collapse into one event";
  EXPECT_EQ(timers.readRegister(0x1F801120), 5u);
}

TEST(Timer, OverflowIRQ) {
  Timers timers;
  // Set mode: IRQ on overflow
  timers.writeRegister(0x1F801104, (1 << 5));

  // Tick close to overflow
  timers.writeRegister(0x1F801100, 0xFFF0); // start near max
  // Need to tick 16 more to overflow
  uint32_t irqs = timers.tick(16);
  EXPECT_NE(irqs & IRQ_TMR0, 0u);
}

TEST(Timer, WriteModResetsCounter) {
  Timers timers;
  timers.tick(100);
  EXPECT_EQ(timers.readRegister(0x1F801100), 100);

  // Writing mode resets counter
  timers.writeRegister(0x1F801104, 0);
  EXPECT_EQ(timers.readRegister(0x1F801100), 0);
}

TEST(Timer, ResetOnTarget) {
  Timers timers;
  timers.writeRegister(0x1F801108, 10);       // target = 10
  timers.writeRegister(0x1F801104, (1 << 3)); // reset on target

  timers.tick(15);
  // Counter should have reset at 10, so now at 5
  uint16_t val = timers.readRegister(0x1F801100);
  EXPECT_EQ(val, 5);
}

// VBlank IRQ Test

TEST(Vblank, IRQRaised) {
  InterruptController ic;
  ic.writeIMask(IRQ_VBLANK);
  ic.raiseInterrupt(IRQ_VBLANK);
  EXPECT_TRUE(ic.hasPendingInterrupt());
}

TEST(Timer, TargetResetDoesNotGenerateOverflowForAnyIrqMode) {
  for (unsigned irqMode = 0; irqMode < 16; ++irqMode) {
    SCOPED_TRACE(irqMode);
    Timer timer;
    timer.writeTarget(7);
    timer.writeMode((irqMode << 4) | 8);
    const unsigned crossings = 5;
    const unsigned expected = !(irqMode & 1) ? 0 : !(irqMode & 4) ? 1 :
        (irqMode & 8) ? (crossings + 1) / 2 : crossings;
    EXPECT_EQ(timer.tick(7 * crossings + 2), expected);
    EXPECT_EQ(timer.readCounter(), 2);
    const auto mode = timer.readMode();
    EXPECT_NE(mode & (1 << 11), 0);
    EXPECT_EQ(mode & (1 << 12), 0);
  }
}

TEST(Timer, AllClockSourceAliasesSelectTheSameClock) {
  for (unsigned index = 0; index < 3; ++index) {
    for (unsigned source = 0; source < 4; ++source) {
      for (unsigned blank = 0; blank < 2; ++blank) {
        for (unsigned dot = 0; dot < 2; ++dot) {
          SCOPED_TRACE(::testing::Message() << index << '/' << source << '/' << blank << '/' << dot);
          Timers timers;
          const uint32_t base = 0x1f801100 + index * 16;
          timers.writeRegister(base + 4, source << 8);
          timers.tick(80, blank, dot);
          const unsigned expected = index == 2 ? ((source & 2) ? 10 : 80) :
              !(source & 1) ? 80 : index == 0 ? dot : blank;
          EXPECT_EQ(timers.readRegister(base), expected);
        }
      }
    }
  }
}

TEST(Timer, DividedClockIsIndependentOfHostChunkBoundaries) {
  for (unsigned chunk = 1; chunk <= 17; ++chunk) {
    SCOPED_TRACE(chunk);
    Timers whole, split;
    for (auto *timers : {&whole, &split}) {
      timers->writeRegister(0x1f801128, 13);
      timers->writeRegister(0x1f801124, 0x258); // /8, repeated target pulse
    }
    whole.tick(10007);
    unsigned irqs = 0;
    for (unsigned time = 0; time < 10007;) {
      const auto step = std::min(chunk, 10007 - time);
      split.tick(step);
      irqs += split.lastIrqCount(2);
      time += step;
    }
    EXPECT_EQ(irqs, whole.lastIrqCount(2));
    EXPECT_EQ(split.readRegister(0x1f801120), whole.readRegister(0x1f801120));
    EXPECT_EQ(split.tick(1), whole.tick(1)); // residual seven clocks cross /8
    EXPECT_EQ(split.readRegister(0x1f801120), whole.readRegister(0x1f801120));
  }
}

TEST(Timer, ModeStatusIsReadOnlyAndReachedFlagsClearOnGuestRead) {
  Timer timer;
  timer.writeTarget(5);
  timer.writeMode(0xffff);
  EXPECT_EQ(timer.readMode() & 0x1c00, 0x400);
  timer.tick(5);
  EXPECT_EQ(timer.readMode() & 0x1800, 0x800);
  EXPECT_EQ(timer.readMode() & 0x1800, 0);
}

TEST(Timer, TimerTwoSynchronizationStopsOnlyModesZeroAndThree) {
  for (unsigned sync = 0; sync < 4; ++sync) {
    for (unsigned clock = 0; clock < 4; ++clock) {
      Timers timers;
      timers.writeRegister(0x1f801124, 1 | (sync << 1) | (clock << 8));
      timers.writeRegister(0x1f801120, 123);
      timers.tick(80);
      EXPECT_EQ(timers.readRegister(0x1f801120),
          123 + ((sync == 0 || sync == 3) ? 0 : (clock & 2) ? 10 : 80));
    }
  }
}

TEST(Timer, StoppedDividerDoesNotAccumulateCredit) {
  Timers timers;
  timers.writeRegister(0x1f801124, 0x200);
  timers.tick(7);
  timers.writeRegister(0x1f801124, 0x201); // stop
  timers.tick(3);
  timers.writeRegister(0x1f801124, 0x200); // resume
  timers.tick(1);
  EXPECT_EQ(timers.readRegister(0x1f801120), 1);
}

TEST(Timer, SnapshotContinuationMatchesUninterruptedIrqMatrix) {
  for (unsigned irqMode = 0; irqMode < 16; ++irqMode) {
    for (unsigned phase = 0; phase < 40; ++phase) {
      Timers continuous, resumed;
      for (unsigned index = 0; index < 3; ++index) {
        const uint32_t base = 0x1f801100 + index * 16;
        continuous.writeRegister(base + 8, 3);
        continuous.writeRegister(base + 4, 8 | (irqMode << 4) | (index == 2 ? 0x200 : 0));
      }
      continuous.tick(phase);
      ASSERT_TRUE(resumed.restore(continuous.snapshot()));
      for (const auto chunk : {1u, 7u, 17u, 131u}) {
        ASSERT_EQ(resumed.tick(chunk), continuous.tick(chunk));
        for (unsigned index = 0; index < 3; ++index) {
          const uint32_t base = 0x1f801100 + index * 16;
          ASSERT_EQ(resumed.lastIrqCount(index), continuous.lastIrqCount(index));
          ASSERT_EQ(resumed.readRegister(base), continuous.readRegister(base));
          ASSERT_EQ(resumed.readRegister(base + 4), continuous.readRegister(base + 4));
        }
      }
    }
  }
}
