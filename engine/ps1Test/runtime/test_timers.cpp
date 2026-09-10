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
