#include "runtime/mdec/mdec.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

namespace ps1::mdec {

// Zigzag Scan Order
const uint8_t MDEC::ZIGZAG[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
    3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

// Default Quantization Table
const uint8_t MDEC::DEFAULT_QUANT_TABLE[64] = {
    2,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// Constructor / Reset

MDEC::MDEC() {
  reset();
  initCosineTable();
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
  state.outputBuffer = outputBuffer_;
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
  std::copy(state.block.begin(), state.block.end(), std::begin(block_));
}

// Cosine Table

void MDEC::initCosineTable() {
  if (cosineTableReady_)
    return;
  for (int u = 0; u < 8; u++) {
    for (int x = 0; x < 8; x++) {
      double cu = (u == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
      cosineTable_[u * 8 + x] = static_cast<int32_t>(
          cu * std::cos((2.0 * x + 1.0) * u * M_PI / 16.0) * 4096);
    }
  }
  cosineTableReady_ = true;
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
  if (!outputBuffer_.empty()) {
    uint32_t val = outputBuffer_.front();
    outputBuffer_.erase(outputBuffer_.begin());
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
  // bits 23-25: current block (0-5)
  // bit 27: data-out request (for DMA)
  if (!dataOutEmpty())
    status |= (1 << 27);
  // bit 28: data-in request (for DMA)
  if (!dataInFull())
    status |= (1 << 28);
  if (outputDepth24_)
    status |= (1 << 25);
  if (outputSigned_)
    status |= (1 << 26);
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
    outputDepth24_ = (cmd >> 27) & 1;
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

  case 3: // Set scale table (cosine table) -- we use precomputed, just consume
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

  if (src >= end)
    return false;

  // First word: [15:10] = q_scale (6-bit), [9:0] = DC coefficient (10-bit
  // signed)
  uint16_t dcWord = *src++;
  uint8_t qScale = (dcWord >> 10) & 0x3F;
  int16_t dc =
      static_cast<int16_t>((dcWord & 0x3FF) << 6) >> 6; // sign-extend 10-bit
  block[0] = static_cast<int16_t>(
      std::clamp(((int32_t)dc * quantTable[0] + 4) >> 3, -1024, 1023));

  int pos = 0;
  while (src < end) {
    uint16_t rlWord = *src++;

    if (rlWord == 0xFE00)
      break; // End of block marker

    uint8_t runLength = (rlWord >> 10) & 0x3F;
    int16_t level = static_cast<int16_t>((rlWord & 0x3FF) << 6) >> 6;

    pos += runLength + 1;
    if (pos >= 64)
      break;

    // Dequantize: AC uses both q_scale and quantization table entry
    int32_t val = ((int32_t)level * qScale * quantTable[ZIGZAG[pos]] + 4) >> 3;
    block[ZIGZAG[pos]] = static_cast<int16_t>(std::clamp(val, -1024, 1023));
  }

  return true;
}

// iDCT

void MDEC::idct(int16_t *block) {
  int32_t temp[64];

  // Row transform
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      int32_t sum = 0;
      for (int u = 0; u < 8; u++) {
        sum += block[y * 8 + u] * cosineTable_[u * 8 + x];
      }
      temp[y * 8 + x] = (sum + 2048) >> 12;
    }
  }

  // Column transform
  for (int x = 0; x < 8; x++) {
    for (int y = 0; y < 8; y++) {
      int32_t sum = 0;
      for (int v = 0; v < 8; v++) {
        sum += temp[v * 8 + x] * cosineTable_[v * 8 + y];
      }
      block[y * 8 + x] =
          static_cast<int16_t>(std::clamp((sum + 2048) >> 12, -128, 127));
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

        int32_t r = yVal + ((crVal * 91881) >> 16);
        int32_t g = yVal - ((cbVal * 22554 + crVal * 46802) >> 16);
        int32_t b = yVal + ((cbVal * 116130) >> 16);

        uint16_t rgb15 = ((clampU8(r) >> 3) & 0x1F) |
                         (((clampU8(g) >> 3) & 0x1F) << 5) |
                         (((clampU8(b) >> 3) & 0x1F) << 10);
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

      int32_t r = yVal + ((crVal * 91881) >> 16);
      int32_t g = yVal - ((cbVal * 22554 + crVal * 46802) >> 16);
      int32_t b = yVal + ((cbVal * 116130) >> 16);

      pushByte(clampU8(r));
      pushByte(clampU8(g));
      pushByte(clampU8(b));
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

  if (opcode == 3)
    return; // Scale table -- ignore, using precomputed

  if (opcode == 1) {
    // Decode macroblocks
    const uint16_t *src = inputBuffer_.data();
    const uint16_t *end = src + inputBuffer_.size();

    if (kMdecDebug)
      fmt::print("[MDEC] decodeSlice: {} words of input\n",
                 inputBuffer_.size());

    int mbCount = 0;

    while (src < end) {
      // Each macroblock = 6 blocks: Cr, Cb, Y1, Y2, Y3, Y4
      int16_t crBlock[64], cbBlock[64], y1Block[64], y2Block[64], y3Block[64],
          y4Block[64];

      if (!rleDecode(src, end, crBlock, chromaQuantTable_.data()))
        break;
      idct(crBlock);
      if (!rleDecode(src, end, cbBlock, chromaQuantTable_.data()))
        break;
      idct(cbBlock);
      if (!rleDecode(src, end, y1Block, lumaQuantTable_.data()))
        break;
      idct(y1Block);
      if (!rleDecode(src, end, y2Block, lumaQuantTable_.data()))
        break;
      idct(y2Block);
      if (!rleDecode(src, end, y3Block, lumaQuantTable_.data()))
        break;
      idct(y3Block);
      if (!rleDecode(src, end, y4Block, lumaQuantTable_.data()))
        break;
      idct(y4Block);

      if (outputDepth24_) {
        yuvToRgb24(crBlock, cbBlock, y1Block, y2Block, y3Block, y4Block);
      } else {
        yuvToRgb15(crBlock, cbBlock, y1Block, y2Block, y3Block, y4Block);
      }

      mbCount++;
      if (kMdecDebug && mbCount <= 4)
        fmt::print("[MDEC]   macroblock {} ok\n", mbCount);
    }

    if (kMdecDebug)
      fmt::print("[MDEC] decodeSlice: {} macroblocks decoded\n", mbCount);
  }
}

} // namespace ps1::mdec
