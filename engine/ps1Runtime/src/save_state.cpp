#include "runtime/save_state.h"
#include "runtime/bios/bios.h"
#include "runtime/games/crash3_profile.h"
#include "runtime/psyq/psyq_pad.h"
#include "runtime/psyq/psyq_state.h"
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <limits>
#include <type_traits>

namespace ps1 {

namespace {

constexpr uint32_t kExtendedStateMarker = 0x32545845u; // "EXT2"
constexpr uint32_t kBiosStateMarker = 0x34534F49u;     // "IOS4"
constexpr uint32_t kMaxSerializedVectorElements = 1u << 20;

template <typename T> void writePod(std::ofstream &f, const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  f.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <typename T> bool readPod(std::ifstream &f, T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return static_cast<bool>(
      f.read(reinterpret_cast<char *>(&value), sizeof(value)));
}

template <typename T>
void writeVector(std::ofstream &f, const std::vector<T> &values) {
  const uint32_t count = static_cast<uint32_t>(values.size());
  writePod(f, count);
  if (count != 0)
    f.write(reinterpret_cast<const char *>(values.data()),
            static_cast<std::streamsize>(count * sizeof(T)));
}

template <typename T>
bool readVector(std::ifstream &f, std::vector<T> &values) {
  uint32_t count = 0;
  if (!readPod(f, count) || count > kMaxSerializedVectorElements)
    return false;
  values.resize(count);
  return count == 0 || static_cast<bool>(f.read(
                           reinterpret_cast<char *>(values.data()),
                           static_cast<std::streamsize>(count * sizeof(T))));
}

struct PsyqDiskState {
  uint32_t vsyncCounter = 0;
  uint32_t lastVSyncCounter = 0;
  uint32_t pendingVblanks = 0;
  uint8_t cdSyncByte = 0;
  uint8_t cdReadyByte = 0;
  uint32_t cdRemaining = 0;
  uint32_t cdDestPtr = 0;
  uint32_t cdWordCount = 0;
  uint32_t cdDataCb = 0;
  uint32_t cdNotifyCb = 0;
  uint32_t gpuSwapCb = 0;
  uint16_t intrMask = 0;
  uint32_t intrCallback[psyq::PsyqState::kIntrSlots]{};
  uint32_t dmaCallback[psyq::PsyqState::kIntrSlots]{};
  bool callbacksEnabled = true;
  psyq::GpuDrawSync drawSync{};
};

PsyqDiskState capturePsyqState() {
  const auto &source = psyq::psyq_state();
  PsyqDiskState state;
  state.vsyncCounter = source.vsyncCounter.load(std::memory_order_acquire);
  state.lastVSyncCounter = source.lastVSyncCounter;
  state.pendingVblanks = source.pendingVblanks.load(std::memory_order_acquire);
  state.cdSyncByte = source.cdSyncByte.load(std::memory_order_acquire);
  state.cdReadyByte = source.cdReadyByte.load(std::memory_order_acquire);
  state.cdRemaining = source.cdRemaining;
  state.cdDestPtr = source.cdDestPtr;
  state.cdWordCount = source.cdWordCount;
  state.cdDataCb = source.cdDataCb;
  state.cdNotifyCb = source.cdNotifyCb;
  state.gpuSwapCb = source.gpuSwapCb;
  state.intrMask = source.intrMask;
  std::copy(std::begin(source.intrCallback), std::end(source.intrCallback),
            std::begin(state.intrCallback));
  std::copy(std::begin(source.dmaCallback), std::end(source.dmaCallback),
            std::begin(state.dmaCallback));
  state.callbacksEnabled = source.callbacksEnabled;
  state.drawSync = source.drawSync;
  return state;
}

void restorePsyqState(const PsyqDiskState &state) {
  auto &destination = psyq::psyq_state();
  destination.vsyncCounter.store(state.vsyncCounter, std::memory_order_release);
  destination.lastVSyncCounter = state.lastVSyncCounter;
  destination.pendingVblanks.store(state.pendingVblanks,
                                   std::memory_order_release);
  // Checkpoints are taken at a completed VSync continuation, never inside an
  // outstanding host wait. Derive the persistent pacing mode from whether a
  // blocking completion has occurred, and discard transient wait ownership
  // from the abandoned host timeline.
  destination.blockingVSyncPacing.store(state.lastVSyncCounter != 0,
                                        std::memory_order_release);
  destination.blockingVSyncTarget.store(0, std::memory_order_release);
  destination.autonomousClockWaiters.store(0, std::memory_order_release);
  destination.cdSyncByte.store(state.cdSyncByte, std::memory_order_release);
  destination.cdReadyByte.store(state.cdReadyByte, std::memory_order_release);
  destination.cdRemaining = state.cdRemaining;
  destination.cdClockDemand.store(state.cdRemaining != 0,
                                  std::memory_order_release);
  destination.cdDestPtr = state.cdDestPtr;
  destination.cdWordCount = state.cdWordCount;
  destination.cdDataCb = state.cdDataCb;
  destination.cdNotifyCb = state.cdNotifyCb;
  destination.gpuSwapCb = state.gpuSwapCb;
  destination.intrMask = state.intrMask;
  std::copy(std::begin(state.intrCallback), std::end(state.intrCallback),
            std::begin(destination.intrCallback));
  std::copy(std::begin(state.dmaCallback), std::end(state.dmaCallback),
            std::begin(destination.dmaCallback));
  destination.callbacksEnabled = state.callbacksEnabled;
  destination.drawSync = state.drawSync;
}

} // namespace

bool SaveStateCoordinator::request(Action action, std::string path) {
  std::lock_guard lock(mutex_);
  if (cancelled_ || request_.has_value())
    return false;
  request_ = Request{action, std::move(path)};
  operationComplete_ = false;
  reloadTimeline_ = false;
  return true;
}

void SaveStateCoordinator::checkpoint(recomp_context &ctx, uint32_t resumePc) {
  std::unique_lock lock(mutex_);
  if (!request_.has_value() || cancelled_)
    return;

  ctx.pc = resumePc;
  gameThreadParked_ = true;
  ready_.notify_one();
  resumed_.wait(lock, [&] { return operationComplete_ || cancelled_; });
  const bool reload = reloadTimeline_ && !cancelled_;
  gameThreadParked_ = false;
  request_.reset();
  operationComplete_ = false;
  reloadTimeline_ = false;
  lock.unlock();
  if (reload)
    throw SaveStateTimelineReload{};
}

bool SaveStateCoordinator::service(
    const std::function<bool(const Request &)> &operation) {
  Request current;
  {
    std::lock_guard lock(mutex_);
    if (!gameThreadParked_ || !request_.has_value() || operationComplete_)
      return false;
    current = *request_;
  }

  const bool succeeded = operation(current);
  {
    std::lock_guard lock(mutex_);
    reloadTimeline_ = succeeded && current.action == Action::Load;
    operationComplete_ = true;
  }
  resumed_.notify_one();
  return true;
}

bool SaveStateCoordinator::pending() const {
  std::lock_guard lock(mutex_);
  return request_.has_value();
}

void SaveStateCoordinator::cancel() {
  {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    operationComplete_ = true;
  }
  resumed_.notify_all();
  ready_.notify_all();
}

void SaveState::writeBytes(std::ofstream &f, const void *data, size_t size) {
  f.write(reinterpret_cast<const char *>(data), size);
}

bool SaveState::readBytes(std::ifstream &f, void *data, size_t size) {
  f.read(reinterpret_cast<char *>(data), size);
  return f.gcount() == static_cast<std::streamsize>(size);
}

uint32_t SaveState::computeChecksum(const void *data, size_t size) {
  uint32_t checksum = 0;
  const auto *bytes = reinterpret_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; i++) {
    checksum ^= static_cast<uint32_t>(bytes[i]) << ((i % 4) * 8);
  }
  return checksum;
}

bool SaveState::save(const std::string &path, const recomp_context &ctx,
                     const Memory &mem, const gpu::GPU &gpu,
                     const spu::SPU &spu, const DMA &dma,
                     const cdrom::CdromController &cdrom, const Timers &timers,
                     const InterruptController &irq,
                     const input::InputController &input,
                     const mdec::MDEC *mdec, const bios::Bios *biosHle) {
  if (biosHle && biosHle->hasNativeThreadContinuations()) {
    fmt::print(stderr, "[SaveState] Native BIOS thread stacks are not serializable\n");
    return false;
  }
  const std::string temporaryPath = path + ".tmp";
  std::ofstream f(temporaryPath, std::ios::binary | std::ios::trunc);
  if (!f) {
    fmt::print(stderr, "[SaveState] Cannot open '{}' for writing\n", path);
    return false;
  }

  // Write header placeholder (we'll update checksum at the end)
  SaveStateHeader header;
  auto headerPos = f.tellp();
  writeBytes(f, &header, sizeof(header));

  // CPU Context
  // GPRs (32 x 4 bytes = 128 bytes)
  writeBytes(f, ctx.r, sizeof(ctx.r));
  // HI, LO, PC
  writeBytes(f, &ctx.hi, sizeof(ctx.hi));
  writeBytes(f, &ctx.lo, sizeof(ctx.lo));
  writeBytes(f, &ctx.pc, sizeof(ctx.pc));
  // COP0 (16 x 4 bytes)
  writeBytes(f, ctx.cop0, sizeof(ctx.cop0));
  // COP2 data + control (64 x 4 bytes)
  writeBytes(f, ctx.cop2d, sizeof(ctx.cop2d));
  writeBytes(f, ctx.cop2c, sizeof(ctx.cop2c));

  // RAM (2MB)
  writeBytes(f, mem.ramPtr(), Memory::RAM_SIZE);

  // Scratchpad (1KB)
  writeBytes(f, mem.scratchpadPtr(), Memory::SCRATCHPAD_SIZE);

  // GPU VRAM (1024x512x2 = 1MB)
  writeBytes(f, gpu.getVRAM(), 1024 * 512 * sizeof(gpu::Color16));

  // GPU State
  uint32_t gpustat = gpu.readGPUSTAT();
  writeBytes(f, &gpustat, sizeof(gpustat));

  // SPU Sound RAM (512KB)
  writeBytes(f, spu.soundRamPtr(), 512 * 1024);

  // SPU Registers snapshot
  // Save key registers (SPUCNT, master volume, key on/off state)
  uint16_t spuCnt = spu.readRegister(0x1F801DAA);
  writeBytes(f, &spuCnt, sizeof(spuCnt));

  // DMA State
  // Save DPCR and DICR
  uint32_t dpcr = dma.readRegister(0x1F8010F0);
  uint32_t dicr = dma.readRegister(0x1F8010F4);
  writeBytes(f, &dpcr, sizeof(dpcr));
  writeBytes(f, &dicr, sizeof(dicr));
  // Save all 7 channel registers (base, block, chcr)
  for (uint32_t ch = 0; ch < 7; ch++) {
    uint32_t base = dma.readRegister(0x1F801080 + ch * 0x10);
    uint32_t block = dma.readRegister(0x1F801084 + ch * 0x10);
    uint32_t chcr = dma.readRegister(0x1F801088 + ch * 0x10);
    writeBytes(f, &base, sizeof(base));
    writeBytes(f, &block, sizeof(block));
    writeBytes(f, &chcr, sizeof(chcr));
  }

  // IRQ State
  uint32_t istat = irq.readIStat();
  uint32_t imask = irq.readIMask();
  writeBytes(f, &istat, sizeof(istat));
  writeBytes(f, &imask, sizeof(imask));

  // Version 2 extension. The fixed v1 prefix remains readable for old files;
  // new state which matters to streamed games follows an explicit marker.
  writePod(f, kExtendedStateMarker);
  const gpu::GPU::GuestState gpuState = gpu.guestState();
  writePod(f, gpuState);
  const DMA::Snapshot dmaState = dma.snapshot();
  writePod(f, dmaState);
  for (uint32_t timer = 0; timer < 3; ++timer) {
    const uint32_t base = 0x1F801100u + timer * 0x10u;
    const uint16_t mode = timers.readRegister(base + 4u);
    const uint16_t target = timers.readRegister(base + 8u);
    const uint16_t counter = timers.readRegister(base);
    writePod(f, mode);
    writePod(f, target);
    writePod(f, counter);
  }

  const cdrom::CdromController::Snapshot cd = cdrom.snapshot();
  writePod(f, cd.state);
  writePod(f, cd.indexReg);
  writeVector(f, cd.paramFifo);
  writeVector(f, cd.responseFifo);
  writePod(f, cd.interruptFlag);
  writePod(f, cd.interruptEnable);
  writePod(f, cd.irqLinePending);
  writePod(f, cd.setLocMinutes);
  writePod(f, cd.setLocSeconds);
  writePod(f, cd.setLocSector);
  writePod(f, cd.currentLba);
  writePod(f, cd.seekTarget);
  writePod(f, cd.mode);
  writePod(f, cd.sectorBuffer);
  writePod(f, cd.sectorSize);
  writePod(f, cd.dataReadOffset);
  writePod(f, cd.sectorReady);
  writePod(f, cd.cyclesUntilResponse);
  writePod(f, cd.cyclesPerSector);
  writePod(f, cd.readCycleCounter);
  writePod(f, cd.waitingForAck);
  writePod(f, cd.pendingClockCycles);
  writePod(f, cd.xaFilterFile);
  writePod(f, cd.xaFilterChannel);
  writePod(f, cd.xaFilterEnabled);
  writePod(f, cd.motorOn);
  writePod(f, cd.shellOpen);
  writePod(f, cd.pendingCommand);
  writePod(f, cd.commandPending);
  writePod(f, cd.hasSecondaryResponse);
  writePod(f, cd.secondaryResponseDelay);
  writePod(f, cd.secondaryInterrupt);
  writeVector(f, cd.secondaryData);

  const spu::SPU::Snapshot spuState = spu.snapshot();
  writePod(f, spuState.voices);
  writePod(f, spuState.spuCtrl);
  writePod(f, spuState.spuStat);
  writePod(f, spuState.mainVolL);
  writePod(f, spuState.mainVolR);
  writePod(f, spuState.irqAddr);
  writePod(f, spuState.irqPending);
  writePod(f, spuState.keyOnLatch);
  writePod(f, spuState.keyOffLatch);
  writePod(f, spuState.noiseOn);
  writePod(f, spuState.pitchModOn);
  writePod(f, spuState.reverbOn);
  writePod(f, spuState.endxFlags);
  writePod(f, spuState.transferAddr);
  writePod(f, spuState.transferCtrl);
  writePod(f, spuState.reverb);
  writePod(f, spuState.noiseLevel);
  writePod(f, spuState.noiseTimer);
  writePod(f, spuState.noiseStep);
  writeVector(f, spuState.xaBuffer);
  writeVector(f, spuState.cdDaBuffer);
  writePod(f, spuState.xaReadPos);
  writePod(f, spuState.cdDaReadPos);
  writePod(f, spuState.generatedSampleCount);
  writePod(f, spuState.firstNonzeroOutputSeen);

  const mdec::MDEC::Snapshot mdecState =
      mdec != nullptr ? mdec->snapshot() : mdec::MDEC::Snapshot{};
  writePod(f, mdecState.state);
  writePod(f, mdecState.busy);
  writePod(f, mdecState.currentCommand);
  writePod(f, mdecState.outputDepth24);
  writePod(f, mdecState.outputSigned);
  writePod(f, mdecState.remainingWords);
  writePod(f, mdecState.lumaQuantTable);
  writePod(f, mdecState.chromaQuantTable);
  writePod(f, mdecState.hasCustomQuantTable);
  writeVector(f, mdecState.inputBuffer);
  writeVector(f, mdecState.outputBuffer);
  writePod(f, mdecState.block);

  const input::InputController::Snapshot inputState = input.snapshot();
  writePod(f, inputState);

  for (int slot = 0; slot < 2; ++slot) {
    const input::MemoryCard::Snapshot card =
        input.getMemoryCard(slot).snapshot();
    writePod(f, card);
  }

  const PsyqDiskState psyqState = capturePsyqState();
  writePod(f, psyqState);
  const psyq::PadModuleSnapshot padState = psyq::psyq_pad_snapshot();
  writePod(f, padState);

  // Version 4: the host-side BIOS HLE is part of the emulated timeline.
  // Delivery epochs and queued callbacks must rewind together with hardware
  // generations; otherwise a uint32 rewind looks like billions of new DMA
  // completions after F9.
  writePod(f, kBiosStateMarker);
  const bool hasBiosState = biosHle != nullptr;
  writePod(f, hasBiosState);
  if (hasBiosState) {
    const bios::Bios::Snapshot biosState = biosHle->snapshot();
    writeVector(f, biosState.eventSystem.events);
    writePod(f, biosState.eventSystem.triggeredBits);
    writeVector(f, biosState.eventSystem.pendingCallbacks);
    writePod(f, biosState.padBuf1Addr);
    writePod(f, biosState.padBuf1Size);
    writePod(f, biosState.padBuf2Addr);
    writePod(f, biosState.padBuf2Size);
    writePod(f, biosState.padActive);
    writePod(f, biosState.cdIntPending);
    writeVector(f, biosState.cdEventQueue);
    writePod(f, biosState.cdExceptionPending);
    writePod(f, biosState.lastIntrTickFrame);
    writePod(f, biosState.lastDmaCompletion);
    writePod(f, biosState.customExceptionBuffer);
  }

  if (!f.good()) {
    fmt::print(stderr, "[SaveState] Short write to '{}'\n", path);
    return false;
  }

  // Update header with checksum
  // Compute checksum of RAM only (for speed)
  header.checksum = computeChecksum(mem.ramPtr(), Memory::RAM_SIZE);
  f.seekp(headerPos);
  writeBytes(f, &header, sizeof(header));

  f.close();
  std::error_code renameError;
  std::filesystem::rename(temporaryPath, path, renameError);
  if (renameError) {
    fmt::print(stderr, "[SaveState] Cannot publish '{}': {}\n", path,
               renameError.message());
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    return false;
  }
  fmt::print("[SaveState] Saved to '{}'\n", path);
  return true;
}

bool SaveState::load(const std::string &path, recomp_context &ctx, Memory &mem,
                     gpu::GPU &gpu, spu::SPU &spu, DMA &dma,
                     cdrom::CdromController &cdrom, Timers &timers,
                     InterruptController &irq, input::InputController &input,
                     mdec::MDEC *mdec, bios::Bios *biosHle) {
  if (biosHle && biosHle->hasNativeThreadContinuations()) {
    fmt::print(stderr, "[SaveState] Native BIOS thread stacks are not serializable\n");
    return false;
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fmt::print(stderr, "[SaveState] Cannot open '{}' for reading\n", path);
    return false;
  }

  // Read header
  SaveStateHeader header;
  readBytes(f, &header, sizeof(header));

  if (header.magic[0] != 'P' || header.magic[1] != 'S' ||
      header.magic[2] != '1' || header.magic[3] != 'S') {
    fmt::print(stderr, "[SaveState] Invalid magic in '{}'\n", path);
    return false;
  }
  if (header.version != 1 && header.version != 2 && header.version != 3 &&
      header.version != 4 && header.version != 5) {
    fmt::print(stderr, "[SaveState] Unsupported version {} in '{}'\n",
               header.version, path);
    return false;
  }

  // CPU Context
  readBytes(f, ctx.r, sizeof(ctx.r));
  readBytes(f, &ctx.hi, sizeof(ctx.hi));
  readBytes(f, &ctx.lo, sizeof(ctx.lo));
  readBytes(f, &ctx.pc, sizeof(ctx.pc));
  readBytes(f, ctx.cop0, sizeof(ctx.cop0));
  readBytes(f, ctx.cop2d, sizeof(ctx.cop2d));
  readBytes(f, ctx.cop2c, sizeof(ctx.cop2c));
  ctx.nativeResumePc = 0;
  ctx.nativeEntryPc = 0;
  // Version 1 states predate the non-architectural PGXP side channels.  Never
  // let precision from the timeline being replaced attach to restored words.
  std::memset(ctx.pgxpGpr, 0, sizeof(ctx.pgxpGpr));
  std::memset(ctx.pgxpGteData, 0, sizeof(ctx.pgxpGteData));
  std::memset(ctx.gteDataSourceToken, 0, sizeof(ctx.gteDataSourceToken));
  std::memset(ctx.gteControlSourceToken, 0, sizeof(ctx.gteControlSourceToken));
  std::memset(ctx.gteControlWriterPc, 0, sizeof(ctx.gteControlWriterPc));
  std::memset(ctx.gteControlWriteGeneration, 0,
              sizeof(ctx.gteControlWriteGeneration));
  ctx.gteControlWriteSerial = 0;

  // RAM (2MB)
  readBytes(f, mem.ramPtr(), Memory::RAM_SIZE);

  // Scratchpad (1KB)
  readBytes(f, mem.scratchpadPtr(), Memory::SCRATCHPAD_SIZE);
  mem.clearPrecision();
  games::crash3::restoreTransientState(mem);

  // GPU VRAM (1024x512x2 = 1MB)
  std::vector<uint8_t> vramData(1024 * 512 * sizeof(gpu::Color16));
  readBytes(f, vramData.data(), vramData.size());
  gpu.loadVram(vramData.data());

  // GPU State
  uint32_t gpustat;
  readBytes(f, &gpustat, sizeof(gpustat));

  // SPU Sound RAM (512KB)
  std::vector<uint8_t> spuRam(spu::SOUND_RAM_SIZE);
  readBytes(f, spuRam.data(), spuRam.size());
  spu.loadSoundRam(spuRam.data());

  // SPU Registers
  uint16_t spuCnt;
  readBytes(f, &spuCnt, sizeof(spuCnt));

  // DMA State
  uint32_t dpcr, dicr;
  readBytes(f, &dpcr, sizeof(dpcr));
  readBytes(f, &dicr, sizeof(dicr));
  struct LegacyDmaChannel {
    uint32_t base = 0;
    uint32_t block = 0;
    uint32_t chcr = 0;
  } legacyDma[7];
  for (uint32_t ch = 0; ch < 7; ch++) {
    readBytes(f, &legacyDma[ch].base, sizeof(legacyDma[ch].base));
    readBytes(f, &legacyDma[ch].block, sizeof(legacyDma[ch].block));
    readBytes(f, &legacyDma[ch].chcr, sizeof(legacyDma[ch].chcr));
  }
  if (header.version == 1) {
    dma.writeRegister(0x1F8010F0, dpcr);
    dma.writeRegister(0x1F8010F4, dicr);
    for (uint32_t ch = 0; ch < 7; ch++) {
      dma.writeRegister(0x1F801080 + ch * 0x10, legacyDma[ch].base);
      dma.writeRegister(0x1F801084 + ch * 0x10, legacyDma[ch].block);
      dma.writeRegister(0x1F801088 + ch * 0x10, legacyDma[ch].chcr);
    }
  }

  // IRQ State
  uint32_t istat, imask;
  readBytes(f, &istat, sizeof(istat));
  readBytes(f, &imask, sizeof(imask));
  irq.writeIMask(imask);
  // Restore I_STAT by raising all bits that were set
  // First clear, then raise
  irq.writeIStat(0);
  for (uint32_t bit = 0; bit < 11; bit++) {
    if (istat & (1u << bit))
      irq.raiseInterrupt(1u << bit);
  }

  bool biosTimelineRestored = false;
  if (header.version >= 2) {
    uint32_t marker = 0;
    gpu::GPU::GuestState gpuState;
    DMA::Snapshot dmaState;
    if (!readPod(f, marker) || marker != kExtendedStateMarker ||
        !readPod(f, gpuState) || !readPod(f, dmaState)) {
      fmt::print(stderr, "[SaveState] Truncated extended state in '{}'\n",
                 path);
      return false;
    }

    struct TimerDiskState {
      uint16_t mode = 0;
      uint16_t target = 0;
      uint16_t counter = 0;
    } timerState[3];
    for (auto &timer : timerState) {
      if (!readPod(f, timer.mode) || !readPod(f, timer.target) ||
          !readPod(f, timer.counter))
        return false;
    }

    cdrom::CdromController::Snapshot cd;
    bool cdValid =
        readPod(f, cd.state) && readPod(f, cd.indexReg) &&
        readVector(f, cd.paramFifo) && readVector(f, cd.responseFifo) &&
        readPod(f, cd.interruptFlag) && readPod(f, cd.interruptEnable) &&
        readPod(f, cd.irqLinePending) && readPod(f, cd.setLocMinutes) &&
        readPod(f, cd.setLocSeconds) && readPod(f, cd.setLocSector) &&
        readPod(f, cd.currentLba) && readPod(f, cd.seekTarget) &&
        readPod(f, cd.mode) && readPod(f, cd.sectorBuffer) &&
        readPod(f, cd.sectorSize) &&
        (header.version < 5 || readPod(f, cd.dataReadOffset)) && readPod(f, cd.sectorReady) &&
        readPod(f, cd.cyclesUntilResponse) && readPod(f, cd.cyclesPerSector) &&
        readPod(f, cd.readCycleCounter) && readPod(f, cd.waitingForAck) &&
        readPod(f, cd.pendingClockCycles) && readPod(f, cd.xaFilterFile) &&
        readPod(f, cd.xaFilterChannel) && readPod(f, cd.xaFilterEnabled) &&
        readPod(f, cd.motorOn) && readPod(f, cd.shellOpen) &&
        readPod(f, cd.pendingCommand) && readPod(f, cd.commandPending) &&
        readPod(f, cd.hasSecondaryResponse) &&
        readPod(f, cd.secondaryResponseDelay) &&
        readPod(f, cd.secondaryInterrupt) && readVector(f, cd.secondaryData);
    if (!cdValid || cd.sectorSize > cd.sectorBuffer.size() || cd.dataReadOffset > cd.sectorSize) {
      fmt::print(stderr, "[SaveState] Invalid CD state in '{}'\n", path);
      return false;
    }

    spu::SPU::Snapshot spuState;
    bool spuValid =
        readPod(f, spuState.voices) && readPod(f, spuState.spuCtrl) &&
        readPod(f, spuState.spuStat) && readPod(f, spuState.mainVolL) &&
        readPod(f, spuState.mainVolR) && readPod(f, spuState.irqAddr) &&
        readPod(f, spuState.irqPending) && readPod(f, spuState.keyOnLatch) &&
        readPod(f, spuState.keyOffLatch) && readPod(f, spuState.noiseOn) &&
        readPod(f, spuState.pitchModOn) && readPod(f, spuState.reverbOn) &&
        readPod(f, spuState.endxFlags) && readPod(f, spuState.transferAddr) &&
        readPod(f, spuState.transferCtrl) && readPod(f, spuState.reverb) &&
        readPod(f, spuState.noiseLevel) && readPod(f, spuState.noiseTimer) &&
        readPod(f, spuState.noiseStep) && readVector(f, spuState.xaBuffer) &&
        readVector(f, spuState.cdDaBuffer) && readPod(f, spuState.xaReadPos) &&
        readPod(f, spuState.cdDaReadPos) &&
        readPod(f, spuState.generatedSampleCount) &&
        readPod(f, spuState.firstNonzeroOutputSeen);
    mdec::MDEC::Snapshot mdecState;
    const bool mdecValid = readPod(f, mdecState.state) &&
                           readPod(f, mdecState.busy) &&
                           readPod(f, mdecState.currentCommand) &&
                           readPod(f, mdecState.outputDepth24) &&
                           readPod(f, mdecState.outputSigned) &&
                           readPod(f, mdecState.remainingWords) &&
                           readPod(f, mdecState.lumaQuantTable) &&
                           readPod(f, mdecState.chromaQuantTable) &&
                           readPod(f, mdecState.hasCustomQuantTable) &&
                           readVector(f, mdecState.inputBuffer) &&
                           readVector(f, mdecState.outputBuffer) &&
                           readPod(f, mdecState.block) && mdecState.state <= 2;
    input::InputController::Snapshot inputState = input.snapshot();
    const bool inputValid = header.version < 3 || readPod(f, inputState);
    input::MemoryCard::Snapshot cards[2];
    const bool cardsValid =
        readPod(f, cards[0]) && readPod(f, cards[1]) &&
        cards[0].protocolState <= 6 && cards[1].protocolState <= 6 &&
        cards[0].byteCounter <= input::MemoryCard::SECTOR_SIZE &&
        cards[1].byteCounter <= input::MemoryCard::SECTOR_SIZE;
    PsyqDiskState psyqState;
    psyq::PadModuleSnapshot padState;
    const auto validPadBuffer = [](uint32_t address) {
      return address == 0 || ((address & 0x1FFFFFFFu) <= Memory::RAM_SIZE - 4u);
    };
    if (!spuValid || !mdecValid || !inputValid || !cardsValid ||
        !readPod(f, psyqState) ||
        (header.version >= 3 &&
         (!readPod(f, padState) || !validPadBuffer(padState.buf1Addr) ||
          !validPadBuffer(padState.buf2Addr)))) {
      fmt::print(stderr,
                 "[SaveState] Invalid SPU/MDEC/card/PsyQ state in '{}'\n",
                 path);
      return false;
    }

    bool hasBiosState = false;
    bios::Bios::Snapshot biosState;
    if (header.version >= 4) {
      uint32_t biosMarker = 0;
      const bool biosValid =
          readPod(f, biosMarker) && biosMarker == kBiosStateMarker &&
          readPod(f, hasBiosState) &&
          (!hasBiosState ||
           (readVector(f, biosState.eventSystem.events) &&
            biosState.eventSystem.events.size() <= 32 &&
            readPod(f, biosState.eventSystem.triggeredBits) &&
            readVector(f, biosState.eventSystem.pendingCallbacks) &&
            readPod(f, biosState.padBuf1Addr) &&
            readPod(f, biosState.padBuf1Size) &&
            readPod(f, biosState.padBuf2Addr) &&
            readPod(f, biosState.padBuf2Size) &&
            readPod(f, biosState.padActive) &&
            readPod(f, biosState.cdIntPending) &&
            readVector(f, biosState.cdEventQueue) &&
            readPod(f, biosState.cdExceptionPending) &&
            readPod(f, biosState.lastIntrTickFrame) &&
            readPod(f, biosState.lastDmaCompletion) &&
            readPod(f, biosState.customExceptionBuffer)));
      if (!biosValid) {
        fmt::print(stderr, "[SaveState] Invalid BIOS state in '{}'\n", path);
        return false;
      }
    }

    gpu.loadGuestState(gpuState);
    dma.restore(dmaState);
    for (uint32_t timer = 0; timer < 3; ++timer) {
      const uint32_t base = 0x1F801100u + timer * 0x10u;
      timers.writeRegister(base + 4u, timerState[timer].mode);
      timers.writeRegister(base + 8u, timerState[timer].target);
      timers.writeRegister(base, timerState[timer].counter);
    }
    cdrom.restore(cd);
    spu.restore(spuState);
    if (mdec != nullptr)
      mdec->restore(mdecState);
    if (header.version >= 3 && !input.restore(inputState)) {
      fmt::print(stderr, "[SaveState] Invalid input/SIO state in '{}'\n", path);
      return false;
    }
    input.getMemoryCard(0).restore(cards[0]);
    input.getMemoryCard(1).restore(cards[1]);
    restorePsyqState(psyqState);
    if (header.version >= 3)
      psyq::psyq_pad_restore(padState);
    if (hasBiosState && biosHle != nullptr) {
      if (!biosHle->restore(biosState)) {
        fmt::print(stderr, "[SaveState] Rejected BIOS state in '{}'\n", path);
        return false;
      }
      biosTimelineRestored = true;
    }
  }

  if (biosHle != nullptr && !biosTimelineRestored)
    biosHle->rebaseAfterTimelineLoad();

  // Verify checksum
  uint32_t checksum = computeChecksum(mem.ramPtr(), Memory::RAM_SIZE);
  if (checksum != header.checksum) {
    fmt::print(stderr,
               "[SaveState] WARNING: Checksum mismatch (got 0x{:08X}, "
               "expected 0x{:08X})\n",
               checksum, header.checksum);
  }

  f.close();
  fmt::print("[SaveState] Loaded from '{}'\n", path);
  return true;
}

bool SaveState::isValid(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;

  SaveStateHeader header;
  f.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!f.good())
    return false;

  return header.magic[0] == 'P' && header.magic[1] == 'S' &&
         header.magic[2] == '1' && header.magic[3] == 'S' &&
         (header.version == 1 || header.version == 2 || header.version == 3 ||
          header.version == 4 || header.version == 5);
}

} // namespace ps1
