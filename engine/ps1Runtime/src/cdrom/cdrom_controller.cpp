#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/metrics.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

// Set PS1_BIOS_DEBUG=1 to enable verbose CDROM controller logging.
static const bool s_cdVerbose = (std::getenv("PS1_BIOS_DEBUG") != nullptr);
#define CDROM_LOG(...)                                                         \
  do {                                                                         \
    if (s_cdVerbose)                                                           \
      fmt::print(__VA_ARGS__);                                                 \
  } while (0)

namespace ps1::cdrom {

// Helpers

uint32_t CdromController::msfToLba(uint8_t m, uint8_t s, uint8_t f) {
  return (m * 60 + s) * 75 + f;
}

void CdromController::lbaToMsf(uint32_t lba, uint8_t &m, uint8_t &s,
                               uint8_t &f) {
  m = lba / (60 * 75);
  s = (lba / 75) % 60;
  f = lba % 75;
}

uint8_t CdromController::toBcd(uint8_t val) {
  return ((val / 10) << 4) | (val % 10);
}

uint8_t CdromController::fromBcd(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Constructor / Reset

CdromController::CdromController() { reset(); }

void CdromController::reset() {
  state_ = CdromState::Idle;
  indexReg_ = 0;
  paramFifo_.clear();
  responseFifo_.clear();
  interruptFlag_ = 0;
  irqLinePending_.store(false, std::memory_order_relaxed);
  interruptEnable_ = 0x1F;
  setLocMinutes_ = 0;
  setLocSeconds_ = 2;
  setLocSector_ = 0;
  currentLba_ = 0;
  seekTarget_ = 0;
  mode_ = 0;
  sectorBuffer_.fill(0);
  sectorSize_ = 2048;
  sectorReady_ = false;
  cyclesUntilResponse_ = 0;
  cyclesPerSector_ = 33868800 / 75; // ~451584 cycles per sector at 1x
  readCycleCounter_ = 0;
  waitingForAck_ = false;
  pendingClockCycles_.store(0, std::memory_order_relaxed);
  xaFilterFile_ = 0;
  xaFilterChannel_ = 0;
  xaFilterEnabled_ = false;
  motorOn_ = false;
  shellOpen_ = false;
  pendingCommand_ = 0;
  commandPending_ = false;
  hasSecondaryResponse_ = false;
  secondaryResponseDelay_ = 0;
  secondaryInterrupt_ = INT_NONE;
  secondaryData_.clear();
}

CdromController::Snapshot CdromController::snapshot() const {
  Snapshot result;
  result.state = state_;
  result.indexReg = indexReg_;
  result.paramFifo.assign(paramFifo_.begin(), paramFifo_.end());
  result.responseFifo.assign(responseFifo_.begin(), responseFifo_.end());
  result.interruptFlag = interruptFlag_;
  result.interruptEnable = interruptEnable_;
  result.irqLinePending = irqLinePending_.load(std::memory_order_acquire);
  result.setLocMinutes = setLocMinutes_;
  result.setLocSeconds = setLocSeconds_;
  result.setLocSector = setLocSector_;
  result.currentLba = currentLba_;
  result.seekTarget = seekTarget_;
  result.mode = mode_;
  result.sectorBuffer = sectorBuffer_;
  result.sectorSize = sectorSize_;
  result.sectorReady = sectorReady_;
  result.cyclesUntilResponse = cyclesUntilResponse_;
  result.cyclesPerSector = cyclesPerSector_;
  result.readCycleCounter = readCycleCounter_;
  result.waitingForAck = waitingForAck_;
  result.pendingClockCycles =
      pendingClockCycles_.load(std::memory_order_acquire);
  result.xaFilterFile = xaFilterFile_;
  result.xaFilterChannel = xaFilterChannel_;
  result.xaFilterEnabled = xaFilterEnabled_;
  result.motorOn = motorOn_;
  result.shellOpen = shellOpen_;
  result.pendingCommand = pendingCommand_;
  result.commandPending = commandPending_;
  result.hasSecondaryResponse = hasSecondaryResponse_;
  result.secondaryResponseDelay = secondaryResponseDelay_;
  result.secondaryInterrupt = secondaryInterrupt_;
  result.secondaryData = secondaryData_;
  return result;
}

void CdromController::restore(const Snapshot &state) {
  state_ = state.state;
  indexReg_ = state.indexReg;
  paramFifo_.assign(state.paramFifo.begin(), state.paramFifo.end());
  responseFifo_.assign(state.responseFifo.begin(), state.responseFifo.end());
  interruptFlag_ = state.interruptFlag;
  interruptEnable_ = state.interruptEnable;
  irqLinePending_.store(state.irqLinePending, std::memory_order_release);
  setLocMinutes_ = state.setLocMinutes;
  setLocSeconds_ = state.setLocSeconds;
  setLocSector_ = state.setLocSector;
  currentLba_ = state.currentLba;
  seekTarget_ = state.seekTarget;
  mode_ = state.mode;
  sectorBuffer_ = state.sectorBuffer;
  sectorSize_ = state.sectorSize;
  sectorReady_ = state.sectorReady;
  cyclesUntilResponse_ = state.cyclesUntilResponse;
  cyclesPerSector_ = state.cyclesPerSector;
  readCycleCounter_ = state.readCycleCounter;
  waitingForAck_ = state.waitingForAck;
  pendingClockCycles_.store(state.pendingClockCycles,
                            std::memory_order_release);
  xaFilterFile_ = state.xaFilterFile;
  xaFilterChannel_ = state.xaFilterChannel;
  xaFilterEnabled_ = state.xaFilterEnabled;
  motorOn_ = state.motorOn;
  shellOpen_ = state.shellOpen;
  pendingCommand_ = state.pendingCommand;
  commandPending_ = state.commandPending;
  hasSecondaryResponse_ = state.hasSecondaryResponse;
  secondaryResponseDelay_ = state.secondaryResponseDelay;
  secondaryInterrupt_ = state.secondaryInterrupt;
  secondaryData_ = state.secondaryData;
}

void CdromController::attachVirtualFs(VirtualFs *vfs) { vfs_ = vfs; }

// Status Byte

void CdromController::fireSecondaryNow() {
  if (!hasSecondaryResponse_)
    return;
  hasSecondaryResponse_ = false;
  secondaryResponseDelay_ = 0;
  responseFifo_.clear();
  for (auto b : secondaryData_)
    responseFifo_.push_back(b);
  interruptFlag_ = static_cast<uint8_t>(secondaryInterrupt_);
  irqLinePending_.store(secondaryInterrupt_ != INT_NONE,
                        std::memory_order_release);
  if (interruptCallback_ && secondaryInterrupt_ != INT_NONE)
    interruptCallback_(static_cast<uint8_t>(secondaryInterrupt_));
}

uint8_t CdromController::buildStatusByte() const {
  uint8_t stat = 0;
  if (motorOn_)
    stat |= (1 << 1);
  if (state_ == CdromState::ReadingData)
    stat |= (1 << 5);
  if (state_ == CdromState::Seeking)
    stat |= (1 << 6);
  if (state_ == CdromState::Playing)
    stat |= (1 << 7);
  if (shellOpen_)
    stat |= (1 << 4);
  return stat;
}

// Register I/O

void CdromController::writeRegister(uint32_t addr, uint8_t val) {
  uint32_t port = addr & 3;

  CDROM_LOG("[CDROM-IO] Write port{}.idx{} = 0x{:02X}\n", port, indexReg_, val);

  if (port == 0) {
    indexReg_ = val & 3;
    return;
  }

  switch (indexReg_) {
  case 0:
    switch (port) {
    case 1: // Command register
      // Execute commands immediately (HLE-style) so the game thread can
      // see the response without waiting for tick().  This avoids a race
      // where the game's tight polling loop times out before the main
      // thread's per-frame tick() ever processes the command.
      pendingCommand_ = val;
      commandPending_ = false; // no deferred processing needed
      executeCommand(val);
      break;
    case 2: // Parameter FIFO
      if (paramFifo_.size() < 16)
        paramFifo_.push_back(val);
      break;
    case 3: // Request register
      // bit5: want data (triggers data read)
      break;
    }
    break;
  case 1:
    switch (port) {
    case 3: // Interrupt flag (ack by writing bits) -- Index 1
      interruptFlag_ &= ~(val & 0x1F);
      irqLinePending_.store(interruptFlag_ != 0, std::memory_order_release);
      // When the game clears interrupt bits, it has finished processing
      // the interrupt (including reading the response and calling any
      // callbacks).  Clear the delivery gate so the CDROM can deliver
      // the next sector, then immediately deliver any queued secondary
      // response (e.g., INT2 Complete after Stop/Pause/Init INT3 ack).
      if (val & 0x1F) {
        waitingForAck_ = false;
        if (secondaryResponseDelay_ == 0)
          fireSecondaryNow();
      }
      if (val & 0x40)
        paramFifo_.clear();
      break;
    }
    break;
  case 2:
    switch (port) {
    case 2: // Interrupt enable
      interruptEnable_ = val & 0x1F;
      break;
    case 3: // Interrupt flag (ack by writing bits) -- Index 2
      // Some docs say index 2/3 port 3 also ACKs IF.
      // PsyQ CdInit writes port3.idx2 = 0x00 (no-op clear).
      interruptFlag_ &= ~(val & 0x1F);
      irqLinePending_.store(interruptFlag_ != 0, std::memory_order_release);
      if (val & 0x1F) {
        waitingForAck_ = false;
        if (secondaryResponseDelay_ == 0)
          fireSecondaryNow();
      }
      if (val & 0x40)
        paramFifo_.clear();
      break;
    }
    break;
  case 3:
    switch (port) {
    case 1: // Volume apply
      break;
    case 3: // Interrupt flag (ack by writing bits) -- Index 3
      // PS1 CDROM: port 3 with ODD index (1 or 3) = write to IF ACK.
      // PsyQ CdInit writes port3.idx3 = 0x20 to clear param FIFO.
      interruptFlag_ &= ~(val & 0x1F);
      irqLinePending_.store(interruptFlag_ != 0, std::memory_order_release);
      if (val & 0x1F) {
        waitingForAck_ = false;
        if (secondaryResponseDelay_ == 0)
          fireSecondaryNow();
      }
      if (val & 0x40)
        paramFifo_.clear();
      break;
    }
    break;
  }
}

uint8_t CdromController::readRegister(uint32_t addr) {
  uint32_t port = addr & 3;

  if (port == 0) {
    // Status register
    uint8_t stat = indexReg_ & 3;
    if (!paramFifo_.empty())
      stat |= (1 << 3); // Param fifo not empty
    if (paramFifo_.size() < 16)
      stat |= (1 << 4); // Param fifo not full
    if (!responseFifo_.empty())
      stat |= (1 << 5); // Response fifo not empty
    if (sectorReady_)
      stat |= (1 << 6); // Data fifo has data
    // bit 7: command/parameter busy
    if (commandPending_)
      stat |= (1 << 7);
    return stat;
  }

  uint8_t result = 0;
  switch (port) {
  case 1: // Response FIFO
    if (!responseFifo_.empty()) {
      result = responseFifo_.front();
      responseFifo_.pop_front();
    }
    return result;
  case 2:     // Data read (sector data)
    return 0; // DMA should be used instead
  case 3:
    if (indexReg_ == 0 || indexReg_ == 2) {
      result = interruptEnable_ | 0xE0;
    } else {
      result = interruptFlag_ | 0xE0;
      CDROM_LOG("[CDROM-IF] Read IF=0x{:02X} (interruptFlag_={}) idx={}\n",
                result, interruptFlag_, indexReg_);
    }
    return result;
  }
  return 0;
}

// Interrupt

void CdromController::ackInterrupt(uint8_t val) {
  interruptFlag_ &= ~(val & 0x1F);
  irqLinePending_.store(interruptFlag_ != 0, std::memory_order_release);
  // NOTE: no longer clears waitingForAck_ here.
  // Use clearWaitingForAck() explicitly after the data callback has run.
}

void CdromController::clearWaitingForAck() { waitingForAck_ = false; }

// Tick

void CdromController::accrueClockCycles(uint32_t cycles) {
  pendingClockCycles_.fetch_add(cycles, std::memory_order_relaxed);
}

void CdromController::serviceClock() {
  // exchange() establishes a clean ownership boundary: cycles that arrive
  // concurrently stay pending for the next game-thread yield. In normal use
  // this is one VBlank of credit; split unusually large stalls so the owned
  // implementation keeps its 32-bit interface well-defined.
  uint64_t cycles = pendingClockCycles_.exchange(0, std::memory_order_acq_rel);
  while (cycles > UINT32_MAX) {
    tickOwned(UINT32_MAX);
    cycles -= UINT32_MAX;
  }
  if (cycles != 0)
    tickOwned(static_cast<uint32_t>(cycles));

  // A 2x drive matures three sectors per PAL VBlank. When libcd has no guest
  // data callback, triggerCdromEvent performs the complete copy+ACK inline on
  // this same thread; consume the remaining matured credit iteratively so a
  // once-per-frame poll does not throttle the drive to 1-2 sectors/frame.
  // A guest callback or register-DMA consumer leaves waitingForAck_ asserted,
  // so the loop stops after exactly one published sector. The eight-sector
  // bound matches the credit cap below and prevents a long host stall from
  // turning one yield into an unbounded burst.
  constexpr int maxSectorsPerService = 8;
  for (int i = 1; i < maxSectorsPerService; ++i) {
    if (state_ != CdromState::ReadingData || waitingForAck_ ||
        readCycleCounter_ < static_cast<int32_t>(cyclesPerSector_))
      break;
    tickOwned(0);
  }
}

void CdromController::tick(uint32_t cycles) { tickOwned(cycles); }

void CdromController::tickOwned(uint32_t cycles) {
  // Process pending command after delay
  if (commandPending_ && cyclesUntilResponse_ > 0) {
    if (cycles >= cyclesUntilResponse_) {
      cyclesUntilResponse_ = 0;
      commandPending_ = false;
      executeCommand(pendingCommand_);
    } else {
      cyclesUntilResponse_ -= cycles;
    }
  } else if (hasSecondaryResponse_ && secondaryResponseDelay_ > 0) {
    if (cycles >= secondaryResponseDelay_) {
      secondaryResponseDelay_ = 0;
      hasSecondaryResponse_ = false;

      responseFifo_.clear();
      for (auto b : secondaryData_)
        responseFifo_.push_back(b);
      interruptFlag_ = static_cast<uint8_t>(secondaryInterrupt_);
      irqLinePending_.store(secondaryInterrupt_ != INT_NONE,
                            std::memory_order_release);
      if (interruptCallback_ && secondaryInterrupt_ != INT_NONE) {
        interruptCallback_(static_cast<uint8_t>(secondaryInterrupt_));
      }
    } else {
      secondaryResponseDelay_ -= cycles;
    }
  }

  // Process ongoing reads. The drive clock keeps advancing while the current
  // sector waits for ACK; a bounded credit represents the controller's small
  // internal sector pipeline. Publication remains one-sector-at-a-time.
  if (cycles != 0 && state_ == CdromState::ReadingData) {
    ps1::metrics::trace("cdrom.read_clock",
                        {{"cycles", cycles},
                         {"cycle_counter", readCycleCounter_},
                         {"cycles_per_sector", cyclesPerSector_},
                         {"waiting_for_ack", waitingForAck_ ? 1 : 0},
                         {"sector_ready", sectorReady_ ? 1 : 0},
                         {"lba", currentLba_},
                         {"has_disc", vfs_ != nullptr ? 1 : 0}});
  }
  if (state_ == CdromState::ReadingData) {
    constexpr int32_t maxBufferedSectors = 8;
    const int64_t accrued = static_cast<int64_t>(readCycleCounter_) + cycles;
    const int64_t creditLimit =
        static_cast<int64_t>(cyclesPerSector_) * maxBufferedSectors;
    readCycleCounter_ = static_cast<int32_t>(std::min(accrued, creditLimit));

    if (waitingForAck_)
      return;

    if (readCycleCounter_ >= static_cast<int32_t>(cyclesPerSector_)) {
      readCycleCounter_ -= static_cast<int32_t>(cyclesPerSector_);

      // Read next sector
      if (vfs_) {
        auto sector = vfs_->readSector(currentLba_);
        if (sector) {
          std::memcpy(sectorBuffer_.data(), sector->raw,
                      std::min<size_t>(SECTOR_SIZE_RAW, 2352));
          sectorReady_ = true;

          // Determine data size based on mode
          sectorSize_ = (mode_ & (1 << 5)) ? 2340 : 2048;

          // The drive has consumed this location once its bytes are in the
          // sector buffer. Advance before publishing INT1: the interrupt
          // callback may ACK and re-enter tick(0) synchronously to service
          // already-matured drive cycles, and that nested tick must observe
          // the following LBA rather than read this sector twice.
          currentLba_++;

          // Deliver INT1 and wait for acknowledgement before next sector
          waitingForAck_ = true;
          pushResponse(INT_DATA_READY, {buildStatusByte()});
        }
      }
    }
  }
}

// Push Response

void CdromController::pushResponse(CdromInt intType,
                                   std::initializer_list<uint8_t> data) {
  responseFifo_.clear();
  for (auto b : data)
    responseFifo_.push_back(b);
  interruptFlag_ = static_cast<uint8_t>(intType);
  irqLinePending_.store(intType != INT_NONE, std::memory_order_release);
  ps1::metrics::trace("cdrom.interrupt",
                      {{"type", static_cast<uint8_t>(intType)},
                       {"response_bytes", static_cast<int64_t>(data.size())},
                       {"lba", currentLba_},
                       {"state", static_cast<uint8_t>(state_)},
                       {"waiting_for_ack", waitingForAck_ ? 1 : 0},
                       {"sector_ready", sectorReady_ ? 1 : 0},
                       {"cycle_counter", readCycleCounter_}});
  // Fire callback immediately so BIOS events are triggered before the game
  // thread can read/acknowledge the hardware interrupt flag.
  if (interruptCallback_ && intType != INT_NONE) {
    interruptCallback_(static_cast<uint8_t>(intType));
  }
}

// Command Dispatch

void CdromController::executeCommand(uint8_t cmd) {
  CDROM_LOG("[CDROM] Command 0x{:02X}\n", cmd);
  ps1::metrics::trace("cdrom.command",
                      {{"command", cmd},
                       {"parameters", static_cast<int64_t>(paramFifo_.size())},
                       {"lba", currentLba_},
                       {"state", static_cast<uint8_t>(state_)},
                       {"waiting_for_ack", waitingForAck_ ? 1 : 0},
                       {"sector_ready", sectorReady_ ? 1 : 0},
                       {"cycle_counter", readCycleCounter_}});

  switch (cmd) {
  case 0x00:
    cmdSync();
    break;
  case 0x01:
    cmdGetStat();
    break;
  case 0x02:
    cmdSetLoc();
    break;
  case 0x03:
    cmdPlay();
    break;
  case 0x06:
    cmdReadN();
    break;
  case 0x08:
    cmdStop();
    break;
  case 0x09:
    cmdPause();
    break;
  case 0x0A:
    cmdInit();
    break;
  case 0x0B:
    cmdMute();
    break;
  case 0x0C:
    cmdDemute();
    break;
  case 0x0D:
    cmdSetFilter();
    break;
  case 0x0E:
    cmdSetMode();
    break;
  case 0x0F:
    cmdGetParam();
    break;
  case 0x10:
    cmdGetLocL();
    break;
  case 0x11:
    cmdGetLocP();
    break;
  case 0x13:
    cmdGetTN();
    break;
  case 0x14:
    cmdGetTD();
    break;
  case 0x15:
    cmdSeekL();
    break;
  case 0x16:
    cmdSeekP();
    break;
  case 0x19:
    cmdTest();
    break;
  case 0x1A:
    cmdGetID();
    break;
  case 0x1B:
    cmdReadS();
    break;
  default:
    CDROM_LOG("[CDROM] Unknown command: 0x{:02X}\n", cmd);
    pushResponse(INT_ERROR, {buildStatusByte(), 0x40}); // Invalid command
    break;
  }
  paramFifo_.clear();
}

// Command Implementations

void CdromController::cmdSync() {
  // CdlSync (0x00): abort any pending async command and return status.
  // On real PS1 this resets the command processor.  Games (e.g. Crash
  // Bandicoot) use it during CdInit to verify the controller is responsive.
  // Response: INT3 with status byte.
  commandPending_ = false;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdGetStat() {
  motorOn_ = true;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdSetLoc() {
  if (paramFifo_.size() >= 3) {
    setLocMinutes_ = fromBcd(paramFifo_[0]);
    setLocSeconds_ = fromBcd(paramFifo_[1]);
    setLocSector_ = fromBcd(paramFifo_[2]);
    seekTarget_ = msfToLba(setLocMinutes_, setLocSeconds_, setLocSector_);
    if (seekTarget_ >= 150)
      seekTarget_ -= 150; // Subtract 2-second pregap
  }
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdPlay() {
  state_ = CdromState::Playing;
  currentLba_ = seekTarget_;
  motorOn_ = true;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdReadN() {
  const uint32_t previousLba = currentLba_;
  motorOn_ = true;
  // NOTE: do NOT clear waitingForAck_ here.  The callback-based approach
  // handles INT1 sequentially: triggerCdromEvent queues the callback,
  // drainPendingCallbacks runs it (DMA copies the sector), THEN clears the
  // gate.  If a new ReadN is issued from inside the callback (queue
  // processing), it happens BEFORE the gate is cleared, which is correct --
  // drainPendingCallbacks clears it afterwards, enabling the first sector
  // of the new read.
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Transition to reading data state, tick() will handle INT1
  state_ = CdromState::ReadingData;
  currentLba_ = seekTarget_;
  // Speed: bit7 of mode = double speed
  cyclesPerSector_ = (mode_ & 0x80) ? (33868800 / 150) : (33868800 / 75);

  // ReadN with a pending Setloc performs an implicit logical seek before the
  // first sector. This follows SwanStation's distance bands: nearby seeks
  // wait for rotational alignment, medium seeks take roughly 30-60 ms, and
  // larger seeks start around 150/200 ms. Beetle independently models the
  // same seek+one-sector composition in PS_CDC_ReadBase().
  constexpr uint32_t masterClock = 33868800u;
  constexpr uint32_t minSeekCycles = 20000u;
  const uint32_t lbaDifference = previousLba > seekTarget_
                                     ? previousLba - seekTarget_
                                     : seekTarget_ - previousLba;
  uint32_t seekCycles = minSeekCycles;
  if (lbaDifference < 32u) {
    const uint32_t rotationalSectors =
        std::min<uint32_t>(5u, lbaDifference == 0u ? 4u : lbaDifference);
    seekCycles += cyclesPerSector_ * rotationalSectors;
  } else {
    const float discDistance =
        0.2323384936f *
        std::log(static_cast<float>((seekTarget_ / 4500u) + 1u));
    float seconds;
    if (lbaDifference <= 75u) {
      seconds = 0.03f + (static_cast<float>(lbaDifference) / 75.0f) * 0.03f +
                discDistance * 0.03f;
    } else if (lbaDifference <= 4500u) {
      seconds = 0.15f + (static_cast<float>(lbaDifference) / 4500.0f) * 0.03f +
                discDistance * 0.05f;
    } else {
      seconds =
          0.2f + (static_cast<float>(lbaDifference) / (72.0f * 4500.0f)) * 0.4f;
    }
    seekCycles += static_cast<uint32_t>(seconds * masterClock);
  }

  // tick() adds host-clock credit and emits at most one INT1 before waiting
  // for ACK. Starting negative makes the first sector due after seekCycles
  // plus the normal sector interval; subsequent sectors use the steady rate.
  readCycleCounter_ = -static_cast<int32_t>(seekCycles);
  ps1::metrics::trace("cdrom.read_begin",
                      {{"from_lba", previousLba},
                       {"target_lba", seekTarget_},
                       {"lba_distance", lbaDifference},
                       {"seek_cycles", seekCycles},
                       {"cycles_per_sector", cyclesPerSector_},
                       {"double_speed", (mode_ & 0x80) ? 1 : 0}});
}

void CdromController::cmdReadS() {
  cmdReadN(); // Same as ReadN for our purposes
}

void CdromController::cmdStop() {
  const bool wasMotorOn = motorOn_;
  state_ = CdromState::Idle;
  motorOn_ = false;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Secondary response: INT2 (Complete) after the spindle stops.
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ =
      wasMotorOn ? ((mode_ & 0x80) ? 25000000u : 13000000u) : 7000u;
  secondaryInterrupt_ = INT_COMPLETE;
  secondaryData_ = {buildStatusByte()};
}

void CdromController::cmdPause() {
  const bool wasReading =
      state_ == CdromState::ReadingData || state_ == CdromState::Playing;
  state_ = CdromState::Paused;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Secondary response: INT2 (Complete) after the drive stops reading.
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ =
      wasReading ? ((mode_ & 0x80) ? 2000000u : 1000000u) : 7000u;
  secondaryInterrupt_ = INT_COMPLETE;
  secondaryData_ = {buildStatusByte()};
}

void CdromController::cmdInit() {
  state_ = CdromState::Idle;
  mode_ = 0;
  motorOn_ = true;

  // Cancel any pending secondary response from a previous command (e.g.,
  // CdlPause INT2 that hasn't fired yet).  CdlInit is a full hardware reset;
  // delivering a stale secondary after it would confuse the CdInit polling loop
  // by re-triggering INT2 via the cdSyncByte watchpoint.
  hasSecondaryResponse_ = false;
  secondaryResponseDelay_ = 0;

  // Deliver INT3 (Acknowledge) immediately.
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Deliver INT2 (Complete) immediately after INT3.
  //
  // The game's CdInit retry loop checks the CDROM interrupt flag register
  // (interruptFlag_) after sending CdlInit to verify INT2 (Complete=2)
  // was received.  Firing INT3 then INT2 in sequence ensures interruptFlag_=2
  // when the game reads it, allowing the success condition to pass.
  //
  // On real hardware INT2 arrives ~300-400ms after INT3, but our HLE fires
  // it synchronously.  hasSecondaryResponse_ is intentionally NOT set here --
  // using the deferred secondary (via watchpoint) caused INT2 to fire
  // BEFORE CdlInit was sent, leaving interruptFlag_=3 (INT3) when the game
  // checked, which caused the 5-retry "CdInit: Init failed" loop.
  responseFifo_.clear();
  responseFifo_.push_back(buildStatusByte());
  interruptFlag_ = static_cast<uint8_t>(INT_COMPLETE);
  irqLinePending_.store(true, std::memory_order_release);
  if (interruptCallback_)
    interruptCallback_(static_cast<uint8_t>(INT_COMPLETE));
  // hasSecondaryResponse_ = false (default) -- no deferred INT2 needed.
}

void CdromController::cmdMute() {
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdDemute() {
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdSetFilter() {
  if (paramFifo_.size() >= 2) {
    xaFilterFile_ = paramFifo_[0];
    xaFilterChannel_ = paramFifo_[1];
  }
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdSetMode() {
  if (!paramFifo_.empty()) {
    mode_ = paramFifo_[0];
    xaFilterEnabled_ = (mode_ & (1 << 4)) != 0;
  }
  CDROM_LOG("[CDROM] SetMode: mode_=0x{:02X}\n", mode_);
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
}

void CdromController::cmdGetParam() {
  pushResponse(INT_ACKNOWLEDGE,
               {buildStatusByte(), mode_, xaFilterFile_, xaFilterChannel_});
}

void CdromController::cmdGetLocL() {
  uint8_t m, s, f;
  lbaToMsf(currentLba_ + 150, m, s, f);
  pushResponse(INT_ACKNOWLEDGE,
               {toBcd(m), toBcd(s), toBcd(f), 0x01, 0x00, 0x00, 0x00, 0x00});
}

void CdromController::cmdGetLocP() {
  uint8_t m, s, f;
  lbaToMsf(currentLba_ + 150, m, s, f);
  pushResponse(INT_ACKNOWLEDGE, {0x01, 0x01, toBcd(m), toBcd(s), toBcd(f),
                                 toBcd(m), toBcd(s), toBcd(f)});
}

void CdromController::cmdGetTN() {
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte(), 0x01, 0x01}); // 1 track
}

void CdromController::cmdGetTD() {
  // Return disc end position (simplified: 74 minutes)
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte(), toBcd(74), toBcd(0)});
}

void CdromController::cmdSeekL() {
  currentLba_ = seekTarget_;
  state_ = CdromState::Seeking;
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Second response after seek completes
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ = 50000;    // Seek mechanical delay
  secondaryInterrupt_ = INT_COMPLETE; // INT2
  state_ = CdromState::Idle; // State becomes idle immediately after seeking
  secondaryData_ = {buildStatusByte()};
}

void CdromController::cmdSeekP() { cmdSeekL(); }

void CdromController::cmdTest() {
  if (!paramFifo_.empty() && paramFifo_[0] == 0x20) {
    // Get CDROM BIOS date/version
    pushResponse(INT_ACKNOWLEDGE, {0x98, 0x06, 0x10, 0xC3});
  } else {
    pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});
  }
}

void CdromController::cmdGetID() {
  motorOn_ = true;
  // First response: acknowledge
  pushResponse(INT_ACKNOWLEDGE, {buildStatusByte()});

  // Second response (INT5) after a delay: return licensed game ID
  // SCEI = Sony Computer Entertainment Inc
  // 02,00 = disc type (licensed data disc)
  // 20,00,53,43,45,49 = "  SCEI"
  hasSecondaryResponse_ = true;
  secondaryResponseDelay_ = 20000;    // Arbitrary mechanical delay
  secondaryInterrupt_ = INT_DATA_END; // INT5
  secondaryData_ = {0x02, 0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x49};
}

} // namespace ps1::cdrom
