#include "runtime/mdec/mdec.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

namespace ps1::mdec {

// Zigzag Scan Order
const uint8_t MDEC::ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Default Quantization Table
const uint8_t MDEC::DEFAULT_QUANT_TABLE[64] = {
    2,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// Constructor / Reset

MDEC::MDEC() {
  reset();
}

void MDEC::reset() {
  state_ = State::Idle;
  busy_ = false;
  currentCommand_ = 0;
  outputDepth24_ = false;
  outputSigned_ = false;
  remainingWords_ = 0;
  std::memcpy(lumaQuantTable_.data(), DEFAULT_QUANT_TABLE, 64);
  std::memcpy(chromaQuantTable_.data(), DEFAULT_QUANT_TABLE, 64);
  hasCustomQuantTable_ = false;
  inputBuffer_.clear();
  outputBuffer_.clear();
  outputReadIndex_ = 0;
  initCosineTable();
  std::memset(block_, 0, sizeof(block_));
}

MDEC::Snapshot MDEC::snapshot() const {
  Snapshot state;
  state.state = static_cast<uint8_t>(state_);
  state.busy = busy_;
  state.currentCommand = currentCommand_;
  state.outputDepth24 = outputDepth24_;
  state.outputSigned = outputSigned_;
  state.remainingWords = remainingWords_;
  state.lumaQuantTable = lumaQuantTable_;
  state.chromaQuantTable = chromaQuantTable_;
  state.hasCustomQuantTable = hasCustomQuantTable_;
  state.inputBuffer = inputBuffer_;
  state.outputBuffer.assign(outputBuffer_.begin() + outputReadIndex_, outputBuffer_.end());
  state.scaleTable = scaleTable_;
  std::copy(std::begin(block_), std::end(block_), state.block.begin());
  return state;
}

void MDEC::restore(const Snapshot &state) {
  if (state.state > static_cast<uint8_t>(State::Decoding)) {
    reset();
    return;
  }
  state_ = static_cast<State>(state.state);
  busy_ = state.busy;
  currentCommand_ = state.currentCommand;
  outputDepth24_ = state.outputDepth24;
  outputSigned_ = state.outputSigned;
  remainingWords_ = state.remainingWords;
  lumaQuantTable_ = state.lumaQuantTable;
  chromaQuantTable_ = state.chromaQuantTable;
  hasCustomQuantTable_ = state.hasCustomQuantTable;
  inputBuffer_ = state.inputBuffer;
  outputBuffer_ = state.outputBuffer;
  outputReadIndex_ = 0;
  scaleTable_ = state.scaleTable;
  std::copy(state.block.begin(), state.block.end(), std::begin(block_));
}

// Cosine Table

void MDEC::initCosineTable() {
  for (int u = 0; u < 8; u++) {
    for (int x = 0; x < 8; x++) {
      double cu = (u == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
      scaleTable_[u * 8 + x] = static_cast<int16_t>(
          cu * std::cos((2.0 * x + 1.0) * u * M_PI / 16.0) * 32768);
    }
  }
}

// Register I/O

void MDEC::writeCommand(uint32_t val) {
  if (state_ == State::Idle) {
    processCommand(val);
  } else if (state_ == State::ReceivingData) {
    inputBuffer_.push_back(val & 0xFFFF);
    inputBuffer_.push_back((val >> 16) & 0xFFFF);
    remainingWords_--;
    if (remainingWords_ == 0) {
      decodeSlice();
      state_ = State::Idle;
      busy_ = false;
    }
  }
}

void MDEC::writeControl(uint32_t val) {
  if (val & (1u << 31)) { // Reset
    reset();
  }
}

uint32_t MDEC::readData() {
  if (!dataOutEmpty()) {
    uint32_t val = outputBuffer_[outputReadIndex_++];
    if (dataOutEmpty()) {
      outputBuffer_.clear();
      outputReadIndex_ = 0;
    }
    return val;
  }
  return 0;
}

uint32_t MDEC::readStatus() const {
  uint32_t status = 0;
  if (dataOutEmpty())
    status |= (1 << 31); // Data-out FIFO empty
  if (dataInFull())
    status |= (1 << 30); // Data-in FIFO full
  if (busy_)
    status |= (1 << 29); // Busy
  // Output format lives in bits 23-26; block index is not modeled here.
  // bit 27: data-out request (for DMA)
  if (!dataOutEmpty())
    status |= (1 << 27);
  // bit 28: data-in request (for DMA)
  if (!dataInFull())
    status |= (1 << 28);
  status |= ((currentCommand_ >> 27) & 3u) << 25;
  if (outputSigned_)
    status |= (1 << 24);
  status |= ((currentCommand_ >> 25) & 1u) << 23;
  status |= (remainingWords_ - 1u) & 0xFFFFu;
  return status;
}

// DMA Interface

void MDEC::dmaIn(const uint32_t *data, uint32_t wordCount) {
  for (uint32_t i = 0; i < wordCount; i++) {
    writeCommand(data[i]);
  }
}

uint32_t MDEC::dmaOutRead() { return readData(); }

// Command Processing

void MDEC::processCommand(uint32_t cmd) {
  uint8_t opcode = (cmd >> 29) & 0x7;

  switch (opcode) {
  case 0: // NOP
    busy_ = false;
    break;

  case 1: // Decode macroblock(s)
    currentCommand_ = cmd;
    outputDepth24_ = ((cmd >> 27) & 3u) == 2;
    outputSigned_ = (cmd >> 26) & 1;
    remainingWords_ = cmd & 0xFFFF;
    if (remainingWords_ == 0)
      remainingWords_ = 0x10000;
    state_ = State::ReceivingData;
    busy_ = true;
    inputBuffer_.clear();
    break;

  case 2: // Set quantization table(s)
    currentCommand_ = cmd;
    remainingWords_ = (cmd & 1) ? 32 : 16; // 64 or 128 bytes
    state_ = State::ReceivingData;
    busy_ = true;
    inputBuffer_.clear();
    break;

  case 3: // Upload the game's fixed-point IDCT scale table.
    currentCommand_ = cmd;
    remainingWords_ = 32;
    state_ = State::ReceivingData;
    busy_ = true;
    inputBuffer_.clear();
    break;

  default:
    fmt::print("[MDEC] Unknown command opcode: {}\n", opcode);
    busy_ = false;
    break;
  }
}

// RLE Decode

bool MDEC::rleDecode(const uint16_t *&src, const uint16_t *end, int16_t *block,
                     const uint8_t *quantTable) {
  std::memset(block, 0, 64 * sizeof(int16_t));

  while (src < end && *src == 0xFE00) ++src; // DMA alignment padding
  if (src >= end)
    return false;

  // First word: [15:10] = q_scale (6-bit), [9:0] = DC coefficient (10-bit
  // signed)
  uint16_t dcWord = *src++;
  uint8_t qScale = (dcWord >> 10) & 0x3F;
  int16_t dc =
      static_cast<int16_t>((dcWord & 0x3FF) << 6) >> 6; // sign-extend 10-bit
  block[0] = static_cast<int16_t>(
      std::clamp(int32_t(dc) * (qScale ? quantTable[0] : 2), -1024, 1023));

  int pos = 0;
  while (src < end) {
    uint16_t rlWord = *src++;

    if (rlWord == 0xFE00)
      return true; // Explicit end of block, including a DC-only block.

    uint8_t runLength = (rlWord >> 10) & 0x3F;
    int16_t level = static_cast<int16_t>((rlWord & 0x3FF) << 6) >> 6;

    pos += runLength + 1;
    if (pos >= 64)
      return true;

    // Dequantize: AC uses both q_scale and quantization table entry
    int32_t val = qScale ? (int32_t(level) * qScale * quantTable[pos] + 4) / 8 : int32_t(level) * 2;
    block[qScale ? ZIGZAG[pos] : pos] = static_cast<int16_t>(std::clamp(val, -1024, 1023));
    if (pos == 63) return true;
  }

  // Exhausting the command is not an end-of-block marker. In particular a
  // truncated sixth block must never fabricate a complete macroblock.
  return false;
}

// iDCT

void MDEC::idct(int16_t *block) {
  // The uploaded matrix uses signed 15-bit fractional coefficients. Keep
  // both passes wide and round once; this also handles non-default tables.
  int64_t intermediate[64]{};
  for (int row = 0; row < 8; ++row)
    for (int col = 0; col < 8; ++col)
      for (int frequency = 0; frequency < 8; ++frequency)
        intermediate[row * 8 + col] +=
            int64_t(block[frequency * 8 + col]) * scaleTable_[frequency * 8 + row];
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      int64_t sum = 0;
      for (int frequency = 0; frequency < 8; ++frequency)
        sum += intermediate[row * 8 + frequency] * scaleTable_[frequency * 8 + col];
      const int value = int((sum + (int64_t(1) << 31)) >> 32);
      const int signedNineBits = ((value & 0x1ff) ^ 0x100) - 0x100;
      block[row * 8 + col] = int16_t(std::clamp(signedNineBits, -128, 127));
    }
  }
}

// YCbCr -> RGB

int32_t MDEC::clampS(int32_t val, int32_t lo, int32_t hi) {
  return std::clamp(val, lo, hi);
}

uint8_t MDEC::clampU8(int32_t val) {
  return static_cast<uint8_t>(std::clamp(val, 0, 255));
}

void MDEC::yuvToRgb15(const int16_t *cr, const int16_t *cb, const int16_t *y1,
                      const int16_t *y2, const int16_t *y3, const int16_t *y4) {
  // 4 Y blocks (each 8x8) -> 16x16 pixel macroblock
  // Output in scan-line order (left-to-right, top-to-bottom), 2 pixels per word
  const int16_t *yBlocks[4] = {y1, y2, y3, y4};

  for (int y = 0; y < 16; y++) {
    int by = y >> 3;
    int py = y & 7;
    for (int x = 0; x < 16; x += 2) {
      uint32_t word = 0;
      for (int i = 0; i < 2; i++) {
        int xi = x + i;
        int bx = xi >> 3;
        int px = xi & 7;
        int cx = xi >> 1; // chroma x (2:1 subsampled)
        int cy = y >> 1;  // chroma y (2:1 subsampled)

        const int16_t *yBlock = yBlocks[by * 2 + bx];
        int32_t yVal = (int32_t)yBlock[py * 8 + px] + 128;
        int32_t crVal = cr[cy * 8 + cx];
        int32_t cbVal = cb[cy * 8 + cx];

        int32_t r = yVal + ((crVal * 22970) / 16384);
        int32_t g = yVal - ((cbVal * 5631 + crVal * 11703) / 16384);
        int32_t b = yVal + ((cbVal * 29032) / 16384);

        const auto component = [&](int32_t channel) {
          const uint8_t byte = clampU8(channel) ^ (outputSigned_ ? 0x80 : 0);
          return std::min((uint32_t(byte) + 4) >> 3, 31u);
        };
        uint16_t rgb15 = component(r) | (component(g) << 5) |
                         (component(b) << 10) | ((currentCommand_ & (1u << 25)) ? 0x8000u : 0u);
        word |= (uint32_t)rgb15 << (i * 16);
      }
      outputBuffer_.push_back(word);
    }
  }
}

void MDEC::yuvToRgb24(const int16_t *cr, const int16_t *cb, const int16_t *y1,
                      const int16_t *y2, const int16_t *y3, const int16_t *y4) {
  // 24bpp: 3 bytes per pixel packed continuously into 32-bit words (scan-line
  // order) 256 pixels x 3 bytes = 768 bytes = 192 words
  const int16_t *yBlocks[4] = {y1, y2, y3, y4};

  uint32_t accum = 0;
  int bytePos = 0;

  auto pushByte = [&](uint8_t byte) {
    accum |= (uint32_t)byte << (bytePos * 8);
    bytePos++;
    if (bytePos == 4) {
      outputBuffer_.push_back(accum);
      accum = 0;
      bytePos = 0;
    }
  };

  for (int y = 0; y < 16; y++) {
    int by = y >> 3;
    int py = y & 7;
    for (int x = 0; x < 16; x++) {
      int bx = x >> 3;
      int px = x & 7;
      int cx = x >> 1;
      int cy = y >> 1;

      const int16_t *yBlock = yBlocks[by * 2 + bx];
      int32_t yVal = (int32_t)yBlock[py * 8 + px] + 128;
      int32_t crVal = cr[cy * 8 + cx];
      int32_t cbVal = cb[cy * 8 + cx];

      int32_t r = yVal + ((crVal * 22970) / 16384);
      int32_t g = yVal - ((cbVal * 5631 + crVal * 11703) / 16384);
      int32_t b = yVal + ((cbVal * 29032) / 16384);

      const uint8_t signMask = outputSigned_ ? 0x80 : 0;
      pushByte(clampU8(r) ^ signMask);
      pushByte(clampU8(g) ^ signMask);
      pushByte(clampU8(b) ^ signMask);
    }
  }

  if (bytePos > 0)
    outputBuffer_.push_back(accum);
}

// Decode Slice

void MDEC::decodeSlice() {
  uint8_t opcode = (currentCommand_ >> 29) & 0x7;

  // MDEC diagnostics
  // Enabled by env var PS1_MDEC_DEBUG=1. Logs macroblock counts so failures
  // in rleDecode() (invalid VLC IDs, misaligned RLE data) are visible.
  // Expected output per FMV frame: "N macroblocks decoded" where
  //   N = (frame_width/16) * (frame_height/16).  For 320x240: N = 300.
  // If N=0 -> first block fails (bad DC word or missing quant table).
  // If N>0 but << 300 -> AC RLE misaligned or early end-marker.
  static const bool kMdecDebug = (std::getenv("PS1_MDEC_DEBUG") != nullptr);

  if (opcode == 2) {
    // Upload quantization table
    for (uint32_t i = 0; i < 64 && i < inputBuffer_.size() * 2; i++) {
      uint8_t val;
      if (i % 2 == 0) {
        val = inputBuffer_[i / 2] & 0xFF;
      } else {
        val = (inputBuffer_[i / 2] >> 8) & 0xFF;
      }
      lumaQuantTable_[i] = val;
    }
    if (currentCommand_ & 1) {
      // Also upload chroma table
      for (uint32_t i = 0; i < 64; i++) {
        uint32_t srcIdx = 64 + i;
        if (srcIdx / 2 < inputBuffer_.size()) {
          uint8_t val;
          if (srcIdx % 2 == 0) {
            val = inputBuffer_[srcIdx / 2] & 0xFF;
          } else {
            val = (inputBuffer_[srcIdx / 2] >> 8) & 0xFF;
          }
          chromaQuantTable_[i] = val;
        }
      }
    }
    hasCustomQuantTable_ = true;
    return;
  }

  if (opcode == 3) {
    for (size_t i = 0; i < scaleTable_.size() && i < inputBuffer_.size(); ++i)
      scaleTable_[i] = static_cast<int16_t>(inputBuffer_[i]);
    return;
  }

  if (opcode == 1) {
    // Decode macroblocks
    const uint16_t *src = inputBuffer_.data();
    const uint16_t *end = src + inputBuffer_.size();

    if (kMdecDebug)
      fmt::print("[MDEC] decodeSlice: {} words of input\n",
                 inputBuffer_.size());

    int mbCount = 0;
    bool truncated = false;

    while (src < end) {
      // Leading/trailing DMA alignment is legal and carries no pixels.
      while (src < end && *src == 0xfe00) ++src;
      if (src == end) break;
      // Each macroblock = 6 blocks: Cr, Cb, Y1, Y2, Y3, Y4
      int16_t blocks[6][64];
      for (unsigned i = 0; i < 6; ++i) {
        if (!rleDecode(src, end, blocks[i], i < 2 ? chromaQuantTable_.data() : lumaQuantTable_.data())) {
          truncated = true;
          break;
        }
        idct(blocks[i]);
      }
      if (truncated) break; // Keep previously completed macroblocks available.

      if (outputDepth24_) {
        yuvToRgb24(blocks[0], blocks[1], blocks[2], blocks[3], blocks[4], blocks[5]);
      } else {
        yuvToRgb15(blocks[0], blocks[1], blocks[2], blocks[3], blocks[4], blocks[5]);
      }

      mbCount++;
      if (kMdecDebug && mbCount <= 4)
        fmt::print("[MDEC]   macroblock {} ok\n", mbCount);
    }

    ps1::metrics::count("mdec.decode_command");
    if (truncated) ps1::metrics::count("mdec.truncated_command");
    // The game submits one complete movie frame per command. Certifying its
    // dimensions still requires the macroblock count from the source catalog.
    if (mbCount && !truncated) ps1::metrics::count("mdec.decoded_frame");
    ps1::metrics::count("mdec.decoded_macroblock", mbCount);
    if (kMdecDebug)
      fmt::print("[MDEC] decodeSlice: {} macroblocks decoded\n", mbCount);
  }
}

} // namespace ps1::mdec
