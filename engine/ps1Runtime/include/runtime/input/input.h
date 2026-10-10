#pragma once

// ps1Runtime -- Input Controller
// PS1 Controller (Digital Pad, DualShock) and Memory Card via SIO

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ps1::input {

// PS1 Button Bits (active-low: 0 = pressed)
enum Button : uint16_t {
  BTN_SELECT = (1u << 0),
  BTN_L3 = (1u << 1),
  BTN_R3 = (1u << 2),
  BTN_START = (1u << 3),
  BTN_UP = (1u << 4),
  BTN_RIGHT = (1u << 5),
  BTN_DOWN = (1u << 6),
  BTN_LEFT = (1u << 7),
  BTN_L2 = (1u << 8),
  BTN_R2 = (1u << 9),
  BTN_L1 = (1u << 10),
  BTN_R1 = (1u << 11),
  BTN_TRIANGLE = (1u << 12),
  BTN_CIRCLE = (1u << 13),
  BTN_CROSS = (1u << 14),
  BTN_SQUARE = (1u << 15),
};

// Controller Type
enum class PadType : uint8_t {
  None = 0x00,
  Digital = 0x41,   // Standard digital pad
  Analog = 0x73,    // DualShock in analog mode
  DualShock = 0x79, // DualShock with pressure
};

// Memory Card
class MemoryCard {
public:
  static constexpr uint32_t CARD_SIZE = 128 * 1024; // 128KB
  static constexpr uint32_t BLOCK_SIZE = 8 * 1024;  // 8KB per block
  static constexpr uint32_t NUM_BLOCKS = 15;        // 15 usable blocks
  static constexpr uint32_t SECTOR_SIZE = 128;      // 128 bytes per sector

  struct Snapshot {
    std::array<uint8_t, CARD_SIZE> data{};
    bool present = true;
    uint8_t protocolState = 0;
    uint16_t sectorAddr = 0;
    uint32_t byteCounter = 0;
    uint8_t checksum = 0;
  };

  MemoryCard();

  void reset();
  Snapshot snapshot() const;
  void restore(const Snapshot &state);
  bool loadFromFile(const std::string &path);
  bool saveToFile(const std::string &path) const;

  bool attachFile(const std::string &path);
  bool flush() const;
  // A loaded timeline may replace card RAM. Protect the current on-disk card
  // again on its first subsequent write, even within the same process.
  void protectNextWrite() { sessionBackedUp_ = false; }
  const std::string &recoverySource() const { return recoverySource_; }
  int findFile(const std::string &name) const;
  int createFile(const std::string &name, unsigned blocks);
  bool eraseFile(const std::string &name);
  std::string fileName(unsigned block) const;
  uint32_t fileSize(unsigned block) const;
  std::vector<unsigned> fileChain(unsigned first) const;
  int readFile(unsigned first, unsigned offset, uint8_t *out, unsigned size) const;
  int writeFile(unsigned first, unsigned offset, const uint8_t *data, unsigned size);

  // SIO communication
  uint8_t transfer(uint8_t dataIn);
  bool isPresent() const { return present_; }
  void setPresent(bool p) { present_ = p; }

private:
  std::string backingPath_;
  mutable bool sessionBackedUp_ = false;
  std::string recoverySource_;
  void fixChecksum(unsigned offset);
  std::array<uint8_t, CARD_SIZE> data_;
  bool present_ = true;

  // SIO state machine
  enum class McState : uint8_t {
    Idle,
    AwaitCommand,
    AwaitAddrHi,
    AwaitAddrLo,
    Reading,
    Writing,
    WriteAck
  };
  McState state_ = McState::Idle;
  uint16_t sectorAddr_ = 0;
  uint32_t byteCounter_ = 0;
  uint8_t checksum_ = 0;
};

// Input Controller
class InputController {
public:
  struct PortSnapshot {
    uint16_t buttons = 0xFFFF;
    uint8_t analogLX = 0x80;
    uint8_t analogLY = 0x80;
    uint8_t analogRX = 0x80;
    uint8_t analogRY = 0x80;
    PadType padType = PadType::Digital;
  };

  struct Snapshot {
    std::array<PortSnapshot, 2> ports{};
    uint32_t joyData = 0;
    uint32_t joyStat = 0;
    uint16_t joyMode = 0;
    uint16_t joyCtrl = 0;
    uint16_t joyBaud = 0;
    uint8_t sioState = 0;
    uint8_t selectedPort = 0;
    bool irqPending = false;
  };

  InputController();
  void reset();
  Snapshot snapshot() const;
  static bool validSnapshot(const Snapshot &state);
  bool restore(const Snapshot &state);

  // Button state management
  void press(uint16_t btn, int port = 0);
  void release(uint16_t btn, int port = 0);
  // Active-high diagnostic overlay, combined with ordinary host input without
  // releasing a concurrently held keyboard/controller button.
  void setScriptedButtons(uint16_t pressedMask, int port = 0);
  // Publish one controller sample to the emulated machine. Press edges are
  // retained until this boundary, so a complete host tap cannot disappear
  // between two guest VBlanks.
  void latchVBlank();
  // TAS input is visible only at the recorded VBlank boundary, including
  // analog axes. Live host events cannot mutate a sample already published.
  void setFrameSampling(bool enabled);
  std::array<PortSnapshot, 2> latchedSample() const;
  void setMovieSample(const std::array<PortSnapshot, 2> &sample);
  uint16_t buttonState(int port = 0) const;
  uint16_t latchedButtonState(int port = 0) const;

  // Analog sticks (0x00-0xFF, 0x80 = center)
  void setAnalog(int port, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);

  // Controller type
  void setPadType(int port, PadType type);
  PadType getPadType(int port) const;

  // SIO register access (0x1F801040-0x1F80104F)
  void writeRegister(uint32_t addr, uint32_t val);
  uint32_t readRegister(uint32_t addr) const;
  void writeRegister16(uint32_t addr, uint16_t val);
  uint16_t readRegister16(uint32_t addr) const;

  // Memory card access
  MemoryCard &getMemoryCard(int slot) { return memCards_[slot]; }
  const MemoryCard &getMemoryCard(int slot) const { return memCards_[slot]; }

  // Check for pending IRQ (controller ACK)
  bool hasInterrupt() const { return irqPending_; }
  void clearInterrupt() { irqPending_ = false; }

private:
  // Port state
  struct PortState {
    uint16_t buttons = 0xFFFF;        // Physical host state, active low
    uint16_t scriptedButtons = 0;     // Diagnostic source, active high
    uint16_t latchedButtons = 0xFFFF; // Last state visible to the guest
    uint16_t pendingPresses = 0;      // Active-high edges not yet retired
    uint8_t analogLX = 0x80;
    uint8_t analogLY = 0x80;
    uint8_t analogRX = 0x80;
    uint8_t analogRY = 0x80;
    PadType padType = PadType::Digital;
  };

  PortState ports_[2];
  std::array<PortSnapshot, 2> latchedSample_{};
  bool frameSampling_ = false;
  mutable std::mutex portMutex_;

  // SIO state
  uint32_t joyData_ = 0;
  uint32_t joyStat_ = 0;
  uint16_t joyMode_ = 0;
  uint16_t joyCtrl_ = 0;
  uint16_t joyBaud_ = 0;

  // SIO transfer state machine
  enum class SioState : uint8_t {
    Idle,
    SelectDevice,
    TransferId,
    TransferPadLo,
    TransferPadHi,
    TransferAnalogRX,
    TransferAnalogRY,
    TransferAnalogLX,
    TransferAnalogLY,
    MemCardTransfer
  };
  SioState sioState_ = SioState::Idle;
  uint8_t selectedPort_ = 0;
  bool irqPending_ = false;

  // Memory cards (2 slots)
  MemoryCard memCards_[2];

  uint8_t processSioTransfer(uint8_t dataIn);
};

} // namespace ps1::input
