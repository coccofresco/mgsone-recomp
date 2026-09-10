#pragma once

// ps1Runtime -- Interrupt Controller
// PS1 I_STAT / I_MASK interrupt handling

#include <array>
#include <cstdint>

namespace ps1 {

// IRQ Source Bits
enum IRQSource : uint32_t {
  IRQ_VBLANK = (1 << 0),
  IRQ_GPU = (1 << 1),
  IRQ_CDROM = (1 << 2),
  IRQ_DMA = (1 << 3),
  IRQ_TMR0 = (1 << 4),
  IRQ_TMR1 = (1 << 5),
  IRQ_TMR2 = (1 << 6),
  IRQ_PAD_MC = (1 << 7), // Controller / Memory Card
  IRQ_SIO = (1 << 8),
  IRQ_SPU = (1 << 9),
  IRQ_LIGHTPEN = (1 << 10),
};

class InterruptController {
public:
  InterruptController();
  void reset();

  // Register I/O
  uint32_t readIStat() const { return iStat_; }
  uint32_t readIMask() const { return iMask_; }
  void writeIStat(uint32_t val); // AND-acknowledge (write 0 to clear)
  void writeIMask(uint32_t val); // Set interrupt enable mask

  // Raise an interrupt from a hardware source
  void raiseInterrupt(uint32_t irqBit);

  // Check if any unmasked interrupt is pending
  bool hasPendingInterrupt() const { return (iStat_ & iMask_) != 0; }

  // For COP0: returns whether IP2 should be set in COP0_CAUSE
  bool shouldSignalCPU() const { return hasPendingInterrupt(); }

private:
  uint32_t iStat_ = 0; // 0x1F801070 -- Interrupt Status (R/W: write 0 to ack)
  uint32_t iMask_ = 0; // 0x1F801074 -- Interrupt Mask
};

// Timer (Root Counter)

class Timer {
public:
  Timer();
  void reset();

  // Register I/O (per-timer)
  uint16_t readCounter() const { return counter_; }
  uint16_t readMode() const;
  uint16_t readTarget() const { return target_; }
  void writeCounter(uint16_t val);
  void writeMode(uint16_t val);
  void writeTarget(uint16_t val);

  // Tick by N clock cycles (source-dependent). Returning the number of IRQ
  // edges is important for root counters: timer 2 can reach its target about
  // twenty times during one PAL VBlank, and collapsing that to a bool changes
  // the game's notion of elapsed time.
  uint32_t tick(uint32_t cycles);

  // Configuration
  void setTimerIndex(uint8_t idx) { timerIndex_ = idx; }

private:
  uint8_t timerIndex_ = 0; // 0, 1, or 2
  uint16_t counter_ = 0;
  uint16_t target_ = 0;
  uint16_t mode_ = 0;

  bool reachedTarget_ = false;
  bool reachedOverflow_ = false;
  bool irqFired_ = false;

  // Mode register bits
  bool syncEnable() const { return mode_ & (1 << 0); }
  uint8_t syncMode() const { return (mode_ >> 1) & 3; }
  bool resetOnTarget() const { return mode_ & (1 << 3); }
  bool irqOnTarget() const { return mode_ & (1 << 4); }
  bool irqOnOverflow() const { return mode_ & (1 << 5); }
  bool irqRepeat() const { return mode_ & (1 << 6); }
  bool irqToggle() const { return mode_ & (1 << 7); }
  uint8_t clockSource() const { return (mode_ >> 8) & 3; }
};

// Timers Manager

class Timers {
public:
  Timers();
  void reset();

  // Per-timer register access (0x1F801100 + N*0x10)
  void writeRegister(uint32_t addr, uint16_t val);
  uint16_t readRegister(uint32_t addr) const;

  // Tick all timers (called from main loop)
  // Returns OR of IRQ bits (IRQ_TMR0, IRQ_TMR1, IRQ_TMR2) that fired
  uint32_t tick(uint32_t sysCycles, bool isHblank = false,
                bool isDotclock = false);

  // IRQ edge count produced by the most recent tick() call for timer 0..2.
  uint32_t lastIrqCount(std::size_t timer) const {
    return timer < lastIrqCounts_.size() ? lastIrqCounts_[timer] : 0;
  }

private:
  Timer timers_[3];
  std::array<uint32_t, 3> lastIrqCounts_{};
};

} // namespace ps1
