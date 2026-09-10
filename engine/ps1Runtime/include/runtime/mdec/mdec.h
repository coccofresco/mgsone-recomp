#pragma once

// ps1Runtime -- MDEC (Motion Decoder)
// PS1 hardware FMV decoder: RLE -> iDCT -> YCbCr -> RGB

#include <array>
#include <cstdint>
#include <vector>

namespace ps1::mdec {

class MDEC {
public:
  struct Snapshot {
    uint8_t state = 0;
    bool busy = false;
    uint32_t currentCommand = 0;
    bool outputDepth24 = false;
    bool outputSigned = false;
    uint32_t remainingWords = 0;
    std::array<uint8_t, 64> lumaQuantTable{};
    std::array<uint8_t, 64> chromaQuantTable{};
    bool hasCustomQuantTable = false;
    std::vector<uint16_t> inputBuffer;
    std::vector<uint32_t> outputBuffer;
    std::array<int16_t, 64> block{};
  };

  MDEC();
  void reset();
  Snapshot snapshot() const;
  void restore(const Snapshot &state);

  // Register I/O
  void writeCommand(uint32_t val); // 0x1F801820 write
  void writeControl(uint32_t val); // 0x1F801824 write
  uint32_t readData();             // 0x1F801820 read
  uint32_t readStatus() const;     // 0x1F801824 read

  // DMA interface
  void dmaIn(const uint32_t *data, uint32_t wordCount);
  bool dmaOutReady() const { return !outputBuffer_.empty(); }
  uint32_t dmaOutRead();
  uint32_t dmaOutRemaining() const {
    return static_cast<uint32_t>(outputBuffer_.size());
  }

  // Status
  bool isBusy() const { return busy_; }
  bool dataInFull() const { return inputBuffer_.size() >= 0xFFFF; }
  bool dataOutEmpty() const { return outputBuffer_.empty(); }

  // Zigzag scan order (public for testing)
  static const uint8_t ZIGZAG[64];

private:
  // Command state
  enum class State : uint8_t { Idle, ReceivingData, Decoding };
  State state_ = State::Idle;
  bool busy_ = false;

  // Current command
  uint32_t currentCommand_ = 0;
  bool outputDepth24_ = false; // 0 = 15-bit, 1 = 24-bit
  bool outputSigned_ = false;
  uint32_t remainingWords_ = 0;

  // Quantization tables
  std::array<uint8_t, 64> lumaQuantTable_;
  std::array<uint8_t, 64> chromaQuantTable_;
  bool hasCustomQuantTable_ = false;

  // Cosine table for iDCT (precomputed)
  std::array<int32_t, 64 * 64> cosineTable_;
  bool cosineTableReady_ = false;

  // Data buffers
  std::vector<uint16_t> inputBuffer_;
  std::vector<uint32_t> outputBuffer_;

  // 8x8 block scratch
  int16_t block_[64]; // current 8x8 block

  // Internal methods
  void processCommand(uint32_t cmd);
  void decodeSlice();

  // RLE decode: variable-length codes -> 8x8 quantized coefficients
  bool rleDecode(const uint16_t *&src, const uint16_t *end, int16_t *block,
                 const uint8_t *quantTable);

  // Inverse Discrete Cosine Transform
  void idct(int16_t *block);
  void initCosineTable();

  // YCbCr -> RGB conversion (6 blocks -> macroblock)
  void yuvToRgb15(const int16_t *cr, const int16_t *cb, const int16_t *y1,
                  const int16_t *y2, const int16_t *y3, const int16_t *y4);
  void yuvToRgb24(const int16_t *cr, const int16_t *cb, const int16_t *y1,
                  const int16_t *y2, const int16_t *y3, const int16_t *y4);

  // Default quantization table
  static const uint8_t DEFAULT_QUANT_TABLE[64];

  // Clamp helpers
  static int32_t clampS(int32_t val, int32_t lo, int32_t hi);
  static uint8_t clampU8(int32_t val);
};

} // namespace ps1::mdec
