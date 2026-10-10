#include "runtime/timers/timers.h"
#include <fmt/format.h>

namespace ps1 {

// Interrupt Controller

InterruptController::InterruptController() { reset(); }

void InterruptController::reset() {
  iStat_ = 0;
  iMask_ = 0;
}

void InterruptController::writeIStat(uint32_t val) {
  // I_STAT is acknowledged by writing 0 to bits you want to clear
  // PS1 behavior: iStat = iStat AND val (write 0 to clear, write 1 to keep)
  iStat_ &= val;
}

void InterruptController::writeIMask(uint32_t val) {
  iMask_ = val & 0x7FF; // Only 11 IRQ sources
}

void InterruptController::raiseInterrupt(uint32_t irqBit) { iStat_ |= irqBit; }

// Timer

Timer::Timer() { reset(); }

void Timer::reset() {
  counter_ = 0;
  target_ = 0;
  mode_ = 1 << 10; // IRQ line inactive
  reachedTarget_ = false;
  reachedOverflow_ = false;
  irqFired_ = false;
}

uint16_t Timer::readMode() const {
  const uint16_t val = peekMode();
  reachedTarget_ = reachedOverflow_ = false;
  return val;
}

uint16_t Timer::peekMode() const {
  uint16_t val = mode_;
  // Bits 11-12 reflect reached status
  if (reachedTarget_)
    val |= (1 << 11);
  if (reachedOverflow_)
    val |= (1 << 12);
  return val;
}

void Timer::writeCounter(uint16_t val) { counter_ = val; }

void Timer::writeMode(uint16_t val) {
  mode_ = (val & 0x3ff) | (1 << 10); // status bits are not writable
  counter_ = 0; // Writing mode resets counter
  reachedTarget_ = false;
  reachedOverflow_ = false;
  irqFired_ = false;
}

void Timer::writeTarget(uint16_t val) { target_ = val; }

uint32_t Timer::tick(uint32_t cycles) {
  if (!countingEnabled())
    return 0;
  uint32_t fireCount = 0;

  for (uint32_t i = 0; i < cycles; i++) {
    counter_++;
    // Capture real wrap before reset-on-target changes the counter to zero.
    const bool overflow = counter_ == 0;
    bool irqCondition = false;

    // Check target
    if (target_ != 0 && counter_ == target_) {
      reachedTarget_ = true;

      irqCondition |= irqOnTarget();

      if (resetOnTarget()) {
        counter_ = 0;
      }
    }

    // Check overflow (0xFFFF -> 0)
    if (overflow) {
      reachedOverflow_ = true;

      irqCondition |= irqOnOverflow();
    }
    if (irqCondition && (!irqFired_ || irqRepeat())) {
      if (irqToggle()) mode_ ^= 1 << 10;
      if (!irqToggle() || !(mode_ & (1 << 10))) {
        ++fireCount;
        irqFired_ = true;
      }
    }
  }

  return fireCount;
}

Timer::Snapshot Timer::snapshot() const {
  return {peekMode(), target_, counter_, uint8_t(irqFired_)};
}

void Timer::restore(const Snapshot &state) {
  mode_ = state.mode & 0x7ff;
  target_ = state.target;
  counter_ = state.counter;
  reachedTarget_ = state.mode & (1 << 11);
  reachedOverflow_ = state.mode & (1 << 12);
  irqFired_ = state.irqFired != 0;
}

// Timers Manager

Timers::Timers() { reset(); }

void Timers::reset() {
  lastIrqCounts_.fill(0);
  sysclkDiv8Carry_ = 0;
  for (int i = 0; i < 3; i++) {
    timers_[i].reset();
    timers_[i].setTimerIndex(i);
  }
}

void Timers::writeRegister(uint32_t addr, uint16_t val) {
  uint32_t offset = addr - 0x1F801100;
  uint32_t timerIdx = offset / 0x10;
  uint32_t reg = offset % 0x10;

  if (timerIdx >= 3)
    return;

  switch (reg) {
  case 0x00:
    timers_[timerIdx].writeCounter(val);
    break;
  case 0x04:
    timers_[timerIdx].writeMode(val);
    break;
  case 0x08:
    timers_[timerIdx].writeTarget(val);
    break;
  }
}

uint16_t Timers::readRegister(uint32_t addr) const {
  uint32_t offset = addr - 0x1F801100;
  uint32_t timerIdx = offset / 0x10;
  uint32_t reg = offset % 0x10;

  if (timerIdx >= 3)
    return 0;

  switch (reg) {
  case 0x00:
    return timers_[timerIdx].readCounter();
  case 0x04:
    return timers_[timerIdx].readMode();
  case 0x08:
    return timers_[timerIdx].readTarget();
  }
  return 0;
}

uint32_t Timers::tick(uint32_t sysCycles, bool isHblank, bool isDotclock) {
  uint32_t irqBits = 0;
  lastIrqCounts_.fill(0);

  // Timer 0: dotclock or sysclock
  {
    const bool external = timers_[0].peekMode() & (1 << 8);
    uint32_t cycles = external ? uint32_t(isDotclock) : sysCycles;
    if (cycles)
      lastIrqCounts_[0] = timers_[0].tick(cycles);
    if (lastIrqCounts_[0] != 0) {
      irqBits |= IRQ_TMR0;
    }
  }

  // Timer 1: hblank or sysclock
  {
    const bool external = timers_[1].peekMode() & (1 << 8);
    uint32_t cycles = external ? uint32_t(isHblank) : sysCycles;
    if (cycles)
      lastIrqCounts_[1] = timers_[1].tick(cycles);
    if (lastIrqCounts_[1] != 0) {
      irqBits |= IRQ_TMR1;
    }
  }

  // Timer 2: sysclock or sysclock/8
  {
    const bool divided = timers_[2].peekMode() & (1 << 9);
    uint32_t cycles = timers_[2].countingEnabled() ? sysCycles : 0;
    if (divided && timers_[2].countingEnabled()) {
      const uint64_t credit = uint64_t(sysCycles) + sysclkDiv8Carry_;
      cycles = credit / 8;
      sysclkDiv8Carry_ = credit % 8;
    }
    if (cycles > 0)
      lastIrqCounts_[2] = timers_[2].tick(cycles);
    if (lastIrqCounts_[2] != 0) {
      irqBits |= IRQ_TMR2;
    }
  }

  return irqBits;
}

Timers::Snapshot Timers::snapshot() const {
  return {{timers_[0].snapshot(), timers_[1].snapshot(), timers_[2].snapshot()},
          sysclkDiv8Carry_};
}

bool Timers::validSnapshot(const Snapshot &state) {
  if (state.sysclkDiv8Carry >= 8) return false;
  for (const auto &timer : state.timers)
    if (timer.irqFired > 1 || (timer.mode & 0xe000)) return false;
  return true;
}

bool Timers::restore(const Snapshot &state) {
  if (!validSnapshot(state)) return false;
  for (unsigned i = 0; i < 3; ++i) timers_[i].restore(state.timers[i]);
  sysclkDiv8Carry_ = state.sysclkDiv8Carry;
  lastIrqCounts_.fill(0); // consumed host delivery counts are not pending IRQs
  return true;
}

} // namespace ps1
