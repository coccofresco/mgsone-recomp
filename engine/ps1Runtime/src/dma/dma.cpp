#include "runtime/dma/dma.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/gpu/gpu.h"
#include "runtime/mdec/mdec.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/spu/spu.h"
#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>

namespace ps1 {

DMA::DMA() { reset(); }

void DMA::reset() {
  std::memset(channels_, 0, sizeof(channels_));
  std::memset(completionGeneration_, 0, sizeof(completionGeneration_));
  dpcr_ = 0x07654321; // TEST: reverted to check if bit11 was breaking DMA2
  dicr_ = 0;
}

DMA::Snapshot DMA::snapshot() const {
  Snapshot state;
  std::memcpy(state.channels, channels_, sizeof(channels_));
  state.dpcr = dpcr_;
  state.dicr = dicr_;
  std::memcpy(state.completionGeneration, completionGeneration_,
              sizeof(completionGeneration_));
  return state;
}

void DMA::restore(const Snapshot &state) {
  std::memcpy(channels_, state.channels, sizeof(channels_));
  dpcr_ = state.dpcr;
  dicr_ = state.dicr;
  std::memcpy(completionGeneration_, state.completionGeneration,
              sizeof(completionGeneration_));
}

// Register I/O

void DMA::writeRegister(uint32_t addr, uint32_t val) {
  uint32_t offset = addr - 0x1F801080;

  // DPCR / DICR
  if (offset == 0x70) {
    // On real PS1, the BIOS pre-enables GPU (Ch2) and OTC (Ch6) during init.
    // Our BIOS HLE does not call the real BIOS init path, so games that write
    // DPCR expecting those channels to already be enabled (e.g. for MDEC-only
    // init during FMV) would inadvertently disable GPU DMA.
    // Force the GPU (bit 11) and OTC (bit 27) enable bits to remain set.
    val |= (1u << 11); // Ch2 GPU -- always enabled
    val |= (1u << 27); // Ch6 OTC -- always enabled
    dpcr_ = val;
    ps1::metrics::count("dma.dpcr_write");
    ps1::metrics::setState("dma.dpcr", dpcr_);
    return;
  }
  if (offset == 0x74) {
    // DICR: bits 0-5 are force-IRQ, bits 16-22 are enable, bits 24-30 are flags
    // Writing 1 to flag bits clears them
    uint32_t flagAck = val & 0x7F000000;
    dicr_ = (dicr_ & ~0x7F000000) & ~flagAck; // Clear acknowledged flags
    dicr_ = (dicr_ & 0x7F000000) | (val & 0x00FF803F); // Set other bits
    updateMasterInterrupt();
    return;
  }

  // Per-channel registers
  if (offset < 0x70) {
    uint32_t ch = offset / 0x10;
    uint32_t reg = offset % 0x10;
    if (ch >= NUM_CHANNELS)
      return;

    switch (reg) {
    case 0x0:
      channels_[ch].baseAddr = val & 0x00FFFFFF;
      break;
    case 0x4:
      channels_[ch].blockControl = val;
      break;
    case 0x8:
      channels_[ch].channelControl = val;
      if (ch == SPU_CH) {
        ps1::metrics::count("dma.spu_chcr_write");
        ps1::metrics::setState("dma.spu_chcr", val);
        ps1::metrics::setState("dma.spu_enabled", isChannelEnabled(ch));
        if ((val >> 24) & 1)
          ps1::metrics::count("dma.spu_start_request");
      }
      // Debug: log Ch3 trigger attempts
      if (ch == CDROM_CH) {
        static int dma3TrigCount = 0;
        if (++dma3TrigCount <= 20) {
          bool triggered = isChannelTriggered(ch);
          fmt::print(stderr,
                     "[DMA3-DBG] #{} CHCR=0x{:08X} MADR=0x{:06X} BCR=0x{:08X} "
                     "triggered={} enabled={} sectorRdy={}\n",
                     dma3TrigCount, val, channels_[ch].baseAddr,
                     channels_[ch].blockControl, triggered,
                     isChannelEnabled(ch),
                     cdrom_ ? cdrom_->hasSectorReady() : false);
        }
      }
      if (ch == GPU_CH) {
        static const bool dbg = std::getenv("PS1_DMA2_DBG") != nullptr;
        if (dbg) {
          static int dma2TrigCount = 0;
          if (++dma2TrigCount <= 20) {
            fmt::print(stderr,
                       "[DMA2-DBG] #{} CHCR=0x{:08X} MADR=0x{:06X} "
                       "BCR=0x{:08X} sync={} fromRam={} caller={}\n",
                       dma2TrigCount, val, channels_[ch].baseAddr,
                       channels_[ch].blockControl,
                       static_cast<int>(getSyncMode(ch)), isFromRam(ch),
                       __builtin_return_address(0));
          }
        }
      }
      if (isChannelTriggered(ch)) {
        if (ch == SPU_CH)
          ps1::metrics::count("dma.spu_trigger");
        executeChannel(ch);
      }
      break;
    }
  }
}

uint32_t DMA::readRegister(uint32_t addr) const {
  uint32_t offset = addr - 0x1F801080;

  if (offset == 0x70)
    return dpcr_;
  if (offset == 0x74)
    return dicr_;

  if (offset < 0x70) {
    uint32_t ch = offset / 0x10;
    uint32_t reg = offset % 0x10;
    if (ch >= NUM_CHANNELS)
      return 0;

    switch (reg) {
    case 0x0:
      return channels_[ch].baseAddr;
    case 0x4:
      return channels_[ch].blockControl;
    case 0x8:
      return channels_[ch].channelControl;
    }
  }
  return 0;
}

// Channel Control

bool DMA::isChannelEnabled(uint32_t ch) const {
  return (dpcr_ >> (ch * 4 + 3)) & 1;
}

bool DMA::isChannelTriggered(uint32_t ch) const {
  if (!isChannelEnabled(ch))
    return false;
  uint32_t chcr = channels_[ch].channelControl;
  bool enable = (chcr >> 24) & 1;  // Start/Busy
  bool trigger = (chcr >> 28) & 1; // Manual trigger
  SyncMode sync = getSyncMode(ch);

  if (!enable)
    return false;
  if (sync == BURST)
    return trigger;
  return true;
}

DMA::SyncMode DMA::getSyncMode(uint32_t ch) const {
  return static_cast<SyncMode>((channels_[ch].channelControl >> 9) & 3);
}

bool DMA::isFromRam(uint32_t ch) const {
  return (channels_[ch].channelControl >> 0) & 1; // bit0: 0=to RAM, 1=from RAM
}

// Channel Execution

void DMA::executeChannel(uint32_t ch) {
  if (!mem_)
    return;

  SyncMode sync = getSyncMode(ch);

  // CDROM device->RAM: defer when no sector is buffered.  On a real PS1 the
  // DMA controller waits for a DRQ from the CDROM before transferring; we
  // emulate that by leaving the start bit set so a later retry (driven from
  // Bios::triggerCdromEvent INT1) will re-enter this function once the
  // sector is ready.  Previously we would copy a zeroed sector and clear
  // the start bit, dropping the read entirely on register-direct paths.
  if (ch == CDROM_CH && !isFromRam(ch) &&
      (!cdrom_ || !cdrom_->hasSectorReady())) {
    return;
  }

  fmt::print("[DMA] Ch{} transfer: sync={}, fromRam={}, addr=0x{:08X}\n", ch,
             static_cast<int>(sync), isFromRam(ch), channels_[ch].baseAddr);
  ps1::metrics::trace("dma.transfer",
                      {{"channel", ch},
                       {"sync", static_cast<int>(sync)},
                       {"from_ram", isFromRam(ch) ? 1 : 0},
                       {"address", channels_[ch].baseAddr},
                       {"block_control", channels_[ch].blockControl}});

  switch (sync) {
  case BURST:
  case SLICE:
    if (ch == OTC) {
      executeOtcTransfer();
    } else {
      executeBlockTransfer(ch);
    }
    break;
  case LINKED_LIST:
    executeLinkedListTransfer(ch);
    break;
  default:
    break;
  }

  // Clear start/busy and trigger bits after transfer
  channels_[ch].channelControl &= ~((1 << 24) | (1 << 28));

  // Set completion flag in DICR
  dicr_ |= (1 << (24 + ch));
  ++completionGeneration_[ch];
  updateMasterInterrupt();
}

void DMA::executeBlockTransfer(uint32_t ch) {
  if (!mem_)
    return;

  uint32_t addr = channels_[ch].baseAddr;
  uint32_t bcr = channels_[ch].blockControl;
  uint32_t blockSize = bcr & 0xFFFF;
  uint32_t blockCount = (bcr >> 16) & 0xFFFF;
  if (blockSize == 0)
    blockSize = 0x10000;
  if (blockCount == 0)
    blockCount = 1;

  uint32_t totalWords = blockSize * blockCount;
  if (ch == SPU_CH) {
    ps1::metrics::count("dma.spu_transfer");
    ps1::metrics::count("dma.spu_word", totalWords);
  }
  bool fromRam = isFromRam(ch);
  int32_t step = ((channels_[ch].channelControl >> 1) & 1) ? -4 : 4;

  struct GpuBlockBatchGuard {
    gpu::GPU *gpu = nullptr;
    explicit GpuBlockBatchGuard(gpu::GPU *owner) : gpu(owner) {
      if (gpu)
        gpu->beginCommandBatch();
    }
    ~GpuBlockBatchGuard() {
      if (gpu)
        gpu->endCommandBatch();
    }
  } gpuBatch(ch == GPU_CH && fromRam ? gpu_ : nullptr);

  uint8_t *ram = mem_->ramPtr();

  for (uint32_t i = 0; i < totalWords; i++) {
    uint32_t physAddr = addr & 0x1FFFFC;

    if (fromRam) {
      // RAM -> Device
      uint32_t word = ram[physAddr] | (ram[physAddr + 1] << 8) |
                      (ram[physAddr + 2] << 16) | (ram[physAddr + 3] << 24);

      switch (ch) {
      case GPU_CH:
        if (gpu_) {
          PGXPValue precision;
          const PGXPValue *precisionPtr =
              lookupPGXPSubmissionPrecision(mem_, physAddr, word, precision)
                  ? &precision
                  : nullptr;
          gpu_->writeGP0(word, physAddr, precisionPtr,
                         PGXPSubmissionPath::DmaBlock);
        }
        break;
      case SPU_CH:
        if (spu_) {
          // DMA4 uses the SPU transfer-address register as its destination.
          // Feeding the transfer FIFO also advances that address exactly as
          // manual writes through 0x1F801DA8 do.
          spu_->writeTransferData(word & 0xFFFF);
          spu_->writeTransferData((word >> 16) & 0xFFFF);
        }
        break;
      case MDEC_IN:
        if (mdec_)
          mdec_->writeCommand(word);
        break;
      default:
        break;
      }
    } else {
      // Device -> RAM
      uint32_t word = 0;

      switch (ch) {
      case CDROM_CH:
        if (cdrom_ && cdrom_->hasSectorReady()) {
          const uint8_t *sector = cdrom_->getSectorBuffer();
          uint32_t sectorSz = cdrom_->getSectorSize();
          // Skip raw sector header to reach user data payload.
          // Raw sector (2352 bytes): 12-sync + 4-header + 8-subheader +
          // 2048-data sectorSize=2048 -> user data at offset 24 sectorSize=2340
          // -> sub-header+data at offset 12
          uint32_t dataOff = (sectorSz == 2048) ? 24 : 12;
          uint32_t offset = dataOff + i * 4;
          if (offset + 3 < 2352) {
            word = sector[offset] | (sector[offset + 1] << 8) |
                   (sector[offset + 2] << 16) | (sector[offset + 3] << 24);
          }
        }
        break;
      case MDEC_OUT:
        if (mdec_ && mdec_->dmaOutReady())
          word = mdec_->dmaOutRead();
        break;
      default:
        break;
      }

      ram[physAddr] = word & 0xFF;
      ram[physAddr + 1] = (word >> 8) & 0xFF;
      ram[physAddr + 2] = (word >> 16) & 0xFF;
      ram[physAddr + 3] = (word >> 24) & 0xFF;
      // Device DMA bypasses Memory::write32().  Drop any PGXP sidecar even
      // when the incoming word happens to equal the previous RAM value: CD
      // streaming and MDEC output routinely recycle these buffers.
      mem_->invalidatePrecision(physAddr, sizeof(word));
    }

    addr += step;
  }

  if (ch == CDROM_CH && cdrom_) {
    cdrom_->clearSectorReady();
  }
}

void DMA::executeLinkedListTransfer(uint32_t ch) {
  if (ch != GPU_CH || !gpu_ || !mem_) {
    static int llSkip = 0;
    if (++llSkip <= 5)
      fmt::print(stderr, "[DMA] LL transfer SKIPPED: ch={}, gpu={}, mem={}\n",
                 ch, (void *)gpu_, (void *)mem_);
    return;
  }

  static int llCount = 0;
  if (++llCount <= 5)
    fmt::print(stderr, "[DMA] LL transfer #{}: addr=0x{:06X}, ramPtr={}\n",
               llCount, channels_[ch].baseAddr, (void *)mem_->ramPtr());

  // GPU linked-list: each node has a header word containing
  // [31:24] = number of words, [23:0] = next node address
  gpu_->processLinkedList(channels_[ch].baseAddr, mem_->ramPtr(), mem_);
}

void DMA::executeOtcTransfer() {
  if (!mem_)
    return;

  // OTC (Ordering Table Clear): fills memory backwards with linked list
  // pointers
  uint32_t addr = channels_[OTC].baseAddr;
  uint32_t count = channels_[OTC].blockControl & 0xFFFF;
  if (count == 0)
    count = 0x10000;

  uint8_t *ram = mem_->ramPtr();

  for (uint32_t i = 0; i < count; i++) {
    uint32_t physAddr = addr & 0x1FFFFC;
    uint32_t val;

    if (i == count - 1) {
      val = 0x00FFFFFF; // End marker
    } else {
      val = (addr - 4) & 0x00FFFFFF; // Previous entry
    }

    ram[physAddr] = val & 0xFF;
    ram[physAddr + 1] = (val >> 8) & 0xFF;
    ram[physAddr + 2] = (val >> 16) & 0xFF;
    ram[physAddr + 3] = 0; // Number of words = 0
    // OTC also writes behind Memory's tracked accessors.  A same-value link
    // must not retain provenance from a primitive that occupied this slot.
    mem_->invalidatePrecision(physAddr, sizeof(val));

    addr -= 4;
  }
}

void DMA::checkAndRunTransfers() {
  for (uint32_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (isChannelTriggered(ch)) {
      executeChannel(ch);
    }
  }
}

bool DMA::hasInterrupt() const {
  return (dicr_ >> 31) & 1; // Master IRQ flag
}

void DMA::updateMasterInterrupt() {
  bool forceIrq = (dicr_ >> 15) & 1;
  uint8_t enableMask = (dicr_ >> 16) & 0x7F;
  uint8_t flagMask = (dicr_ >> 24) & 0x7F;

  bool masterFlag = forceIrq || ((enableMask & flagMask) != 0);
  if (masterFlag) {
    dicr_ |= (1u << 31);
  } else {
    dicr_ &= ~(1u << 31);
  }
}

} // namespace ps1
