#include "runtime/memory_access.h"

#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/mdec/mdec.h"
#include "runtime/spu/spu.h"
#include "runtime/timers/timers.h"

namespace ps1 {

uint8_t Memory::read8Slow(uint32_t addr, uint32_t phys) const {
  (void)addr;
  if (phys >= 0x1F801080 && phys < 0x1F8010F8)
    return dma_ ? dma_->readRegister(phys & ~3u) >> ((phys & 3u) * 8) : 0;
  if (phys >= 0x1F801100 && phys < 0x1F801130)
    return timers_ ? timers_->readRegister(phys & ~3u) >> ((phys & 3u) * 8) : 0;
  if (phys >= 0x1F801040 && phys < 0x1F801050)
    return input_ ? input_->readRegister(phys & ~1u) >> ((phys & 1u) * 8) : 0;
  if (phys >= 0x1F801800 && phys < 0x1F801804)
    return cdrom_ ? cdrom_->readRegister(phys) : 0;
  return 0;
}

uint16_t Memory::read16Slow(uint32_t addr, uint32_t phys) const {
  (void)addr;
  if (phys >= 0x1F801C00 && phys < 0x1F802000)
    return spu_ ? spu_->readRegister(phys) : 0;
  if (phys >= 0x1F801100 && phys < 0x1F801130)
    return timers_ ? timers_->readRegister(phys & ~3u) >> ((phys & 3u) * 8) : 0;
  if (phys >= 0x1F801080 && phys < 0x1F8010F8)
    return dma_ ? dma_->readRegister(phys & ~3u) >> ((phys & 3u) * 8) : 0;
  if (phys >= 0x1F801040 && phys < 0x1F801050)
    return input_ ? input_->readRegister16(phys) : 0;
  if (phys == 0x1F801070)
    return irqCtrl_ ? irqCtrl_->readIStat() & 0xFFFFu : 0;
  if (phys == 0x1F801074)
    return irqCtrl_ ? irqCtrl_->readIMask() & 0xFFFFu : 0;
  if (phys >= 0x1F801800 && phys < 0x1F801804) {
    // The CD bus is byte-wide: consecutive ports, not repeated FIFO reads.
    const uint16_t lo = read8(addr);
    const uint16_t hi = read8(addr + 1);
    return lo | (hi << 8);
  }
  return 0;
}

uint32_t Memory::read32Slow(uint32_t addr, uint32_t phys) const {
  if (phys == 0x1F801810)
    return gpu_ ? gpu_->readGPUREAD() : 0;
  if (phys == 0x1F801814)
    return gpu_ ? gpu_->readGPUSTAT() : 0;
  if (phys >= 0x1F801080 && phys <= 0x1F8010F4)
    return dma_ ? dma_->readRegister(phys) : 0;
  if (phys >= 0x1F801100 && phys < 0x1F801130)
    return timers_ ? timers_->readRegister(phys) : 0;
  if (phys == 0x1F801070)
    return irqCtrl_ ? irqCtrl_->readIStat() : 0;
  if (phys == 0x1F801074)
    return irqCtrl_ ? irqCtrl_->readIMask() : 0;
  if (phys == 0x1F801820)
    return mdec_ ? mdec_->readData() : 0;
  if (phys == 0x1F801824)
    return mdec_ ? mdec_->readStatus() : 0;
  if (phys >= 0x1F801040 && phys < 0x1F801050)
    return input_ ? input_->readRegister(phys) : 0;
  if (phys >= 0x1F801C00 && phys < 0x1F802000) {
    const uint16_t lo = spu_ ? spu_->readRegister(phys) : 0;
    const uint16_t hi = spu_ ? spu_->readRegister(phys + 2) : 0;
    return lo | (static_cast<uint32_t>(hi) << 16);
  }
  if (phys >= 0x1F801000 && phys < 0x1F801040)
    return 0;
  if (phys == 0x1F801060)
    return 0x00000B88;
  if (phys == 0xFFFE0130)
    return 0;

  const uint8_t b0 = read8(addr);
  const uint8_t b1 = read8(addr + 1);
  const uint8_t b2 = read8(addr + 2);
  const uint8_t b3 = read8(addr + 3);
  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
         (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

void Memory::write8Slow(uint32_t addr, uint32_t phys, uint8_t value) {
  (void)addr;
  if (phys >= 0x1F801080 && phys < 0x1F8010F8 && dma_) {
    const uint32_t shift = (phys & 3u) * 8;
    dma_->writeRegister(phys & ~3u, uint32_t(value) << shift, 0xFFu << shift);
    return;
  }
  if (phys >= 0x1F801040 && phys < 0x1F801050) {
    if (input_)
      input_->writeRegister(phys & ~1u, uint32_t(value) << ((phys & 1u) * 8));
    return;
  }
  if (phys >= 0x1F801800 && phys < 0x1F801804 && cdrom_)
    cdrom_->writeRegister(phys, value);
}

void Memory::write16Slow(uint32_t addr, uint32_t phys, uint16_t value) {
  if (phys >= 0x1F801080 && phys < 0x1F8010F8) {
    // Keep the runtime's existing masked DMA-write contract, but publish the
    // complete halfword before checking CHCR. Never execute between its bytes.
    const uint32_t shift = (phys & 3u) * 8;
    if (dma_)
      dma_->writeRegister(phys & ~3u, uint32_t(value) << shift, 0xFFFFu << shift);
    return;
  }
  if (phys >= 0x1F801C00 && phys < 0x1F802000) {
    if (spu_)
      spu_->writeRegister(phys, value);
    return;
  }
  if (phys >= 0x1F801100 && phys < 0x1F801130) {
    if (timers_)
      timers_->writeRegister(phys, value);
    return;
  }
  if (phys >= 0x1F801040 && phys < 0x1F801050) {
    if (input_)
      input_->writeRegister16(phys, value);
    return;
  }
  if (phys == 0x1F801070) {
    if (irqCtrl_)
      irqCtrl_->writeIStat(value);
    return;
  }
  if (phys == 0x1F801074) {
    if (irqCtrl_)
      irqCtrl_->writeIMask(value);
    return;
  }
  write8(addr, static_cast<uint8_t>(value & 0xFFu));
  write8(addr + 1, static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void Memory::write32Slow(uint32_t addr, uint32_t phys, uint32_t value) {
  if (phys >= 0x1F801100 && phys < 0x1F801130) {
    if (timers_)
      timers_->writeRegister(phys, static_cast<uint16_t>(value));
    return;
  }
  if (phys == 0x1F801810) {
    if (gpu_)
      gpu_->writeGP0(value);
    return;
  }
  if (phys == 0x1F801814) {
    if (gpu_)
      gpu_->writeGP1(value);
    return;
  }
  if (phys >= 0x1F801080 && phys <= 0x1F8010F4) {
    if (dma_)
      dma_->writeRegister(phys, value);
    return;
  }
  if (phys == 0x1F801070) {
    if (irqCtrl_)
      irqCtrl_->writeIStat(value);
    return;
  }
  if (phys == 0x1F801074) {
    if (irqCtrl_)
      irqCtrl_->writeIMask(value);
    return;
  }
  if (phys == 0x1F801820) {
    if (mdec_)
      mdec_->writeCommand(value);
    return;
  }
  if (phys == 0x1F801824) {
    if (mdec_)
      mdec_->writeControl(value);
    return;
  }
  if (phys >= 0x1F801040 && phys < 0x1F801050) {
    if (input_)
      input_->writeRegister(phys, value);
    return;
  }
  if (phys >= 0x1F801800 && phys < 0x1F801804) {
    // Index changes in the first lane apply to the following ports, including
    // audio matrix staging/application and IRQ acknowledgement.
    for (unsigned lane = 0; lane < 4; ++lane)
      write8(addr + lane, static_cast<uint8_t>(value >> (lane * 8)));
    return;
  }
  if (phys >= 0x1F801C00 && phys < 0x1F802000) {
    if (spu_) {
      spu_->writeRegister(phys, value & 0xFFFFu);
      spu_->writeRegister(phys + 2, (value >> 16) & 0xFFFFu);
    }
    return;
  }
  if ((phys >= 0x1F801000 && phys < 0x1F801040) ||
      phys == 0x1F801060 || phys == 0xFFFE0130) {
    return;
  }

  write8(addr, static_cast<uint8_t>(value & 0xFFu));
  write8(addr + 1, static_cast<uint8_t>((value >> 8) & 0xFFu));
  write8(addr + 2, static_cast<uint8_t>((value >> 16) & 0xFFu));
  write8(addr + 3, static_cast<uint8_t>((value >> 24) & 0xFFu));
}

void Memory::write32TrackedSlow(uint32_t addr, uint32_t phys, uint32_t value,
                                const PGXPValue *precision) {
  if (phys == 0x1F801810) {
    if (gpu_) {
      if (precision && precision->hasXY(value)) {
        countPGXPStage(PGXPStage::Gp0WordXY);
        if (precision->hasXYZ(value))
          countPGXPStage(PGXPStage::Gp0WordXYZ);
        else if (precision->hasSignedProjection(value))
          countPGXPStage(PGXPStage::Gp0WordSigned);
      }
      gpu_->writeGP0(value, UINT32_MAX,
                     precision && precision->hasXY(value) ? precision
                                                           : nullptr);
    }
    return;
  }
  write32(addr, value);
  if (precision && precision->hasXY(value))
    storePrecision(addr, value, *precision);
}

} // namespace ps1
