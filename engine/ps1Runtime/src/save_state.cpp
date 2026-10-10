#include "runtime/save_state.h"
#include "runtime/save_files.h"
#include <ctime>
#include "runtime/bios/bios.h"
#include "runtime/games/crash3_profile.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/games/mgs_disc.h"
#include "runtime/psyq/psyq_pad.h"
#include "runtime/psyq/psyq_state.h"
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <limits>
#include <type_traits>

namespace ps1 {

namespace {

constexpr uint32_t kVersionTwelveFlags = 0x4356534eu; // NSVC: BIOS allocators and files
constexpr uint32_t kVersionElevenFlags = 0x4256534eu; // NSVB: timer phase and IRQ latch
constexpr uint32_t kVersionTenFlags = 0x4156534eu; // NSVA: XA decoder and CD mixer
constexpr uint32_t kVersionNineFlags = 0x3956534eu; // NSV9: DMA IRQ semantics + MDEC scale matrix
constexpr uint32_t kVersionEightFlags = 0x3856534eu; // NSV8
constexpr size_t kMediaIdentitySize = 64; // fixed v8 trailer; no host paths
constexpr uint32_t kVersionSevenFlags = 0x3756534eu; // NSV7
constexpr uint32_t kVersionSixFlags = 0x3656534eu; // NSV6
constexpr uint32_t kExtendedStateMarker = 0x32545845u; // "EXT2"
constexpr uint32_t kBiosStateMarker = 0x34534F49u;     // "IOS4"
constexpr uint32_t kMaxSerializedVectorElements = 1u << 20;
uint32_t payloadChecksum(std::istream &f) {
  uint32_t hash = 2166136261u;
  std::array<char, 65536> buffer;
  while (f.read(buffer.data(), buffer.size()) || f.gcount())
    for (std::streamsize i = 0; i < f.gcount(); ++i)
      hash = (hash ^ static_cast<uint8_t>(buffer[i])) * 16777619u;
  return hash;
}

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

// NSV12 variable-length data is bounded by both a total budget and the bytes
// actually present. A forged length must not cause a large allocation first.
template <typename Container>
bool readBoundedBytes(std::ifstream &f, Container &out, size_t limit, size_t &budget) {
  uint32_t count = 0;
  if (!readPod(f, count) || count > limit || count > budget) return false;
  const auto start = f.tellg();
  if (start < 0) return false;
  f.seekg(0, std::ios::end);
  const auto end = f.tellg();
  f.seekg(start);
  if (!f || end - start < std::streamoff(count) + std::streamoff(kMediaIdentitySize))
    return false;
  budget -= count;
  out.resize(count);
  return count == 0 || bool(f.read(reinterpret_cast<char *>(out.data()), count));
}

void writeBiosResources(std::ofstream &f, const bios::Bios::Snapshot &state) {
  writePod(f, state.heap.baseAddress);
  writePod(f, state.heap.totalSize);
  writePod(f, state.heap.firstBlock);
  writePod(f, state.kernelHeapNext);
  writePod(f, state.kernelHeapEnd);
  const auto writeString = [&](const std::string &s) {
    writePod(f, uint32_t(s.size()));
    f.write(s.data(), s.size());
  };
  for (const auto &fd : state.files.descriptors) {
    writePod(f, fd.kind);
    writeString(fd.path);
    writeVector(f, fd.data);
    writePod(f, fd.cursor);
    writePod(f, fd.firstBlock);
    writePod(f, fd.cardSlot);
  }
  writePod(f, uint32_t(state.files.directory.size()));
  for (const auto &[name, size] : state.files.directory) {
    writeString(name);
    writePod(f, size);
  }
  writePod(f, state.files.directoryIndex);
  writePod(f, state.randomSeed);
}

bool readBiosResources(std::ifstream &f, bios::Bios::Snapshot &state) {
  using bios::FileIO;
  if (!(readPod(f, state.heap.baseAddress) && readPod(f, state.heap.totalSize) &&
        readPod(f, state.heap.firstBlock) && readPod(f, state.kernelHeapNext) &&
        readPod(f, state.kernelHeapEnd))) return false;
  size_t dataBudget = FileIO::MAX_SNAPSHOT_DATA;
  size_t pathBudget = FileIO::MAX_FDS * FileIO::MAX_SNAPSHOT_PATH + 15u * 20u;
  for (auto &fd : state.files.descriptors) {
    if (!(readPod(f, fd.kind) &&
          readBoundedBytes(f, fd.path, FileIO::MAX_SNAPSHOT_PATH, pathBudget) &&
          readBoundedBytes(f, fd.data, FileIO::MAX_SNAPSHOT_DATA, dataBudget) &&
          readPod(f, fd.cursor) && readPod(f, fd.firstBlock) && readPod(f, fd.cardSlot)))
      return false;
  }
  uint32_t count = 0;
  if (!readPod(f, count) || count > 15) return false;
  state.files.directory.resize(count);
  for (auto &[name, size] : state.files.directory)
    if (!readBoundedBytes(f, name, 20, pathBudget) || !readPod(f, size)) return false;
  return readPod(f, state.files.directoryIndex) && readPod(f, state.randomSeed) &&
         bios::Bios::validSnapshot(state);
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
  uint32_t intrCallback[8]{}; // Frozen v2-v6 disk prefix
  uint32_t dmaCallback[8]{};
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
  std::copy_n(std::begin(source.intrCallback), 8,
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
  // A successful load while host-paused must still unwind when we resume,
  // even if the player saves again before closing the host menu.
  return true;
}

void SaveStateCoordinator::checkpoint(recomp_context &ctx, uint32_t resumePc) {
  std::unique_lock lock(mutex_);
  if ((!request_.has_value() && !hostPauseRequested_) || cancelled_)
    return;

  ctx.pc = resumePc;
  gameThreadParked_ = true;
  ready_.notify_one();
  resumed_.wait(lock, [&] {
    return cancelled_ || (!hostPauseRequested_ &&
                          (!request_.has_value() || operationComplete_));
  });
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
    reloadTimeline_ |= succeeded && current.action == Action::Load;
    operationComplete_ = true;
    // Keep the guest parked but allow another operation from the host menu.
    if (hostPauseRequested_) request_.reset();
  }
  resumed_.notify_one();
  return true;
}

bool SaveStateCoordinator::pending() const {
  std::lock_guard lock(mutex_);
  return request_.has_value();
}

void SaveStateCoordinator::setHostPaused(bool paused) {
  {
    std::lock_guard lock(mutex_);
    hostPauseRequested_ = paused;
  }
  resumed_.notify_all();
}

bool SaveStateCoordinator::hostPaused() const {
  std::lock_guard lock(mutex_);
  return hostPauseRequested_ && gameThreadParked_;
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
  if (biosHle && !biosHle->canSaveNativeThreads()) {
    fmt::print(stderr, "[SaveState] Native threads have not reached resumable boundaries\n");
    return false;
  }
  const auto previous = inspect(path);
  if (previous.version > SaveStateHeader{}.version) {
    fmt::print(stderr, "[SaveState] Refusing to overwrite newer state format {}\n", previous.version);
    return false;
  }
  try {
  storage::Transaction transaction(path);
  const auto &temporaryPath = transaction.temporary();
  std::ofstream f(temporaryPath, std::ios::binary | std::ios::trunc);
  if (!f) {
    fmt::print(stderr, "[SaveState] Cannot open '{}' for writing\n", path);
    return false;
  }

  // Write header placeholder (we'll update checksum at the end)
  SaveStateHeader header;
  header.flags = kVersionTwelveFlags;
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
  const auto timerState = timers.snapshot(); // does not acknowledge guest flags
  for (const auto &timer : timerState.timers) {
    writePod(f, timer.mode);
    writePod(f, timer.target);
    writePod(f, timer.counter);
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
  const auto biosState = hasBiosState ? biosHle->snapshot() : bios::Bios::Snapshot{};
  if (hasBiosState && !bios::Bios::validSnapshot(biosState)) return false;
  writePod(f, hasBiosState);
  if (hasBiosState) {
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
    writePod(f, biosState.kernelThreads);
    writeVector(f, biosState.cardEvents);
  }

  // v7: keep the original PsyQ disk layout and append IRQ slots 8-10
  // and the unconsumed guest SPU clock credit. PCM is presentation-only.
  for (size_t slot = 8; slot < psyq::PsyqState::kIntrSlots; ++slot)
    writePod(f, psyq::psyq_state().intrCallback[slot]);
  writePod(f, spuState.clockSamplesPending);
  // v9 appends the MDEC matrix; all earlier hardware sections stay frozen.
  writePod(f, mdecState.scaleTable);
  // v10: XA prediction, interpolation, filtering and both CD mixer stages.
  writePod(f, cd.xaDecoder.history);
  writePod(f, cd.xaDecoder.ring);
  writePod(f, cd.xaDecoder.position);
  writePod(f, cd.xaDecoder.sixstep);
  writePod(f, cd.xaCurrentFile);
  writePod(f, cd.xaCurrentChannel);
  writePod(f, cd.xaCurrentSet);
  writePod(f, cd.muted);
  writePod(f, cd.adpcmMuted);
  writePod(f, cd.audioVolume);
  writePod(f, cd.nextAudioVolume);
  writePod(f, spuState.cdVolL);
  writePod(f, spuState.cdVolR);
  // v11: four bytes, deliberately outside the frozen timer register prefix.
  for (const auto &timer : timerState.timers) writePod(f, timer.irqFired);
  writePod(f, timerState.sysclkDiv8Carry);
  // v12: only when IOS4 declares a BIOS. No host pointers or backing paths.
  if (hasBiosState) writeBiosResources(f, biosState);
  std::array<char, kMediaIdentitySize> media{};
  const auto identity = cdrom.mediaIdentity();
  if (identity.size() >= media.size()) return false;
  std::copy(identity.begin(), identity.end(), media.begin());
  writePod(f, media);

  if (!f.good()) {
    fmt::print(stderr, "[SaveState] Short write to '{}'\n", path);
    return false;
  }

  // Update the header with the checksum of the complete payload.
  f.flush();
  if (!f) return false;
  std::ifstream payload(temporaryPath, std::ios::binary);
  if (!payload) return false;
  payload.seekg(sizeof(SaveStateHeader));
  header.checksum = payloadChecksum(payload);
  // Release the checksum reader before publication: Windows correctly refuses
  // replacing an open file whose reader did not grant FILE_SHARE_DELETE.
  payload.close();
  f.seekp(headerPos);
  writeBytes(f, &header, sizeof(header));

  f.close();
  if (!f) return false;
  if (!transaction.commit()) return false;
  fmt::print("[SaveState] Saved to '{}'\n", path);
  std::fflush(stdout);
  return true;
  } catch (const std::exception &error) {
    fmt::print(stderr, "[SaveState] Save failed: {}\n", error.what());
    return false;
  }
}

bool SaveState::load(const std::string &path, recomp_context &ctx, Memory &mem,
                     gpu::GPU &gpu, spu::SPU &spu, DMA &dma,
                     cdrom::CdromController &cdrom, Timers &timers,
                     InterruptController &irq, input::InputController &input,
                     mdec::MDEC *mdec, bios::Bios *biosHle) {
  if (biosHle && !biosHle->canSaveNativeThreads()) {
    fmt::print(stderr, "[SaveState] Native threads have not reached resumable boundaries\n");
    return false;
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fmt::print(stderr, "[SaveState] Cannot open '{}' for reading\n", path);
    return false;
  }

  // Read header
  SaveStateHeader header;
  if (!readBytes(f, &header, sizeof(header))) return false;
  if ((header.version == 12 || header.flags == kVersionTwelveFlags) &&
      (header.version != 12 || header.flags != kVersionTwelveFlags)) return false;
  if ((header.version == 11 || header.flags == kVersionElevenFlags) &&
      (header.version != 11 || header.flags != kVersionElevenFlags)) return false;
  if ((header.version == 10 || header.flags == kVersionTenFlags) &&
      (header.version != 10 || header.flags != kVersionTenFlags)) return false;
  if ((header.version == 9 || header.flags == kVersionNineFlags) &&
      (header.version != 9 || header.flags != kVersionNineFlags)) return false;
  if ((header.version == 6 || header.flags == kVersionSixFlags) &&
      (header.version != 6 || header.flags != kVersionSixFlags)) return false;
  if ((header.version == 8 || header.flags == kVersionEightFlags) &&
      (header.version != 8 || header.flags != kVersionEightFlags)) return false;
  if ((header.version == 7 || header.flags == kVersionSevenFlags) &&
      (header.version != 7 || header.flags != kVersionSevenFlags)) return false;

  if (header.magic[0] != 'P' || header.magic[1] != 'S' ||
      header.magic[2] != '1' || header.magic[3] != 'S') {
    fmt::print(stderr, "[SaveState] Invalid magic in '{}'\n", path);
    return false;
  }
  if (header.version != 1 && header.version != 2 && header.version != 3 &&
      header.version != 4 && header.version != 5 && header.version != 6 && header.version != 7 && header.version != 8 && header.version != 9 && header.version != 10 && header.version != 11 && header.version != 12) {
    fmt::print(stderr, "[SaveState] Unsupported version {} in '{}'\n",
               header.version, path);
    return false;
  }

  if (header.version >= 6) {
    const uint32_t actual = payloadChecksum(f);
    if (actual != header.checksum) {
      fmt::print(stderr, "[SaveState] Corrupt or truncated state '{}'\n", path);
      return false;
    }
    f.clear();
    f.seekg(sizeof(SaveStateHeader));
  }

  // Media selection is the last fallible operation, after parsing/validating
  // the entire candidate. A rejected load must not change even the mounted CD.
  const auto restoreMedium = [&]() {
    if (header.version >= 8) {
      std::array<char, kMediaIdentitySize> media{};
      f.clear();
      f.seekg(-static_cast<std::streamoff>(media.size()), std::ios::end);
      if (!readPod(f, media) || media.back() != '\0' ||
          !cdrom.restoreMedia(std::string(media.data()))) {
        fmt::print(stderr, "[SaveState] Required medium unavailable in '{}'\n", path);
        return false;
      }
      return true;
    }
    return games::mgs::restoreLegacyDisc(f, header.version, cdrom);
  };
  f.clear();
  f.seekg(sizeof(SaveStateHeader));

  // Stage all architectural bytes; checksums alone do not validate structure.
  recomp_context candidate = ctx;
  readBytes(f, candidate.r, sizeof(candidate.r));
  readBytes(f, &candidate.hi, sizeof(candidate.hi));
  readBytes(f, &candidate.lo, sizeof(candidate.lo));
  readBytes(f, &candidate.pc, sizeof(candidate.pc));
  readBytes(f, candidate.cop0, sizeof(candidate.cop0));
  readBytes(f, candidate.cop2d, sizeof(candidate.cop2d));
  readBytes(f, candidate.cop2c, sizeof(candidate.cop2c));
  candidate.nativeResumePc = 0;
  candidate.nativeEntryPc = 0;
  // Version 1 states predate the non-architectural PGXP side channels.  Never
  // let precision from the timeline being replaced attach to restored words.
  std::memset(candidate.pgxpGpr, 0, sizeof(candidate.pgxpGpr));
  std::memset(candidate.pgxpGteData, 0, sizeof(candidate.pgxpGteData));
  std::memset(candidate.gteDataSourceToken, 0, sizeof(candidate.gteDataSourceToken));
  std::memset(candidate.gteControlSourceToken, 0, sizeof(candidate.gteControlSourceToken));
  std::memset(candidate.gteControlWriterPc, 0, sizeof(candidate.gteControlWriterPc));
  std::memset(candidate.gteControlWriteGeneration, 0,
              sizeof(candidate.gteControlWriteGeneration));
  candidate.gteControlWriteSerial = 0;

  std::vector<uint8_t> ramData(Memory::RAM_SIZE);
  std::array<uint8_t, Memory::SCRATCHPAD_SIZE> scratchpadData{};
  readBytes(f, ramData.data(), ramData.size());
  readBytes(f, scratchpadData.data(), scratchpadData.size());

  // GPU VRAM (1024x512x2 = 1MB)
  std::vector<uint8_t> vramData(1024 * 512 * sizeof(gpu::Color16));
  readBytes(f, vramData.data(), vramData.size());

  // GPU State
  uint32_t gpustat;
  readBytes(f, &gpustat, sizeof(gpustat));

  // SPU Sound RAM (512KB)
  std::vector<uint8_t> spuRam(spu::SOUND_RAM_SIZE);
  readBytes(f, spuRam.data(), spuRam.size());

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
  // IRQ State
  uint32_t istat, imask;
  readBytes(f, &istat, sizeof(istat));
  readBytes(f, &imask, sizeof(imask));
  if (!f) return false;

  const auto validEnd = [&]() {
    const auto parsedEnd = f.tellg();
    if (parsedEnd < 0) return false;
    f.seekg(0, std::ios::end);
    const auto trailer = header.version >= 8 ? kMediaIdentitySize : 0;
    return f && f.tellg() == parsedEnd + static_cast<std::streamoff>(trailer);
  };
  const auto commitPrefix = [&]() {
    ctx = candidate;
    std::memcpy(mem.ramPtr(), ramData.data(), ramData.size());
    std::memcpy(mem.scratchpadPtr(), scratchpadData.data(), scratchpadData.size());
    mem.clearPrecision();
    games::crash3::restoreTransientState(mem);
    gpu.loadVram(vramData.data());
    spu.loadSoundRam(spuRam.data());
    irq.writeIMask(imask);
    irq.writeIStat(0);
    for (uint32_t bit = 0; bit < 11; ++bit)
      if (istat & (1u << bit)) irq.raiseInterrupt(1u << bit);
  };

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

    Timers::Snapshot timerState;
    for (auto &timer : timerState.timers) {
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
    mdec::MDEC::Snapshot mdecState = mdec::MDEC().snapshot();
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
                           readPod(f, mdecState.block) &&
                           mdecState.state <= 2;
    input::InputController::Snapshot inputState = input.snapshot();
    const bool inputValid = header.version < 3 ||
        (readPod(f, inputState) && input::InputController::validSnapshot(inputState));
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
            readPod(f, biosState.customExceptionBuffer) &&
            (header.version < 6 || (readPod(f, biosState.kernelThreads) &&
                                   readVector(f, biosState.cardEvents)))));
      if (!biosValid || (hasBiosState && !bios::Bios::validSnapshot(biosState))) {
        fmt::print(stderr, "[SaveState] Invalid BIOS state in '{}'\n", path);
        return false;
      }
    }

    uint32_t extraIntrCallbacks[3]{};
    if (header.version >= 7) {
      if (!readPod(f, extraIntrCallbacks) || !readPod(f, spuState.clockSamplesPending))
        return false;
    } else if (spu.guestClockEnabled() && psyqState.intrCallback[0] == 0x80085150u) {
      // USA v1.0 states from the old eight-slot runtime stored UserSpuIRQProc
      // in slot 0. MGS installs its scheduler separately via gpuSwapCb.
      // Migrate only that known callback in the verified MGS profile.
      extraIntrCallbacks[1] = psyqState.intrCallback[0];
      psyqState.intrCallback[0] = 0;
    }

    if (header.version >= 9 && !readPod(f, mdecState.scaleTable)) return false;

    if (header.version >= 10) {
      if (!(readPod(f, cd.xaDecoder.history) &&
            readPod(f, cd.xaDecoder.ring) &&
            readPod(f, cd.xaDecoder.position) &&
            readPod(f, cd.xaDecoder.sixstep) &&
            readPod(f, cd.xaCurrentFile) &&
            readPod(f, cd.xaCurrentChannel) &&
            readPod(f, cd.xaCurrentSet) &&
            readPod(f, cd.muted) &&
            readPod(f, cd.adpcmMuted) &&
            readPod(f, cd.audioVolume) &&
            readPod(f, cd.nextAudioVolume) &&
            readPod(f, spuState.cdVolL) &&
            readPod(f, spuState.cdVolR)) ||
          cd.xaDecoder.position >= 32 || cd.xaDecoder.sixstep < 1 || cd.xaDecoder.sixstep > 6)
        return false;
    } else {
      // Older runtimes did not retain CD volume writes. Unity gain restores
      // their mixer behavior; the next retail SpuSetCommonAttr sets real gain.
      spuState.cdVolL = spuState.cdVolR = 0x7fff;
    }

    if (header.version >= 11) {
      for (auto &timer : timerState.timers)
        if (!readPod(f, timer.irqFired)) return false;
      if (!readPod(f, timerState.sysclkDiv8Carry) || !Timers::validSnapshot(timerState))
        return false;
    } else {
      // Old files had only registers: no divider phase or one-shot latch.
      // Preserve their previous re-armed behavior; do not invent lost history.
      for (auto &timer : timerState.timers)
        timer.mode = (timer.mode & 0x1bff) | 0x400;
    }

    if (header.version >= 12 && hasBiosState && !readBiosResources(f, biosState))
      return false;
    // Attachment validation must precede even media selection and RAM commit.
    if (hasBiosState && biosHle && !biosHle->canRestore(biosState)) return false;

    if (header.version < 9) {
      // Earlier hosts generated callbacks for every DMA regardless of DICR,
      // and HLE registration never enabled hardware IRQs. Their flag bits
      // therefore cannot represent pending interrupts. Reconstruct only the
      // callbacks not yet consumed at the saved cooperative boundary.
      dmaState.dicr &= 0x0000803Fu;
      for (size_t ch = 0; ch < DMA::NUM_CHANNELS; ++ch) {
        if (psyqState.dmaCallback[ch] == 0) continue;
        dmaState.dicr |= (1u << 23) | (1u << (16 + ch));
        if (hasBiosState && dmaState.completionGeneration[ch] != biosState.lastDmaCompletion[ch])
          dmaState.dicr |= 1u << (24 + ch);
      }
      if ((mdecState.currentCommand >> 29) == 1)
        mdecState.outputDepth24 = ((mdecState.currentCommand >> 27) & 3u) == 2;
    }
    if (!validEnd() || !restoreMedium()) return false;
    commitPrefix();
    gpu.loadGuestState(gpuState);
    dma.restore(dmaState);
    timers.restore(timerState); // validated before any timeline mutation
    cdrom.restore(cd);
    spu.restore(spuState);
    if (mdec != nullptr)
      mdec->restore(mdecState);
    if (header.version >= 3) input.restore(inputState); // already validated
    input.getMemoryCard(0).restore(cards[0]);
    input.getMemoryCard(1).restore(cards[1]);
    input.getMemoryCard(0).protectNextWrite();
    input.getMemoryCard(1).protectNextWrite();
    restorePsyqState(psyqState);
    games::mgs::resetSceneCapture();
    std::copy(std::begin(extraIntrCallbacks), std::end(extraIntrCallbacks),
              psyq::psyq_state().intrCallback + 8);
    if (header.version >= 3)
      psyq::psyq_pad_restore(padState);
    if (hasBiosState && biosHle != nullptr) {
      biosHle->restore(biosState); // already validated, including event handlers
      biosTimelineRestored = true;
    }
  } else {
    if (!validEnd() || !restoreMedium()) return false;
    commitPrefix();
    dma.writeRegister(0x1F8010F0, dpcr);
    dma.writeRegister(0x1F8010F4, dicr);
    for (uint32_t ch = 0; ch < 7; ++ch) {
      dma.writeRegister(0x1F801080 + ch * 0x10, legacyDma[ch].base);
      dma.writeRegister(0x1F801084 + ch * 0x10, legacyDma[ch].block);
      dma.writeRegister(0x1F801088 + ch * 0x10, legacyDma[ch].chcr);
    }
  }

  if (biosHle != nullptr && !biosTimelineRestored)
    biosHle->rebaseAfterTimelineLoad();

  // Verify checksum
  uint32_t checksum = computeChecksum(mem.ramPtr(), Memory::RAM_SIZE);
  if (header.version < 6 && checksum != header.checksum) {
    fmt::print(stderr,
               "[SaveState] WARNING: Checksum mismatch (got 0x{:08X}, "
               "expected 0x{:08X})\n",
               checksum, header.checksum);
  }

  f.close();
  fmt::print("[SaveState] Loaded from '{}'\n", path);
  std::fflush(stdout);
  return true;
}

SaveState::Info SaveState::inspect(const std::string &path) {
  Info info;
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return info;
  info.exists = true;
  info.bytes = std::filesystem::file_size(path, error);
  if (error) return info;
  const auto time = std::filesystem::last_write_time(path, error);
  if (!error) {
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::file_clock::to_sys(time));
    const auto seconds = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{}; char text[32]{};
#if defined(_WIN32)
    const bool converted = localtime_s(&local, &seconds) == 0;
#else
    const bool converted = localtime_r(&seconds, &local) != nullptr;
#endif
    if (converted && std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local)) info.savedAt = text;
  }
  std::ifstream file(path, std::ios::binary);
  SaveStateHeader header;
  if (!readPod(file, header) || std::memcmp(header.magic, "PS1S", 4) != 0) return info;
  info.version = header.version;
  info.supported = header.version >= 1 && header.version <= SaveStateHeader{}.version;
  return info;
}

bool SaveState::loadRecovering(const std::string &path, recomp_context &ctx, Memory &mem,
    gpu::GPU &gpu, spu::SPU &spu, DMA &dma, cdrom::CdromController &cdrom,
    Timers &timers, InterruptController &irq, input::InputController &input,
    mdec::MDEC *mdec, bios::Bios *biosHle, std::string &recoverySource) {
  recoverySource.clear();
  if (load(path, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input, mdec, biosHle)) return true;
  // A valid candidate may require another CD or an unavailable attachment.
  // Don't silently replace it with older progress, or downgrade future states.
  if (inspect(path).version > SaveStateHeader{}.version || isValid(path)) return false;
  for (const auto &backup : storage::backups(path)) {
    if (load(backup.string(), ctx, mem, gpu, spu, dma, cdrom, timers, irq, input, mdec, biosHle)) {
      recoverySource = backup.string();
      fmt::print(stderr, "[SaveState] Recovered '{}' from '{}'; primary left unchanged\n", path, recoverySource);
      return true;
    }
  }
  return false;
}

bool SaveState::isValid(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;

  SaveStateHeader header;
  f.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!f.good())
    return false;

  const bool headerValid = header.magic[0] == 'P' && header.magic[1] == 'S' &&
         header.magic[2] == '1' && header.magic[3] == 'S' &&
         (header.version == 1 || header.version == 2 || header.version == 3 ||
          header.version == 4 || header.version == 5 || header.version == 6 || header.version == 7 || header.version == 8 || header.version == 9 || header.version == 10 || header.version == 11 || header.version == 12);
  return headerValid && (header.version < 6
      ? header.flags != kVersionSixFlags && header.flags != kVersionSevenFlags && header.flags != kVersionEightFlags && header.flags != kVersionNineFlags && header.flags != kVersionTenFlags && header.flags != kVersionElevenFlags && header.flags != kVersionTwelveFlags
      : header.flags == (header.version == 6 ? kVersionSixFlags : header.version == 7 ? kVersionSevenFlags : header.version == 8 ? kVersionEightFlags : header.version == 9 ? kVersionNineFlags : header.version == 10 ? kVersionTenFlags : header.version == 11 ? kVersionElevenFlags : kVersionTwelveFlags)
          && payloadChecksum(f) == header.checksum);
}

} // namespace ps1
