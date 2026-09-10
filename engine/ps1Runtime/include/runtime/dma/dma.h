#pragma once

// ps1Runtime -- DMA Controller
// PS1 DMA: 7 channels for high-speed data transfer between devices

#include <cstdint>
#include <cstring>
#include <functional>

namespace ps1 {

// Forward declarations
class Memory;

namespace gpu {
class GPU;
}
namespace spu {
class SPU;
}
namespace cdrom {
class CdromController;
}
namespace mdec {
class MDEC;
}

class DMA {
public:
  static constexpr uint32_t NUM_CHANNELS = 7;

  enum Channel : uint8_t {
    MDEC_IN = 0,
    MDEC_OUT = 1,
    GPU_CH = 2,
    CDROM_CH = 3,
    SPU_CH = 4,
    PIO = 5,
    OTC = 6,
  };

  // Transfer sync mode
  enum SyncMode : uint8_t {
    BURST = 0,       // Transfer all at once (manual start)
    SLICE = 1,       // Sync to DMA request, N words at a time
    LINKED_LIST = 2, // GPU linked-list mode (ch2 only)
  };

  struct ChannelRegs {
    uint32_t baseAddr = 0;       // MADR -- DMA base address
    uint32_t blockControl = 0;   // BCR -- block size/count
    uint32_t channelControl = 0; // CHCR -- channel control
  };

  struct Snapshot {
    ChannelRegs channels[NUM_CHANNELS]{};
    uint32_t dpcr = 0;
    uint32_t dicr = 0;
    uint32_t completionGeneration[NUM_CHANNELS]{};
  };

  DMA();
  void reset();
  Snapshot snapshot() const;
  void restore(const Snapshot &state);

  // Register access (0x1F801080-0x1F8010FF)
  void writeRegister(uint32_t addr, uint32_t val);
  uint32_t readRegister(uint32_t addr) const;

  // Attach hardware components
  void setMemory(Memory *mem) { mem_ = mem; }
  void setGPU(gpu::GPU *gpu) { gpu_ = gpu; }
  void setSPU(spu::SPU *spu) { spu_ = spu; }
  void setCDROM(cdrom::CdromController *cdrom) { cdrom_ = cdrom; }
  void setMDEC(mdec::MDEC *mdec) { mdec_ = mdec; }

  // Check for pending DMA interrupts
  bool hasInterrupt() const;

  // Monotonic per-channel completion count.  Hardware transfers execute
  // synchronously in this runtime, while PsyQ observes their IRQ callbacks at
  // cooperative game-thread yield points.  Bios uses this counter to bridge
  // those two timing domains without calling recompiled code re-entrantly
  // from a memory-mapped CHCR write.
  uint32_t completionGeneration(uint32_t ch) const {
    return ch < NUM_CHANNELS ? completionGeneration_[ch] : 0;
  }

  // Process pending DMA transfers
  void checkAndRunTransfers();

private:
  ChannelRegs channels_[NUM_CHANNELS];
  uint32_t dpcr_ = 0x07654321; // DMA Priority Control Register
  uint32_t dicr_ = 0;          // DMA Interrupt Control Register
  uint32_t completionGeneration_[NUM_CHANNELS] = {};

  Memory *mem_ = nullptr;
  gpu::GPU *gpu_ = nullptr;
  spu::SPU *spu_ = nullptr;
  cdrom::CdromController *cdrom_ = nullptr;
  mdec::MDEC *mdec_ = nullptr;

  // Per-channel transfer execution
  void executeChannel(uint32_t ch);
  void executeBlockTransfer(uint32_t ch);
  void executeLinkedListTransfer(uint32_t ch);
  void executeSliceTransfer(uint32_t ch);
  void executeOtcTransfer();

  // Channel enable check
  bool isChannelEnabled(uint32_t ch) const;
  bool isChannelTriggered(uint32_t ch) const;
  SyncMode getSyncMode(uint32_t ch) const;
  bool isFromRam(uint32_t ch) const; // direction: 0=to device, 1=from device

  // Interrupt handling
  void updateMasterInterrupt();
};

} // namespace ps1
