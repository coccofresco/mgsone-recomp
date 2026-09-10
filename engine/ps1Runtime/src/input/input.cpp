#include "runtime/input/input.h"
#include <fmt/format.h>

namespace ps1::input {

// Memory Card

MemoryCard::MemoryCard() { reset(); }

void MemoryCard::reset() {
  data_.fill(0);
  state_ = McState::Idle;
  sectorAddr_ = 0;
  byteCounter_ = 0;
  checksum_ = 0;

  // Initialize memory card header (block 0)
  // Magic: "MC" at offset 0
  data_[0] = 'M';
  data_[1] = 'C';
  data_[0x7F] = 0x0E; // Checksum placeholder
}

MemoryCard::Snapshot MemoryCard::snapshot() const {
  Snapshot state;
  state.data = data_;
  state.present = present_;
  state.protocolState = static_cast<uint8_t>(state_);
  state.sectorAddr = sectorAddr_;
  state.byteCounter = byteCounter_;
  state.checksum = checksum_;
  return state;
}

void MemoryCard::restore(const Snapshot &state) {
  if (state.protocolState > static_cast<uint8_t>(McState::WriteAck) ||
      state.byteCounter > SECTOR_SIZE) {
    state_ = McState::Idle;
    return;
  }
  data_ = state.data;
  present_ = state.present;
  state_ = static_cast<McState>(state.protocolState);
  sectorAddr_ = state.sectorAddr;
  byteCounter_ = state.byteCounter;
  checksum_ = state.checksum;
}

bool MemoryCard::loadFromFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  file.read(reinterpret_cast<char *>(data_.data()), CARD_SIZE);
  return file.good() || file.eof();
}

bool MemoryCard::saveToFile(const std::string &path) const {
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  file.write(reinterpret_cast<const char *>(data_.data()), CARD_SIZE);
  return file.good();
}

uint8_t MemoryCard::transfer(uint8_t dataIn) {
  switch (state_) {
  case McState::Idle:
    if (dataIn == 0x81) { // Memory card access byte
      state_ = McState::AwaitCommand;
      return 0xFF; // Acknowledge flag
    }
    return 0xFF;

  case McState::AwaitCommand:
    switch (dataIn) {
    case 0x52: // Read
      state_ = McState::AwaitAddrHi;
      return 0x5A; // ID1
    case 0x57:     // Write
      state_ = McState::AwaitAddrHi;
      return 0x5A;
    default:
      state_ = McState::Idle;
      return 0xFF;
    }

  case McState::AwaitAddrHi:
    sectorAddr_ = static_cast<uint16_t>(dataIn) << 8;
    state_ = McState::AwaitAddrLo;
    return 0x5D; // ID2

  case McState::AwaitAddrLo:
    sectorAddr_ |= dataIn;
    byteCounter_ = 0;
    checksum_ = static_cast<uint8_t>(sectorAddr_ >> 8) ^
                static_cast<uint8_t>(sectorAddr_ & 0xFF);
    state_ = McState::Reading;
    return 0x00; // ACK MSB

  case McState::Reading: {
    uint32_t addr =
        static_cast<uint32_t>(sectorAddr_) * SECTOR_SIZE + byteCounter_;
    uint8_t val = (addr < CARD_SIZE) ? data_[addr] : 0xFF;
    checksum_ ^= val;
    byteCounter_++;
    if (byteCounter_ >= SECTOR_SIZE) {
      state_ = McState::Idle;
    }
    return val;
  }

  case McState::Writing: {
    uint32_t addr =
        static_cast<uint32_t>(sectorAddr_) * SECTOR_SIZE + byteCounter_;
    if (addr < CARD_SIZE) {
      data_[addr] = dataIn;
    }
    checksum_ ^= dataIn;
    byteCounter_++;
    if (byteCounter_ >= SECTOR_SIZE) {
      state_ = McState::WriteAck;
    }
    return 0x00;
  }

  case McState::WriteAck:
    state_ = McState::Idle;
    return 0x47; // Good ("G")
  }

  return 0xFF;
}

// Input Controller

InputController::InputController() { reset(); }

void InputController::reset() {
  for (auto &p : ports_) {
    p = PortState{};
  }
  joyData_ = 0;
  joyStat_ = 0x05; // TX ready, TX not empty
  joyMode_ = 0;
  joyCtrl_ = 0;
  joyBaud_ = 0x88;
  sioState_ = SioState::Idle;
  selectedPort_ = 0;
  irqPending_ = false;
}

InputController::Snapshot InputController::snapshot() const {
  std::lock_guard lock(portMutex_);
  Snapshot state;
  for (int port = 0; port < 2; ++port) {
    state.ports[port].buttons =
        ports_[port].buttons & ~ports_[port].scriptedButtons;
    state.ports[port].analogLX = ports_[port].analogLX;
    state.ports[port].analogLY = ports_[port].analogLY;
    state.ports[port].analogRX = ports_[port].analogRX;
    state.ports[port].analogRY = ports_[port].analogRY;
    state.ports[port].padType = ports_[port].padType;
  }
  state.joyData = joyData_;
  state.joyStat = joyStat_;
  state.joyMode = joyMode_;
  state.joyCtrl = joyCtrl_;
  state.joyBaud = joyBaud_;
  state.sioState = static_cast<uint8_t>(sioState_);
  state.selectedPort = selectedPort_;
  state.irqPending = irqPending_;
  return state;
}

bool InputController::restore(const Snapshot &state) {
  if (state.sioState > static_cast<uint8_t>(SioState::MemCardTransfer) ||
      state.selectedPort > 1)
    return false;
  for (const auto &port : state.ports) {
    if (port.padType != PadType::None && port.padType != PadType::Digital &&
        port.padType != PadType::Analog && port.padType != PadType::DualShock)
      return false;
  }

  std::lock_guard lock(portMutex_);
  for (int port = 0; port < 2; ++port) {
    ports_[port].buttons = state.ports[port].buttons;
    ports_[port].scriptedButtons = 0;
    // Transient host edges deliberately do not enlarge the on-disk snapshot.
    // A restored controller starts from the saved physical state and acquires
    // fresh host transitions on the next VBlank.
    ports_[port].latchedButtons = state.ports[port].buttons;
    ports_[port].pendingPresses = 0;
    ports_[port].analogLX = state.ports[port].analogLX;
    ports_[port].analogLY = state.ports[port].analogLY;
    ports_[port].analogRX = state.ports[port].analogRX;
    ports_[port].analogRY = state.ports[port].analogRY;
    ports_[port].padType = state.ports[port].padType;
  }
  joyData_ = state.joyData;
  joyStat_ = state.joyStat;
  joyMode_ = state.joyMode;
  joyCtrl_ = state.joyCtrl;
  joyBaud_ = state.joyBaud;
  sioState_ = static_cast<SioState>(state.sioState);
  selectedPort_ = state.selectedPort;
  irqPending_ = state.irqPending;
  return true;
}

void InputController::press(uint16_t btn, int port) {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    const uint16_t newlyPressed = ports_[port].buttons & btn;
    ports_[port].buttons &= ~btn;
    // Make a press visible immediately to SIO/PadRead and remember its edge
    // until at least one VBlank has published it. This handles SDL batches
    // containing both KEYDOWN and KEYUP for a short tap.
    ports_[port].latchedButtons &= ~btn;
    ports_[port].pendingPresses |= newlyPressed;
  }
}

void InputController::release(uint16_t btn, int port) {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    ports_[port].buttons |= btn;
  }
}

void InputController::setScriptedButtons(uint16_t pressedMask, int port) {
  if (port < 0 || port >= 2)
    return;
  std::lock_guard lock(portMutex_);
  const uint16_t before = ports_[port].buttons & ~ports_[port].scriptedButtons;
  const uint16_t after = ports_[port].buttons & ~pressedMask;
  ports_[port].scriptedButtons = pressedMask;
  const uint16_t newlyPressed = before & ~after;
  ports_[port].latchedButtons &= after;
  ports_[port].pendingPresses |= newlyPressed;
}

void InputController::latchVBlank() {
  std::lock_guard lock(portMutex_);
  for (auto &port : ports_) {
    port.latchedButtons =
        (port.buttons & ~port.scriptedButtons) & ~port.pendingPresses;
    port.pendingPresses = 0;
  }
}

uint16_t InputController::buttonState(int port) const {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    return ports_[port].buttons & ~ports_[port].scriptedButtons;
  }
  return 0xFFFF;
}

uint16_t InputController::latchedButtonState(int port) const {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    return ports_[port].latchedButtons;
  }
  return 0xFFFF;
}

void InputController::setAnalog(int port, uint8_t lx, uint8_t ly, uint8_t rx,
                                uint8_t ry) {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    ports_[port].analogLX = lx;
    ports_[port].analogLY = ly;
    ports_[port].analogRX = rx;
    ports_[port].analogRY = ry;
  }
}

void InputController::setPadType(int port, PadType type) {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    ports_[port].padType = type;
  }
}

PadType InputController::getPadType(int port) const {
  if (port >= 0 && port < 2) {
    std::lock_guard lock(portMutex_);
    return ports_[port].padType;
  }
  return PadType::None;
}

// SIO Register Access

void InputController::writeRegister(uint32_t addr, uint32_t val) {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x00: // JOY_DATA
    joyData_ = val;
    {
      uint8_t response = processSioTransfer(val & 0xFF);
      joyData_ = response;
      irqPending_ = true;
    }
    break;
  }
}

uint32_t InputController::readRegister(uint32_t addr) const {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x00:
    return joyData_;
  case 0x04:
    return joyStat_;
  }
  return 0;
}

void InputController::writeRegister16(uint32_t addr, uint16_t val) {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x08:
    joyMode_ = val;
    break;
  case 0x0A:
    joyCtrl_ = val;
    if (val & (1 << 4)) { // Acknowledge
      irqPending_ = false;
    }
    if (val & (1 << 6)) { // Reset
      sioState_ = SioState::Idle;
      joyStat_ = 0x05;
    }
    // Select port (bit 13)
    selectedPort_ = (val >> 13) & 1;
    break;
  case 0x0E:
    joyBaud_ = val;
    break;
  }
}

uint16_t InputController::readRegister16(uint32_t addr) const {
  uint32_t offset = addr - 0x1F801040;
  switch (offset) {
  case 0x08:
    return joyMode_;
  case 0x0A:
    return joyCtrl_;
  case 0x0E:
    return joyBaud_;
  case 0x04:
    return joyStat_ & 0xFFFF;
  }
  return 0;
}

// SIO Transfer State Machine

uint8_t InputController::processSioTransfer(uint8_t dataIn) {
  PortState port;
  {
    std::lock_guard lock(portMutex_);
    port = ports_[selectedPort_];
    port.buttons = port.latchedButtons;
  }

  switch (sioState_) {
  case SioState::Idle:
    if (dataIn == 0x01) { // Start communication
      sioState_ = SioState::SelectDevice;
      return 0xFF;
    }
    if (dataIn == 0x81) { // Memory card
      sioState_ = SioState::MemCardTransfer;
      return memCards_[selectedPort_].transfer(dataIn);
    }
    return 0xFF;

  case SioState::SelectDevice:
    if (dataIn == 0x42) { // Read pad
      sioState_ = SioState::TransferId;
      return static_cast<uint8_t>(port.padType);
    }
    sioState_ = SioState::Idle;
    return 0xFF;

  case SioState::TransferId:
    sioState_ = SioState::TransferPadLo;
    return 0x5A; // Always 0x5A

  case SioState::TransferPadLo:
    sioState_ = SioState::TransferPadHi;
    return port.buttons & 0xFF;

  case SioState::TransferPadHi:
    if (port.padType == PadType::Analog || port.padType == PadType::DualShock) {
      sioState_ = SioState::TransferAnalogRX;
    } else {
      sioState_ = SioState::Idle;
    }
    return (port.buttons >> 8) & 0xFF;

  case SioState::TransferAnalogRX:
    sioState_ = SioState::TransferAnalogRY;
    return port.analogRX;

  case SioState::TransferAnalogRY:
    sioState_ = SioState::TransferAnalogLX;
    return port.analogRY;

  case SioState::TransferAnalogLX:
    sioState_ = SioState::TransferAnalogLY;
    return port.analogLX;

  case SioState::TransferAnalogLY:
    sioState_ = SioState::Idle;
    return port.analogLY;

  case SioState::MemCardTransfer:
    return memCards_[selectedPort_].transfer(dataIn);
  }

  return 0xFF;
}

} // namespace ps1::input
