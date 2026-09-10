#pragma once
/**
 * @file memory.h
 * @brief PS1 memory subsystem -- 2 MB RAM, scratchpad, BIOS ROM and I/O
 * routing.
 *
 * `ps1::Memory` maps the full PS1 address space and routes reads/writes to the
 * correct hardware device. All three KSEG mirrors (KUSEG/KSEG0/KSEG1) resolve
 * to the same physical addresses via `toPhysical()`.
 *
 * ## Memory Map
 * | Range | Size | Description |
 * |-------|------|-------------|
 * | 0x00000000 - 0x001FFFFF | 2 MB | Main RAM (KUSEG) |
 * | 0x1F800000 - 0x1F8003FF | 1 KB | Scratchpad (data cache) |
 * | 0x1F801000 - 0x1F802FFF | 8 KB | I/O ports (hardware registers) |
 * | 0x1FC00000 - 0x1FC7FFFF | 512 KB | BIOS ROM |
 * | 0x80000000 - 0x801FFFFF | 2 MB | Main RAM mirror -- KSEG0 (cached) |
 * | 0xA0000000 - 0xA01FFFFF | 2 MB | Main RAM mirror -- KSEG1 (uncached) |
 *
 * **Thread safety:** RAM reads/writes use `atomic_thread_fence` acquire/release
 * so the VBlank counter is visible across the game thread and VBlank thread.
 */

#include "runtime/pgxp.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ps1 {

class DMA;
class InterruptController;
class Timers;
namespace cdrom {
class CdromController;
}
namespace gpu {
class GPU;
}
namespace input {
class InputController;
}
namespace mdec {
class MDEC;
}
namespace spu {
class SPU;
}

// PS1 Memory Map
//
// 0x00000000 - 0x001FFFFF  Main RAM (2MB) -- KUSEG
// 0x1F800000 - 0x1F8003FF  Scratchpad (1KB data cache)
// 0x1F801000 - 0x1F802FFF  I/O Ports (hardware registers)
// 0x1FC00000 - 0x1FC7FFFF  BIOS ROM (512KB)
// 0x80000000 - 0x801FFFFF  Main RAM mirror -- KSEG0 (cached)
// 0xA0000000 - 0xA01FFFFF  Main RAM mirror -- KSEG1 (uncached)
//

class Memory {
public:
  static constexpr uint32_t RAM_SIZE = 2 * 1024 * 1024; // 2MB
  static constexpr uint32_t SCRATCHPAD_SIZE = 1024;     // 1KB
  static constexpr uint32_t BIOS_SIZE = 512 * 1024;     // 512KB

  // Memory region base addresses (physical)
  static constexpr uint32_t RAM_BASE = 0x00000000;
  static constexpr uint32_t SCRATCHPAD_BASE = 0x1F800000;
  static constexpr uint32_t IO_BASE = 0x1F801000;
  static constexpr uint32_t BIOS_BASE = 0x1FC00000;

  Memory() { reset(); }

  void reset() {
    std::memset(ram_, 0, sizeof(ram_));
    std::memset(scratchpad_, 0, sizeof(scratchpad_));
    std::memset(bios_, 0xFF, sizeof(bios_)); // BIOS starts as 0xFF (ROM)
    clearPrecision();
  }

  void clearPrecision() {
    // The compact validity plane is authoritative. Leaving invalid payloads
    // untouched avoids sweeping the 56 MiB PGXP provenance store on every
    // save-state load; a later store overwrites the complete value before
    // publishing its validity byte.
    if (pgxpRamValidData_)
      std::memset(pgxpRamValidData_, 0, RAM_SIZE / sizeof(uint32_t));
    std::fill(pgxpScratchpad_.begin(), pgxpScratchpad_.end(), PGXPValue{});
  }

  void invalidatePrecision(uint32_t addr, uint32_t byteCount = 4) {
    if (byteCount == 0)
      return;
    uint32_t current = addr;
    uint32_t remaining = byteCount;
    while (remaining != 0) {
      const uint32_t phys = toPhysical(current);
      if (phys < RAM_SIZE) {
        if (pgxpRamValidData_)
          pgxpRamValidData_[phys >> 2] = 0;
      } else if (PGXPValue *slot = precisionSlot(phys)) {
        slot->invalidate();
      }
      const uint32_t chunk = std::min<uint32_t>(remaining, 4u - (phys & 3u));
      current += chunk;
      remaining -= chunk;
    }
  }

  void storePrecision(uint32_t addr, uint32_t value,
                      const PGXPValue &precision) {
    const uint32_t phys = toPhysical(addr);
    if ((phys & 3u) != 0 || !precision.hasXY(value))
      return;
    PGXPValue *slot = precisionSlot(phys, true);
    if (slot == nullptr)
      return;
    PGXPValue stored = precision;
    stored.value = value;
    *slot = stored;
    if (phys < RAM_SIZE)
      pgxpRamValidData_[phys >> 2] = 1;
    countPGXPStage(PGXPStage::RamStore);
  }

  bool lookupPrecision(uint32_t addr, uint32_t value, PGXPValue &out) const {
    const uint32_t phys = toPhysical(addr);
    const PGXPValue *stored = nullptr;
    if ((phys & 3u) == 0) {
      if (phys < RAM_SIZE) {
        if (pgxpRamValidData_ && pgxpRamValidData_[phys >> 2] != 0)
          stored = &pgxpRamData_[phys >> 2];
      } else {
        stored = precisionSlot(phys);
      }
    }
    if (stored == nullptr) {
      notePGXPRamLookup(addr, value, PGXPRamLookup::NoSidecar);
      countPGXPStage(PGXPStage::RamLookupMiss);
      return false;
    }
    if (!stored->hasXY(value)) {
      notePGXPRamLookup(addr, value,
                        (stored->flags & PGXPValue::VALID_XY) ==
                                PGXPValue::VALID_XY
                            ? PGXPRamLookup::Stale
                            : PGXPRamLookup::Empty);
      countPGXPStage(PGXPStage::RamLookupMiss);
      return false;
    }
    out = *stored;
    const PGXPRamLookup result = stored->hasXYZ(value) ? PGXPRamLookup::HitXYZ
                                 : stored->hasSignedProjection(value)
                                     ? PGXPRamLookup::HitSignedProjection
                                     : PGXPRamLookup::HitXY;
    notePGXPRamLookup(addr, value, result);
    countPGXPStage(PGXPStage::RamLookupHit);
    return true;
  }

  // Cheap hot-path prefilter for recompiled LW. More than 98% of Crash 3's
  // word loads have no precision companion; consulting this compact plane
  // avoids entering the full lookup/diagnostic path and touching a sparse
  // 112-byte PGXPValue for those ordinary integer loads.
  [[gnu::always_inline]] bool mightHavePrecision(uint32_t addr) const {
    const uint32_t phys = toPhysical(addr);
    if ((phys & 3u) != 0)
      return false;
    if (phys < RAM_SIZE)
      return pgxpRamValidData_ && pgxpRamValidData_[phys >> 2] != 0;
    if (phys >= SCRATCHPAD_BASE &&
        phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      return (pgxpScratchpad_[(phys - SCRATCHPAD_BASE) >> 2].flags &
              PGXPValue::VALID_XY) != 0;
    }
    return false;
  }

  // Copy guest bytes while moving the PGXP sidecar in lockstep.  HLE libc
  // must not turn an exact GTE projection back into an integer-only packet
  // merely because the game used memcpy/memmove to stage its ordering table.
  //
  // Provenance is retained only for complete aligned destination words whose
  // four bytes originate from one aligned source word. Partial or differently
  // aligned copies deliberately invalidate the affected word: combining byte
  // fragments cannot produce trustworthy XYZ. The sidecar move happens before
  // the data move so overlapping memmove ranges preserve the original source.
  void copyBytes(uint32_t dstAddr, uint32_t srcAddr, uint32_t byteCount) {
    if (byteCount == 0)
      return;

    const uint32_t dstPhys = toPhysical(dstAddr);
    const uint32_t srcPhys = toPhysical(srcAddr);
    if (dstPhys == srcPhys)
      return;

    const bool contiguousRam = dstPhys < RAM_SIZE && srcPhys < RAM_SIZE &&
                               byteCount <= RAM_SIZE - dstPhys &&
                               byteCount <= RAM_SIZE - srcPhys;
    if (!contiguousRam) {
      // A snapshot is required when a range crosses the 2 MiB RAM mirror or
      // moves between RAM and scratchpad. Choosing direction from only the
      // initial physical addresses corrupts wrapped overlaps.
      std::vector<uint8_t> payload(byteCount);
      for (uint32_t i = 0; i < byteCount; ++i)
        payload[i] = read8(srcAddr + i);

      struct PrecisionCopy {
        uint32_t destination;
        uint32_t value;
        PGXPValue precision;
      };
      std::vector<PrecisionCopy> precisionCopies;
      if (byteCount <= RAM_SIZE && ((dstAddr ^ srcAddr) & 3u) == 0) {
        const uint32_t first = (4u - (dstAddr & 3u)) & 3u;
        for (uint32_t offset = first; offset + 4u <= byteCount; offset += 4u) {
          const uint32_t sourcePhys = toPhysical(srcAddr + offset);
          const uint32_t destinationPhys = toPhysical(dstAddr + offset);
          const PGXPValue *sourcePrecision =
              sourcePhys < RAM_SIZE
                  ? (pgxpRamValidData_ &&
                             pgxpRamValidData_[sourcePhys >> 2] != 0
                         ? &pgxpRamData_[sourcePhys >> 2]
                         : nullptr)
                  : precisionSlot(sourcePhys);
          if (sourcePrecision == nullptr ||
              !supportsPrecisionAddress(destinationPhys))
            continue;
          uint32_t value;
          std::memcpy(&value, payload.data() + offset, sizeof(value));
          if (sourcePrecision->hasXY(value))
            precisionCopies.push_back(
                {dstAddr + offset, value, *sourcePrecision});
        }
      }

      for (uint32_t i = 0; i < byteCount; ++i)
        write8(dstAddr + i, payload[i]);
      for (const PrecisionCopy &copy : precisionCopies) {
        if (read32(copy.destination) == copy.value)
          storePrecision(copy.destination, copy.value, copy.precision);
      }
      return;
    }

    const uint32_t dstEnd = dstPhys + byteCount;
    const bool alignedMapping = ((dstPhys ^ srcPhys) & 3u) == 0;
    uint32_t firstFullWord = 0;
    uint32_t fullWordEnd = 0;
    if (pgxpRamData_) {
      if (alignedMapping) {
        firstFullWord = (dstPhys + 3u) & ~3u;
        fullWordEnd = dstEnd & ~3u;
        if (firstFullWord < fullWordEnd) {
          const uint32_t sourceFirst = srcPhys + (firstFullWord - dstPhys);
          std::memmove(&pgxpRamData_[firstFullWord >> 2],
                       &pgxpRamData_[sourceFirst >> 2],
                       (fullWordEnd - firstFullWord) / 4u * sizeof(PGXPValue));
          std::memmove(&pgxpRamValidData_[firstFullWord >> 2],
                       &pgxpRamValidData_[sourceFirst >> 2],
                       (fullWordEnd - firstFullWord) / 4u);
        }

        if ((dstPhys & 3u) != 0) {
          pgxpRamData_[dstPhys >> 2].invalidate();
          pgxpRamValidData_[dstPhys >> 2] = 0;
        }
        if ((dstEnd & 3u) != 0) {
          pgxpRamData_[(dstEnd - 1u) >> 2].invalidate();
          pgxpRamValidData_[(dstEnd - 1u) >> 2] = 0;
        }
      } else {
        invalidatePrecision(dstAddr, byteCount);
      }
    }

    std::memmove(&ram_[dstPhys], &ram_[srcPhys], byteCount);
    std::atomic_thread_fence(std::memory_order_release);

    // A direct loader may have changed RAM without touching its sidecar.
    // Never amplify such a stale entry through a bulk copy.
    if (pgxpRamData_ && alignedMapping) {
      for (uint32_t addr = firstFullWord; addr < fullWordEnd; addr += 4u) {
        uint32_t value;
        std::memcpy(&value, &ram_[addr], sizeof(value));
        if (pgxpRamValidData_[addr >> 2] != 0 &&
            !pgxpRamData_[addr >> 2].hasXY(value)) {
          pgxpRamData_[addr >> 2].invalidate();
          pgxpRamValidData_[addr >> 2] = 0;
        }
      }
    }
  }

  // Read

  uint8_t read8(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);

    if (phys < RAM_SIZE) {
      // Acquire fence: prevents the compiler from hoisting RAM reads out of
      // busy-wait loops.  The main thread writes the VBlank counter while
      // the game thread spins on it in VSync's busy-wait (func_8003E638).
      std::atomic_thread_fence(std::memory_order_acquire);
      return ram_[phys];
    }
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      return scratchpad_[phys - SCRATCHPAD_BASE];
    }
    if (phys >= BIOS_BASE && phys < BIOS_BASE + BIOS_SIZE) {
      return bios_[phys - BIOS_BASE];
    }
    return read8Slow(addr, phys);
  }

  uint16_t read16(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);

    if ((phys >= 0x1F801C00 && phys < 0x1F802000) ||
        (phys >= 0x1F801100 && phys < 0x1F801130) ||
        (phys >= 0x1F801040 && phys < 0x1F801050) || phys == 0x1F801070 ||
        phys == 0x1F801074) {
      return read16Slow(addr, phys);
    }

    uint8_t lo = read8(addr);
    uint8_t hi = read8(addr + 1);
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  }

  uint32_t read32(uint32_t addr) const {
    uint32_t phys = toPhysical(addr);

    // RAM fast-path (most common, avoids 4x read8 calls)
    if (phys < RAM_SIZE) {
      std::atomic_thread_fence(std::memory_order_acquire);
      uint32_t val;
      std::memcpy(&val, &ram_[phys], sizeof(val));
      return val; // little-endian, matches PS1
    }

    if ((phys >= 0x1F801000 && phys < 0x1F802000) ||
        phys == 0xFFFE0130) {
      return read32Slow(addr, phys);
    }

    // Default: compose from byte reads
    uint8_t b0 = read8(addr);
    uint8_t b1 = read8(addr + 1);
    uint8_t b2 = read8(addr + 2);
    uint8_t b3 = read8(addr + 3);
    return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
           (static_cast<uint32_t>(b2) << 16) |
           (static_cast<uint32_t>(b3) << 24);
  }

  // Write

  void write8(uint32_t addr, uint8_t val) {
    uint32_t phys = toPhysical(addr);

    if (phys < RAM_SIZE) {
      invalidatePrecision(addr, 1);
      ram_[phys] = val;
      return;
    }
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      invalidatePrecision(addr, 1);
      scratchpad_[phys - SCRATCHPAD_BASE] = val;
      // A diagnostic watch names a 32-bit guest word, but hand-written PS1
      // code is free to alter that word with SB/SH.  Report the reconstructed
      // word after every overlapping byte write; aligned SW still takes the
      // single-callback fast path in write32().  Without this, an apparent
      // spontaneous scratchpad change can hide the instruction that caused
      // it precisely when debugging packed renderer/VM state.
      if (watchpointPhysAddr_ != 0 && watchpointCb_ &&
          phys >= watchpointPhysAddr_ && phys < watchpointPhysAddr_ + 4u &&
          watchpointPhysAddr_ >= SCRATCHPAD_BASE &&
          watchpointPhysAddr_ + sizeof(uint32_t) <=
              SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
        uint32_t watched = 0;
        std::memcpy(&watched,
                    &scratchpad_[watchpointPhysAddr_ - SCRATCHPAD_BASE],
                    sizeof(watched));
        watchpointCb_(watched);
      }
      return;
    }

    write8Slow(addr, phys, val);
  }

  void write16(uint32_t addr, uint16_t val) {
    uint32_t phys = toPhysical(addr);

    if ((phys >= 0x1F801C00 && phys < 0x1F802000) ||
        (phys >= 0x1F801100 && phys < 0x1F801130) ||
        (phys >= 0x1F801040 && phys < 0x1F801050) || phys == 0x1F801070 ||
        phys == 0x1F801074) {
      write16Slow(addr, phys, val);
      return;
    }

    write8(addr, static_cast<uint8_t>(val & 0xFF));
    write8(addr + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
  }

  void write32(uint32_t addr, uint32_t val) {
    uint32_t phys = toPhysical(addr);

    // RAM fast-path
    if (phys < RAM_SIZE) {
      invalidatePrecision(addr, sizeof(val));
      std::memcpy(&ram_[phys], &val, sizeof(val));
      std::atomic_thread_fence(std::memory_order_release);
      // Fire write watchpoint if registered for this address
      if (watchpointPhysAddr_ != 0 && phys == watchpointPhysAddr_ &&
          watchpointCb_) {
        watchpointCb_(val);
      }
      return;
    }

    // Keep aligned scratchpad writes whole as well.  Besides matching the RAM
    // fast path, this lets the optional write watchpoint trace renderer work
    // queues kept in the PS1's 1 KB scratchpad.
    if (phys >= SCRATCHPAD_BASE &&
        phys + sizeof(val) <= SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
      invalidatePrecision(addr, sizeof(val));
      std::memcpy(&scratchpad_[phys - SCRATCHPAD_BASE], &val, sizeof(val));
      if (watchpointPhysAddr_ != 0 && phys == watchpointPhysAddr_ &&
          watchpointCb_) {
        watchpointCb_(val);
      }
      return;
    }

    write32Slow(addr, phys, val);
  }

  // Tracked 32-bit store used by the PGXP-aware CPU paths. Crash 3 submits
  // most GP0 packets with ordinary SW instructions to the MMIO data port, so
  // limiting provenance to DMA/ordering-table reads loses the coordinate at
  // the final and most important hop.
  void write32Tracked(uint32_t addr, uint32_t val, const PGXPValue *precision) {
    const uint32_t phys = toPhysical(addr);
    if (phys == 0x1F801810) {
      write32TrackedSlow(addr, phys, val, precision);
      return;
    }
    write32(addr, val);
    if (precision && precision->hasXY(val))
      storePrecision(addr, val, *precision);
  }

  // Direct access for loading binaries

  uint8_t *ramPtr() { return ram_; }
  const uint8_t *ramPtr() const { return ram_; }

  uint8_t *biosPtr() { return bios_; }
  const uint8_t *biosPtr() const { return bios_; }

  uint8_t *scratchpadPtr() { return scratchpad_; }
  const uint8_t *scratchpadPtr() const { return scratchpad_; }

  // Hardware Connections

  void setGPU(ps1::gpu::GPU *gpu) { gpu_ = gpu; }
  ps1::gpu::GPU *getGPU() const { return gpu_; }
  void setSPU(spu::SPU *spu) { spu_ = spu; }
  void setDMA(DMA *dma) { dma_ = dma; }
  void setCDROM(cdrom::CdromController *cdrom) { cdrom_ = cdrom; }
  void setInput(input::InputController *input) { input_ = input; }
  void setMDEC(mdec::MDEC *mdec) { mdec_ = mdec; }
  void setTimers(Timers *timers) { timers_ = timers; }
  void setInterruptController(InterruptController *irq) { irqCtrl_ = irq; }

  // Write Watchpoint
  // Register a single-address watchpoint: fires cb(newValue) whenever
  // the 32-bit word at virtAddr is written.  Used by the BIOS to detect
  // when the game clears cdSyncByte (writes 0) so that a deferred INT2
  // can be delivered immediately on the game thread.
  void setWriteWatchpoint(uint32_t virtAddr, std::function<void(uint32_t)> cb) {
    watchpointPhysAddr_ = toPhysical(virtAddr);
    watchpointCb_ = std::move(cb);
  }
  void clearWriteWatchpoint() {
    watchpointPhysAddr_ = 0;
    watchpointCb_ = nullptr;
  }

private:
  // The recompiled CPU keeps RAM, scratchpad, BIOS, PGXP and watchpoint paths
  // inline. Only device/MMIO routing crosses this cold boundary, which keeps
  // renderer headers out of generated translation units without adding a call
  // to ordinary guest memory accesses.
  uint8_t read8Slow(uint32_t addr, uint32_t phys) const;
  uint16_t read16Slow(uint32_t addr, uint32_t phys) const;
  uint32_t read32Slow(uint32_t addr, uint32_t phys) const;
  void write8Slow(uint32_t addr, uint32_t phys, uint8_t value);
  void write16Slow(uint32_t addr, uint32_t phys, uint16_t value);
  void write32Slow(uint32_t addr, uint32_t phys, uint32_t value);
  void write32TrackedSlow(uint32_t addr, uint32_t phys, uint32_t value,
                          const PGXPValue *precision);

  uint8_t ram_[RAM_SIZE];
  uint8_t scratchpad_[SCRATCHPAD_SIZE];
  uint8_t bios_[BIOS_SIZE];
  std::unique_ptr<PGXPValue[]> pgxpRam_;
  // Hot misses consult this 512 KiB plane before touching a sparse 112-byte
  // provenance record. It also lets ordinary integer stores invalidate one
  // byte instead of pulling an entire PGXPValue cache line into the CPU.
  std::unique_ptr<uint8_t[]> pgxpRamValid_;
  // Non-owning aliases keep libstdc++ unique_ptr accessors out of the two
  // hottest generated-code paths. The unique_ptr members remain the owners.
  PGXPValue *pgxpRamData_ = nullptr;
  uint8_t *pgxpRamValidData_ = nullptr;
  std::array<PGXPValue, SCRATCHPAD_SIZE / sizeof(uint32_t)> pgxpScratchpad_{};

  static bool supportsPrecisionAddress(uint32_t phys) {
    return phys < RAM_SIZE || (phys >= SCRATCHPAD_BASE &&
                               phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE);
  }

  [[gnu::always_inline]] PGXPValue *
  precisionSlot(uint32_t phys, bool allocateRam = false) {
    if (phys < RAM_SIZE) {
      if (!pgxpRam_ && allocateRam) {
        pgxpRam_ = std::make_unique<PGXPValue[]>(RAM_SIZE / sizeof(uint32_t));
        pgxpRamValid_ =
            std::make_unique<uint8_t[]>(RAM_SIZE / sizeof(uint32_t));
        pgxpRamData_ = pgxpRam_.get();
        pgxpRamValidData_ = pgxpRamValid_.get();
      }
      return pgxpRamData_ ? &pgxpRamData_[phys >> 2] : nullptr;
    }
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE)
      return &pgxpScratchpad_[(phys - SCRATCHPAD_BASE) >> 2];
    return nullptr;
  }

  [[gnu::always_inline]] const PGXPValue *
  precisionSlot(uint32_t phys) const {
    if (phys < RAM_SIZE)
      return pgxpRamData_ ? &pgxpRamData_[phys >> 2] : nullptr;
    if (phys >= SCRATCHPAD_BASE && phys < SCRATCHPAD_BASE + SCRATCHPAD_SIZE)
      return &pgxpScratchpad_[(phys - SCRATCHPAD_BASE) >> 2];
    return nullptr;
  }

  // Hardware device pointers (non-owning)
  ps1::gpu::GPU *gpu_ = nullptr;
  spu::SPU *spu_ = nullptr;
  DMA *dma_ = nullptr;
  cdrom::CdromController *cdrom_ = nullptr;
  input::InputController *input_ = nullptr;
  mdec::MDEC *mdec_ = nullptr;
  Timers *timers_ = nullptr;
  InterruptController *irqCtrl_ = nullptr;

  // Write watchpoint (single address, optional)
  uint32_t watchpointPhysAddr_ = 0;
  std::function<void(uint32_t)> watchpointCb_;

public:
  /// Convert virtual address to physical address
  /// PS1 uses 3 mirrors: KUSEG (0x0), KSEG0 (0x80), KSEG1 (0xA0)
  /// PS1 also mirrors 2MB RAM across the first 8MB of physical address space.
  [[gnu::always_inline]] static uint32_t toPhysical(uint32_t addr) {
    // Strip KSEG0/KSEG1 bits
    // KSEG0: 0x80000000-0x9FFFFFFF -> mask off bit 31
    // KSEG1: 0xA0000000-0xBFFFFFFF -> mask off bits 31,29
    const uint32_t physSegment =
        addr >= 0x80000000u ? addr & 0x1FFFFFFFu : addr;
    uint32_t phys = physSegment;

    // PS1 RAM mirroring: 2MB is mirrored across the first 8MB
    // Physical addresses 0x000000-0x7FFFFF all map to the same 2MB.
    // This is critical for games that set SP to 0x807FFF00 (phys 0x7FFF00)
    // which must map to RAM offset 0x1FFF00.
    if (phys < 0x00800000) {
      phys &= (RAM_SIZE - 1); // phys & 0x1FFFFF
    }

    return phys;
  }
};

} // namespace ps1
