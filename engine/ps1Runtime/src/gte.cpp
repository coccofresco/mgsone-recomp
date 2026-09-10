// ps1Runtime -- GTE Runtime Implementation
// PS1-accurate fixed-point math for all 22 GTE commands

#include "runtime/gte_color_math.h"
#include "runtime/gte_projection_math.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <runtime/games/crash3_profile.h>
#include <runtime/gte.h>
#include <runtime/gte_control_provenance.h>
#include <runtime/metrics.h>
#include <runtime/mips_construction_lineage.h>
#include <runtime/mips_interpreter.h>
#include <runtime/mips_value_flow.h>

namespace ps1 {

namespace metrics::detail {
extern std::atomic<int> g_gteTraceFastGate;
} // namespace metrics::detail

namespace {

std::atomic<uint64_t> nextProjectionId{1};

uint64_t allocateProjectionId() {
  uint64_t id = nextProjectionId.fetch_add(1, std::memory_order_relaxed);
  if (id == 0)
    id = nextProjectionId.fetch_add(1, std::memory_order_relaxed);
  return id;
}

uint64_t hashProjectionContext(const CPUContext *ctx, bool sf) {
  uint64_t hash = 1469598103934665603ull;
  auto add = [&hash](uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash ^= (value >> shift) & 0xffu;
      hash *= 1099511628211ull;
    }
  };
  add(static_cast<uint16_t>(ctx->cop2c[GTE_H]));
  add(ctx->cop2c[GTE_OFX]);
  add(ctx->cop2c[GTE_OFY]);
  add(sf ? 1u : 0u);
  return hash;
}

uint64_t hashProjectionTransform(const int16_t rotation[3][3],
                                 const int32_t translation[3]) {
  uint64_t hash = 1469598103934665603ull;
  auto add = [&hash](uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash ^= (value >> shift) & 0xffu;
      hash *= 1099511628211ull;
    }
  };
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column)
      add(static_cast<uint16_t>(rotation[row][column]));
  }
  for (std::size_t index = 0; index < 3; ++index)
    add(static_cast<uint32_t>(translation[index]));
  return hash;
}

} // namespace

uint64_t GTE::projectionTransformLineage(const CPUContext *ctx) noexcept {
  uint64_t hash = 1469598103934665603ull;
  auto add32 = [&hash](uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash ^= (value >> shift) & 0xffu;
      hash *= 1099511628211ull;
    }
  };
  auto add64 = [&add32](uint64_t value) {
    add32(static_cast<uint32_t>(value));
    add32(static_cast<uint32_t>(value >> 32));
  };
  for (uint8_t reg = GTE_RT11RT12; reg <= GTE_TRZ; ++reg) {
    add32(ctx->gteControlWriterPc[reg]);
    add64(ctx->gteControlSourceToken[reg]);
  }
  return hash;
}

bool GTE::projectionTransformLineageComplete(const CPUContext *ctx) noexcept {
  for (uint8_t reg = GTE_RT11RT12; reg <= GTE_TRZ; ++reg) {
    if (ctx->gteControlWriteGeneration[reg] == 0)
      return false;
  }
  return true;
}

namespace {

uint64_t sourceAddressToken(uint32_t address) {
  return static_cast<uint64_t>(address) + 1u;
}

int64_t traceSourceAddress(uint64_t token) {
  return token == 0 ? -1 : static_cast<int64_t>(token - 1u);
}

int64_t coherentTransformSource(const CPUContext *ctx) {
  const uint64_t first = ctx->gteControlSourceToken[GTE_RT11RT12];
  if (first == 0)
    return -1;
  const uint32_t base = static_cast<uint32_t>(first - 1u);
  for (uint8_t reg = GTE_RT11RT12; reg <= GTE_TRZ; ++reg) {
    const uint32_t expectedOffset =
        reg <= GTE_RT33 ? static_cast<uint32_t>(reg) * 4u
                        : 20u + static_cast<uint32_t>(reg - GTE_TRX) * 4u;
    if (ctx->gteControlSourceToken[reg] !=
        sourceAddressToken(base + expectedOffset))
      return -1;
  }
  return static_cast<int64_t>(base);
}

int64_t coherentModelVertexSource(const CPUContext *ctx, uint8_t vecIdx) {
  const uint8_t xyReg = static_cast<uint8_t>(GTE_VXY0 + vecIdx * 2u);
  const uint8_t zReg = static_cast<uint8_t>(xyReg + 1u);
  const uint64_t xy = ctx->gteDataSourceToken[xyReg];
  if (xy == 0)
    return -1;
  const uint32_t base = static_cast<uint32_t>(xy - 1u);
  return ctx->gteDataSourceToken[zReg] == sourceAddressToken(base + 4u)
             ? static_cast<int64_t>(base)
             : -1;
}

uint32_t hashGteRegisters(const CPUContext *ctx) {
  uint32_t hash = 2166136261u;
  auto add = [&hash](uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash ^= (value >> shift) & 0xffu;
      hash *= 16777619u;
    }
  };
  for (const uint32_t value : ctx->cop2d)
    add(value);
  for (const uint32_t value : ctx->cop2c)
    add(value);
  return hash;
}

inline bool gteTraceEnabledFast() {
  // The first call resolves the full trace contract. Once metrics proves that
  // tracing is absent, the hot GTE path pays one relaxed load and a predictable
  // branch instead of repeated out-of-line environment/arming checks.
  if (metrics::detail::g_gteTraceFastGate.load(std::memory_order_relaxed) == 0)
    return false;
  return metrics::gteTraceEnabled();
}

// Explicit, bounded diagnostic companion to the existing GTE trace. Disabled
// tracing never opens a file or copies these registers. The independent donor
// adapter consumes the input half; the recorded output detects shared errors
// between the architectural GTE and its presentation-side reconstructions.
FILE *gteColorCaptureFile(uint8_t command) {
  switch (command) {
  case 0x10:
  case 0x11:
  case 0x13:
  case 0x14:
  case 0x16:
  case 0x1b:
  case 0x1c:
  case 0x1e:
  case 0x20:
  case 0x29:
  case 0x2a:
  case 0x3f:
    break;
  default:
    return nullptr;
  }
  struct CloseFile {
    void operator()(FILE *file) const { std::fclose(file); }
  };
  static const std::unique_ptr<FILE, CloseFile> file([] {
    const char *path = std::getenv("PS1_GTE_COLOR_CAPTURE");
    FILE *output = path != nullptr && *path ? std::fopen(path, "wb") : nullptr;
    if (output != nullptr)
      std::fwrite("GTECOL1\0", 1, 8, output);
    return output;
  }());
  return file.get();
}

class GteTraceScope {
public:
  GteTraceScope(CPUContext *ctx, uint8_t command, bool sf, bool lm)
      : ctx_(ctx), command_(command), sf_(sf), lm_(lm),
        enabled_(gteTraceEnabledFast()),
        inputHash_(enabled_ ? hashGteRegisters(ctx) : 0) {
    if (enabled_ && (captureFile_ = gteColorCaptureFile(command_)) != nullptr) {
      capture_ = std::make_unique<std::array<uint32_t, 130>>();
      (*capture_)[0] = ctx_->pc;
      (*capture_)[1] = command_ | (uint32_t{sf_} << 19) | (uint32_t{lm_} << 10);
      std::copy_n(ctx_->cop2d, 32, capture_->begin() + 2);
      std::copy_n(ctx_->cop2c, 32, capture_->begin() + 34);
    }
  }

  ~GteTraceScope() {
    if (!enabled_)
      return;
    if (capture_) {
      std::copy_n(ctx_->cop2d, 32, capture_->begin() + 66);
      std::copy_n(ctx_->cop2c, 32, capture_->begin() + 98);
      std::fwrite(capture_->data(), sizeof(*capture_), 1, captureFile_);
      // Bounded diagnostics may terminate with _Exit at a VBlank boundary.
      // Do not leave the final record in a C-library buffer awaiting exit().
      std::fflush(captureFile_);
    }
    metrics::trace("gte.command", {{"pc", ctx_->pc},
                                   {"command", command_},
                                   {"sf", sf_ ? 1 : 0},
                                   {"lm", lm_ ? 1 : 0},
                                   {"input_hash", inputHash_},
                                   {"output_hash", hashGteRegisters(ctx_)},
                                   {"flag", ctx_->cop2c[GTE_FLAG]},
                                   {"ir0", ctx_->cop2d[GTE_IR0]},
                                   {"ir1", ctx_->cop2d[GTE_IR1]},
                                   {"ir2", ctx_->cop2d[GTE_IR2]},
                                   {"ir3", ctx_->cop2d[GTE_IR3]},
                                   {"vxy0", ctx_->cop2d[GTE_VXY0]},
                                   {"vz0", ctx_->cop2d[GTE_VZ0]},
                                   {"vxy1", ctx_->cop2d[GTE_VXY1]},
                                   {"vz1", ctx_->cop2d[GTE_VZ1]},
                                   {"vxy2", ctx_->cop2d[GTE_VXY2]},
                                   {"vz2", ctx_->cop2d[GTE_VZ2]},
                                   {"sxy0", ctx_->cop2d[GTE_SXY0]},
                                   {"sxy1", ctx_->cop2d[GTE_SXY1]},
                                   {"sxy2", ctx_->cop2d[GTE_SXY2]},
                                   {"sz0", ctx_->cop2d[GTE_SZ0]},
                                   {"sz1", ctx_->cop2d[GTE_SZ1]},
                                   {"sz2", ctx_->cop2d[GTE_SZ2]},
                                   {"sz3", ctx_->cop2d[GTE_SZ3]},
                                   {"mac0", ctx_->cop2d[GTE_MAC0]},
                                   {"mac1", ctx_->cop2d[GTE_MAC1]},
                                   {"mac2", ctx_->cop2d[GTE_MAC2]},
                                   {"mac3", ctx_->cop2d[GTE_MAC3]},
                                   {"rgb2", ctx_->cop2d[GTE_RGB2]},
                                   {"otz", ctx_->cop2d[GTE_OTZ]},
                                   {"rt11rt12", ctx_->cop2c[GTE_RT11RT12]},
                                   {"rt13rt21", ctx_->cop2c[GTE_RT13RT21]},
                                   {"rt22rt23", ctx_->cop2c[GTE_RT22RT23]},
                                   {"rt31rt32", ctx_->cop2c[GTE_RT31RT32]},
                                   {"rt33", ctx_->cop2c[GTE_RT33]},
                                   {"trx", ctx_->cop2c[GTE_TRX]},
                                   {"try", ctx_->cop2c[GTE_TRY]},
                                   {"trz", ctx_->cop2c[GTE_TRZ]},
                                   {"ofx", ctx_->cop2c[GTE_OFX]},
                                   {"ofy", ctx_->cop2c[GTE_OFY]},
                                   {"h", ctx_->cop2c[GTE_H]},
                                   {"zsf3", ctx_->cop2c[GTE_ZSF3]},
                                   {"zsf4", ctx_->cop2c[GTE_ZSF4]}});
  }

private:
  FILE *captureFile_ = nullptr;
  std::unique_ptr<std::array<uint32_t, 130>> capture_;
  CPUContext *ctx_;
  uint8_t command_;
  bool sf_;
  bool lm_;
  bool enabled_;
  uint32_t inputHash_;
};

} // namespace

// CLZ (Count Leading Zeros)

static int countLeadingZeros(uint32_t val) {
  if (val == 0)
    return 32;
#ifdef __GNUC__
  return __builtin_clz(val);
#else
  int n = 0;
  if ((val & 0xFFFF0000) == 0) {
    n += 16;
    val <<= 16;
  }
  if ((val & 0xFF000000) == 0) {
    n += 8;
    val <<= 8;
  }
  if ((val & 0xF0000000) == 0) {
    n += 4;
    val <<= 4;
  }
  if ((val & 0xC0000000) == 0) {
    n += 2;
    val <<= 2;
  }
  if ((val & 0x80000000) == 0) {
    n += 1;
  }
  return n;
#endif
}

// Register Access

uint32_t GTE::readData(CPUContext *ctx, uint8_t reg) {
  reg &= 0x1F;
  if (reg == GTE_SXY0 &&
      (ctx->pc == UINT32_C(0x8003FD34) || ctx->pc == UINT32_C(0x80040C7C) ||
       ctx->pc == UINT32_C(0x80040CB4) || ctx->pc == UINT32_C(0x8003EC60) ||
       ctx->pc == UINT32_C(0x8003ECEC)))
    games::crash3::observeObjectPacketBinding(*ctx);
  const auto readSxy = [&](uint8_t sourceReg, uint32_t value) {
    const uint32_t presented = games::crash3::rendererPresentationSxy(
        ctx->pc, sourceReg, value, static_cast<int32_t>(ctx->cop2c[GTE_OFX]));
    if (!pgxpTrackingEnabled())
      return presented;
    // pgxp_mfc2/pgxp_swc2 validate the side channel against the returned
    // packed word immediately after this call. Retag the integer witness even
    // for Crash's branch-local guard value: the parallel X/Y and Q remain the
    // exact final projection, so a value propagated through a packet keeps
    // its real Vulkan position instead of becoming partially precise. A later
    // exact FIFO read simply replaces this temporary integer witness again.
    PGXPValue &precision = ctx->pgxpGteData[sourceReg];
    precision.value = presented;
    games::crash3::observeProjectedBillboardCentre(*ctx, sourceReg, presented,
                                                   precision);
    return presented;
  };
  switch (reg) {
  case GTE_SXYP:
    // SXYP is a read mirror of the newest screen coordinate (SXY2).
    return readSxy(GTE_SXY2, ctx->cop2d[GTE_SXY2]);
  case GTE_SXY0:
  case GTE_SXY1:
  case GTE_SXY2:
    return readSxy(reg, ctx->cop2d[reg]);
  case GTE_IRGB:
  case GTE_ORGB:
    // Convert signed IR values to RGB555. Negative and overflowing values
    // clamp; masking would wrap them to unrelated colours.
    return static_cast<uint32_t>(std::clamp(
               static_cast<int32_t>(ctx->cop2d[GTE_IR1]) / 0x80, 0, 0x1F)) |
           (static_cast<uint32_t>(std::clamp(
                static_cast<int32_t>(ctx->cop2d[GTE_IR2]) / 0x80, 0, 0x1F))
            << 5) |
           (static_cast<uint32_t>(std::clamp(
                static_cast<int32_t>(ctx->cop2d[GTE_IR3]) / 0x80, 0, 0x1F))
            << 10);
  case GTE_LZCS:
    return ctx->cop2d[GTE_LZCS];
  case GTE_LZCR: {
    int32_t val = static_cast<int32_t>(ctx->cop2d[GTE_LZCS]);
    if (val >= 0)
      return countLeadingZeros(val);
    return countLeadingZeros(~static_cast<uint32_t>(val));
  }
  default:
    return ctx->cop2d[reg];
  }
}

void GTE::writeData(CPUContext *ctx, uint8_t reg, uint32_t val) {
  reg &= 0x1F;
  if (pgxpTrackingEnabled()) {
    if (reg == GTE_SXYP) {
      ctx->pgxpGteData[GTE_SXY0] = ctx->pgxpGteData[GTE_SXY1];
      ctx->pgxpGteData[GTE_SXY1] = ctx->pgxpGteData[GTE_SXY2];
      ctx->pgxpGteData[GTE_SXY2].invalidate();
      ctx->gteDataSourceToken[GTE_SXY0] = ctx->gteDataSourceToken[GTE_SXY1];
      ctx->gteDataSourceToken[GTE_SXY1] = ctx->gteDataSourceToken[GTE_SXY2];
      ctx->gteDataSourceToken[GTE_SXY2] = 0;
    } else {
      ctx->pgxpGteData[reg].invalidate();
      ctx->gteDataSourceToken[reg] = 0;
    }
  }
  switch (reg) {
  case GTE_VZ0:
  case GTE_VZ1:
  case GTE_VZ2:
  case GTE_IR0:
  case GTE_IR1:
  case GTE_IR2:
  case GTE_IR3:
    ctx->cop2d[reg] =
        static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(val)));
    break;
  case GTE_OTZ:
  case GTE_SZ0:
  case GTE_SZ1:
  case GTE_SZ2:
  case GTE_SZ3:
    ctx->cop2d[reg] = static_cast<uint16_t>(val);
    break;
  case GTE_IRGB:
    ctx->cop2d[GTE_IRGB] = val & 0x7FFF;
    ctx->cop2d[GTE_IR1] = static_cast<uint32_t>(
        static_cast<int32_t>(static_cast<int16_t>((val & 0x1F) * 0x80)));
    ctx->cop2d[GTE_IR2] = static_cast<uint32_t>(
        static_cast<int32_t>(static_cast<int16_t>(((val >> 5) & 0x1F) * 0x80)));
    ctx->cop2d[GTE_IR3] = static_cast<uint32_t>(static_cast<int32_t>(
        static_cast<int16_t>(((val >> 10) & 0x1F) * 0x80)));
    break;
  case GTE_ORGB:
  case GTE_LZCR:
    break; // read-only registers
  case GTE_SXYP:
    ctx->cop2d[GTE_SXY0] = ctx->cop2d[GTE_SXY1];
    ctx->cop2d[GTE_SXY1] = ctx->cop2d[GTE_SXY2];
    ctx->cop2d[GTE_SXY2] = val;
    break;
  case GTE_LZCS:
    ctx->cop2d[GTE_LZCS] = val;
    ctx->cop2d[GTE_LZCR] = (static_cast<int32_t>(val) >= 0)
                               ? countLeadingZeros(val)
                               : countLeadingZeros(~val);
    break;
  default:
    ctx->cop2d[reg] = val;
    break;
  }
}

void GTE::noteDataSource(CPUContext *ctx, uint8_t reg, uint32_t sourceAddress) {
  if (!pgxpTrackingEnabled())
    return;
  reg &= 0x1F;
  if (reg == GTE_SXYP)
    reg = GTE_SXY2;
  ctx->gteDataSourceToken[reg] = sourceAddressToken(sourceAddress);
}

uint32_t GTE::readControl(CPUContext *ctx, uint8_t reg) {
  return ctx->cop2c[reg & 0x1F];
}

namespace {

void writeControlWithOrigin(CPUContext *ctx, uint8_t reg, uint32_t val,
                             uint32_t sourceAddress, uint32_t writer,
                             bool guestInstruction) {
  reg &= 0x1F;
  if (pgxpTrackingEnabled()) {
    if (guestInstruction)
      games::crash3::observeGteControlWrite(*ctx, reg, val);
    ctx->gteControlSourceToken[reg] =
        sourceAddress == UINT32_MAX ? 0 : sourceAddressToken(sourceAddress);
    ctx->gteControlWriterPc[reg] = writer;
    ++ctx->gteControlWriteSerial;
    if (ctx->gteControlWriteSerial == 0)
      ++ctx->gteControlWriteSerial;
    ctx->gteControlWriteGeneration[reg] = ctx->gteControlWriteSerial;
  }
  switch (reg) {
  case GTE_RT33:
  case GTE_L33:
  case GTE_LB3:
  case GTE_H:
  case GTE_DQA:
  case GTE_ZSF3:
  case GTE_ZSF4:
    ctx->cop2c[reg] =
        static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(val)));
    break;
  case GTE_FLAG: {
    uint32_t flag = val & 0x7FFFF000;
    if (flag & 0x7F87E000)
      flag |= gte_flag::ERROR_FLAG;
    ctx->cop2c[GTE_FLAG] = flag;
    break;
  }
  default:
    ctx->cop2c[reg] = val;
    break;
  }
}

} // namespace

void GTE::writeControl(CPUContext *ctx, uint8_t reg, uint32_t val,
                       uint32_t sourceAddress) {
  writeControlWithOrigin(ctx, reg, val, sourceAddress, ctx->pc, true);
}

void writeGteControlFromHost(CPUContext *ctx, uint8_t reg, uint32_t value,
                             HostGteControlWriter writer,
                             uint32_t sourceAddress) {
  writeControlWithOrigin(ctx, reg, value, sourceAddress,
                          static_cast<uint32_t>(writer), false);
}

// Internal Helpers

void GTE::getVector(CPUContext *ctx, uint8_t v, int16_t &vx, int16_t &vy,
                    int16_t &vz) {
  switch (v) {
  case 0:
    vx = lo16(ctx->cop2d[GTE_VXY0]);
    vy = hi16(ctx->cop2d[GTE_VXY0]);
    vz = lo16(ctx->cop2d[GTE_VZ0]);
    break;
  case 1:
    vx = lo16(ctx->cop2d[GTE_VXY1]);
    vy = hi16(ctx->cop2d[GTE_VXY1]);
    vz = lo16(ctx->cop2d[GTE_VZ1]);
    break;
  case 2:
    vx = lo16(ctx->cop2d[GTE_VXY2]);
    vy = hi16(ctx->cop2d[GTE_VXY2]);
    vz = lo16(ctx->cop2d[GTE_VZ2]);
    break;
  case 3:
    vx = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
    vy = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
    vz = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
    break;
  }
}

void GTE::getMatrix(CPUContext *ctx, uint8_t m, int16_t mat[3][3]) {
  uint8_t base = m * 8; // RT=0, L=8, LC=16
  if (m >= 3) {
    std::memset(mat, 0, sizeof(int16_t) * 9);
    return;
  }
  mat[0][0] = lo16(ctx->cop2c[base + 0]);
  mat[0][1] = hi16(ctx->cop2c[base + 0]);
  mat[0][2] = lo16(ctx->cop2c[base + 1]);
  mat[1][0] = hi16(ctx->cop2c[base + 1]);
  mat[1][1] = lo16(ctx->cop2c[base + 2]);
  mat[1][2] = hi16(ctx->cop2c[base + 2]);
  mat[2][0] = lo16(ctx->cop2c[base + 3]);
  mat[2][1] = hi16(ctx->cop2c[base + 3]);
  mat[2][2] = lo16(ctx->cop2c[base + 4]);
}

void GTE::getTranslation(CPUContext *ctx, uint8_t tv, int32_t tr[3]) {
  switch (tv) {
  case 0:
    tr[0] = static_cast<int32_t>(ctx->cop2c[GTE_TRX]);
    tr[1] = static_cast<int32_t>(ctx->cop2c[GTE_TRY]);
    tr[2] = static_cast<int32_t>(ctx->cop2c[GTE_TRZ]);
    break;
  case 1:
    tr[0] = static_cast<int32_t>(ctx->cop2c[GTE_RBK]);
    tr[1] = static_cast<int32_t>(ctx->cop2c[GTE_GBK]);
    tr[2] = static_cast<int32_t>(ctx->cop2c[GTE_BBK]);
    break;
  case 2:
    tr[0] = static_cast<int32_t>(ctx->cop2c[GTE_RFC]);
    tr[1] = static_cast<int32_t>(ctx->cop2c[GTE_GFC]);
    tr[2] = static_cast<int32_t>(ctx->cop2c[GTE_BFC]);
    break;
  case 3:
    tr[0] = 0;
    tr[1] = 0;
    tr[2] = 0;
    break;
  }
}

void GTE::checkMAC(CPUContext *ctx, int64_t val, uint8_t macIdx) {
  const int64_t limit = INT64_C(1) << (macIdx == 0 ? 31 : 43);
  if (val > limit - 1) {
    if (macIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC0_OF;
    else if (macIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC1_OF;
    else if (macIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC2_OF;
    else if (macIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC3_OF;
  }
  if (val < -limit) {
    if (macIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC0_UF;
    else if (macIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC1_UF;
    else if (macIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC2_UF;
    else if (macIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::MAC3_UF;
  }
}

int32_t GTE::clampIR(CPUContext *ctx, int32_t val, uint8_t irIdx, bool lm) {
  int32_t lo = lm ? 0 : -0x8000;
  int32_t hi = 0x7FFF;
  if (irIdx == 0) {
    lo = 0;
    hi = 0x1000;
  }

  if (val < lo) {
    if (irIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR0_SATURATED;
    else if (irIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR1_SATURATED;
    else if (irIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR2_SATURATED;
    else if (irIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR3_SATURATED;
    return lo;
  }
  if (val > hi) {
    if (irIdx == 0)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR0_SATURATED;
    else if (irIdx == 1)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR1_SATURATED;
    else if (irIdx == 2)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR2_SATURATED;
    else if (irIdx == 3)
      ctx->cop2c[GTE_FLAG] |= gte_flag::IR3_SATURATED;
    return hi;
  }
  return val;
}

void GTE::setMAC(CPUContext *ctx, int64_t mac1, int64_t mac2, int64_t mac3,
                 bool sf) {
  int shift = sf ? 12 : 0;
  checkMAC(ctx, mac1, 1);
  checkMAC(ctx, mac2, 2);
  checkMAC(ctx, mac3, 3);
  ctx->cop2d[GTE_MAC1] =
      static_cast<uint32_t>(static_cast<int32_t>(mac1 >> shift));
  ctx->cop2d[GTE_MAC2] =
      static_cast<uint32_t>(static_cast<int32_t>(mac2 >> shift));
  ctx->cop2d[GTE_MAC3] =
      static_cast<uint32_t>(static_cast<int32_t>(mac3 >> shift));
}

void GTE::setIR(CPUContext *ctx, int32_t ir1, int32_t ir2, int32_t ir3,
                bool lm) {
  ctx->cop2d[GTE_IR1] = static_cast<uint32_t>(clampIR(ctx, ir1, 1, lm));
  ctx->cop2d[GTE_IR2] = static_cast<uint32_t>(clampIR(ctx, ir2, 2, lm));
  ctx->cop2d[GTE_IR3] = static_cast<uint32_t>(clampIR(ctx, ir3, 3, lm));
}

void GTE::setMAC0(CPUContext *ctx, int64_t val) {
  checkMAC(ctx, val, 0);
  ctx->cop2d[GTE_MAC0] = static_cast<uint32_t>(static_cast<int32_t>(val));
}

void GTE::setIR0(CPUContext *ctx, int32_t val, bool /*lm*/) {
  ctx->cop2d[GTE_IR0] = static_cast<uint32_t>(clampIR(ctx, val, 0, true));
}

void GTE::pushSXY(CPUContext *ctx, int32_t sx, int32_t sy) {
  ctx->cop2d[GTE_SXY0] = ctx->cop2d[GTE_SXY1];
  ctx->cop2d[GTE_SXY1] = ctx->cop2d[GTE_SXY2];
  if (pgxpTrackingEnabled()) {
    ctx->pgxpGteData[GTE_SXY0] = ctx->pgxpGteData[GTE_SXY1];
    ctx->pgxpGteData[GTE_SXY1] = ctx->pgxpGteData[GTE_SXY2];
    ctx->pgxpGteData[GTE_SXY2].invalidate();
  }
  int32_t csx = sx, csy = sy;
  if (sx < -0x400) {
    csx = -0x400;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SX2_SATURATED;
  }
  if (sx > 0x3FF) {
    csx = 0x3FF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SX2_SATURATED;
  }
  if (sy < -0x400) {
    csy = -0x400;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SY2_SATURATED;
  }
  if (sy > 0x3FF) {
    csy = 0x3FF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SY2_SATURATED;
  }
  ctx->cop2d[GTE_SXY2] =
      static_cast<uint32_t>((static_cast<uint16_t>(csx) & 0xFFFF) |
                            (static_cast<uint16_t>(csy) << 16));
}

void GTE::pushSZ(CPUContext *ctx, int32_t sz) {
  ctx->cop2d[GTE_SZ0] = ctx->cop2d[GTE_SZ1];
  ctx->cop2d[GTE_SZ1] = ctx->cop2d[GTE_SZ2];
  ctx->cop2d[GTE_SZ2] = ctx->cop2d[GTE_SZ3];
  if (sz < 0) {
    sz = 0;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  if (sz > 0xFFFF) {
    sz = 0xFFFF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  ctx->cop2d[GTE_SZ3] = static_cast<uint32_t>(sz);
}

void GTE::pushRGB(CPUContext *ctx, uint8_t r, uint8_t g, uint8_t b,
                  uint8_t cd) {
  ctx->cop2d[GTE_RGB0] = ctx->cop2d[GTE_RGB1];
  ctx->cop2d[GTE_RGB1] = ctx->cop2d[GTE_RGB2];
  ctx->cop2d[GTE_RGB2] = r | (static_cast<uint32_t>(g) << 8) |
                         (static_cast<uint32_t>(b) << 16) |
                         (static_cast<uint32_t>(cd) << 24);
}

uint32_t GTE::divide(CPUContext *ctx, uint16_t h, uint16_t sz3) {
  // The hardware saturates when H/SZ3 is at least 2.0, including equality.
  if (sz3 == 0 || static_cast<uint32_t>(h) >= static_cast<uint32_t>(sz3) * 2u) {
    ctx->cop2c[GTE_FLAG] |= gte_flag::DIV_OVERFLOW;
    return 0x1FFFF;
  }

  // Normalize SZ3, look up an 8-bit reciprocal seed, then perform the two
  // fixed-point refinement steps used by the GTE.  The previous code stopped
  // after multiplying by the seed, producing quotients hundreds of times too
  // small and collapsing projected geometry around OFX/OFY.
  return gte_math::divide(h, sz3);
}

// MVMVA core

void GTE::doMVMVA(CPUContext *ctx, uint8_t mx, uint8_t mv, uint8_t tv, bool sf,
                  bool lm) {
  int16_t mat[3][3];
  int16_t vx, vy, vz;
  int32_t tr[3];
  getMatrix(ctx, mx, mat);
  getVector(ctx, mv, vx, vy, vz);
  getTranslation(ctx, tv, tr);

  std::array<int64_t, 3> mac{};
  for (uint8_t row = 0; row < 3; ++row) {
    const auto stage = [&](int64_t value) {
      checkMAC(ctx, value, row + 1);
      // Hardware sign-extends the 44-bit accumulator between the first two
      // additions. The final MAC register truncates only after sf shifting.
      return std::bit_cast<int64_t>(static_cast<uint64_t>(value) << 20) >> 20;
    };
    mac[row] =
        stage(stage(int64_t{tr[row]} * 4096 + int64_t{mat[row][0]} * vx) +
              int64_t{mat[row][1]} * vy) +
        int64_t{mat[row][2]} * vz;
  }
  setMAC(ctx, mac[0], mac[1], mac[2], sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
}

// RTPS core (single vertex)

void GTE::doRTPS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm,
                 bool lastVertex) {
  // Keep the unshifted 4.12 transform for PGXP before the architectural GTE
  // path truncates it into MAC/IR.  Reconstructing this from MAC below would
  // retain the true floating-point divide but still quantize camera-space X,
  // Y and Z to integers, which is precisely the sub-pixel jitter PGXP is
  // intended to remove.
  const bool trackPrecision = pgxpTrackingEnabled();
  int16_t rotation[3][3]{};
  int16_t vx = 0, vy = 0, vz = 0;
  int32_t translation[3]{};
  int64_t rawViewX = 0, rawViewY = 0, rawViewZ = 0;
  if (trackPrecision) {
    getMatrix(ctx, 0, rotation);
    getVector(ctx, vecIdx, vx, vy, vz);
    getTranslation(ctx, 0, translation);
    rawViewX = static_cast<int64_t>(translation[0]) * 4096 +
               static_cast<int64_t>(rotation[0][0]) * vx +
               static_cast<int64_t>(rotation[0][1]) * vy +
               static_cast<int64_t>(rotation[0][2]) * vz;
    rawViewY = static_cast<int64_t>(translation[1]) * 4096 +
               static_cast<int64_t>(rotation[1][0]) * vx +
               static_cast<int64_t>(rotation[1][1]) * vy +
               static_cast<int64_t>(rotation[1][2]) * vz;
    rawViewZ = static_cast<int64_t>(translation[2]) * 4096 +
               static_cast<int64_t>(rotation[2][0]) * vx +
               static_cast<int64_t>(rotation[2][1]) * vy +
               static_cast<int64_t>(rotation[2][2]) * vz;
  }

  // Rotation x Vector + Translation
  doMVMVA(ctx, 0, vecIdx, 0, sf, lm);

  // Push SZ FIFO
  pushSZ(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> (sf ? 0 : 12));

  // Perspective division: n = H * 0x20000 / SZ3
  uint32_t quotient = divide(ctx, static_cast<uint16_t>(ctx->cop2c[GTE_H]),
                             static_cast<uint16_t>(ctx->cop2d[GTE_SZ3]));

  // Architectural screen X/Y must remain bit-identical to the original GTE.
  // In particular, Crash consumes the SXY FIFO through NCLIP and additional
  // renderer visibility tests. Widening these integers changed guest control
  // flow and created wrapped/collapsed edge polygons. The parallel PGXP value
  // below owns the Crash-specific 16:9 presentation transform instead.
  int64_t sx =
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC1])) *
          quotient +
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_OFX]));
  int64_t sy =
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC2])) *
          quotient +
      static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_OFY]));
  pushSXY(ctx, static_cast<int32_t>(sx >> 16), static_cast<int32_t>(sy >> 16));

  if (trackPrecision) {
    // Parallel floating-point projection for PGXP. Architectural registers and
    // flags above remain bit-exact; this value only follows the packed SXY word
    // as provenance metadata. X/Y use the command's sf shift while screen depth
    // always comes from the transform's 4.12 Z, matching the GTE SZ FIFO and
    // the preserve-projection-precision path used by SwanStation.
    const double xyScale = sf ? 4096.0 : 1.0;
    const double viewX = static_cast<double>(rawViewX) / xyScale;
    const double viewY = static_cast<double>(rawViewY) / xyScale;
    const double viewZ = static_cast<double>(rawViewZ) / 4096.0;
    const double projectionPlane =
        static_cast<double>(static_cast<uint16_t>(ctx->cop2c[GTE_H]));
    const double horizontalProjectionPlane =
        projectionPlane * games::crash3::horizontalProjectionScale();
    const double screenOriginX =
        static_cast<double>(static_cast<int32_t>(ctx->cop2c[GTE_OFX])) /
        65536.0;
    const double screenOriginY =
        static_cast<double>(static_cast<int32_t>(ctx->cop2c[GTE_OFY])) /
        65536.0;
    PGXPValue &precise = ctx->pgxpGteData[GTE_SXY2];
    precise.value = ctx->cop2d[GTE_SXY2];
    precise.origin = PGXPOrigin::GteProjection;
    precise.z = viewZ;
    precise.qx = screenOriginX * viewZ + viewX * horizontalProjectionPlane;
    precise.qy = screenOriginY * viewZ + viewY * projectionPlane;
    precise.nearDepth = projectionPlane * 0.5 + 1.0 / 4096.0;
    precise.projectionId = allocateProjectionId();
    precise.projectionContext = hashProjectionContext(ctx, sf);
    precise.projectionTransformValueFingerprint =
        hashProjectionTransform(rotation, translation);
    const bool completeControlLineage = projectionTransformLineageComplete(ctx);
    const int64_t modelVertexSource = coherentModelVertexSource(ctx, vecIdx);
    precise.projectionTransformLineage = projectionTransformLineage(ctx);
    mips::noteConstructionLineageProjectionTransform(
        precise.projectionTransformLineage);
    precise.modelVertexSource =
        modelVertexSource >= 0
            ? sourceAddressToken(static_cast<uint32_t>(modelVertexSource))
            : 0;
    games::crash3::attachProjectionTag(*ctx, vecIdx, precise);
    const bool fullValueFlowExpected = mips::valueFlowEnabled();
    const bool emitValueFlowTrace = gteTraceEnabledFast();
    const mips::GteProjectionValueFlow valueFlow =
        fullValueFlowExpected ? mips::captureGteProjectionValueFlow(
                                    ctx, vecIdx, emitValueFlowTrace)
                              : mips::GteProjectionValueFlow{};
    const mips::GteProjectionConstructionLineage constructionLineage =
        mips::captureGteProjectionConstructionLineage(ctx, vecIdx);
    const mips::InterpreterSliceIdentity interpreterSlice =
        mips::currentInterpreterSliceIdentity();
    const bool constructionMatchesFull =
        !fullValueFlowExpected ||
        (valueFlow.modelFingerprintComplete && constructionLineage.complete &&
         valueFlow.modelSourceFingerprint ==
             constructionLineage.modelSourceFingerprint &&
         valueFlow.modelProgramFingerprint ==
             constructionLineage.modelProgramFingerprint);
    const bool modelFingerprintComplete =
        constructionLineage.complete && constructionMatchesFull;
    if (emitValueFlowTrace) {
      metrics::noteGteProjectionForTrace(precise.projectionId);
      const std::optional<uint64_t> gameProfileTag =
          games::crash3::decodeProjectionTag(precise.modelVertexSource);
      const std::optional<uint64_t> proceduralIdentity =
          gameProfileTag.has_value()
              ? games::crash3::proceduralProjectionIdentity(
                    precise.gameProfileCallsite, *gameProfileTag)
              : std::nullopt;
      const uint32_t objectSource =
          games::crash3::projectionCallsiteObjectSource(
              precise.gameProfileCallsite);
      const std::optional<uint32_t> objectCommand =
          objectSource != 0u
              ? games::crash3::objectProjectionCommand(precise.projectionId)
              : std::nullopt;
      const std::optional<uint32_t> logicalObject =
          objectSource != 0u ? games::crash3::objectProjectionLogicalObject(
                                   precise.projectionId)
                             : std::nullopt;
      uint64_t firstGeneration = std::numeric_limits<uint64_t>::max();
      uint64_t lastGeneration = 0;
      for (uint8_t reg = GTE_RT11RT12; reg <= GTE_TRZ; ++reg) {
        const uint64_t generation = ctx->gteControlWriteGeneration[reg];
        firstGeneration = std::min(firstGeneration, generation);
        lastGeneration = std::max(lastGeneration, generation);
      }
      if (!completeControlLineage)
        firstGeneration = 0;
      metrics::trace(
          "gte.projection",
          {{"pc", ctx->pc},
           {"projection_id", static_cast<int64_t>(precise.projectionId)},
           {"projection_context",
            static_cast<int64_t>(precise.projectionContext)},
           {"game_profile_callsite",
            static_cast<int64_t>(precise.gameProfileCallsite)},
           {"game_profile_tag", gameProfileTag.has_value()
                                    ? static_cast<int64_t>(*gameProfileTag)
                                    : 0},
           {"procedural_identity",
            proceduralIdentity.has_value()
                ? static_cast<int64_t>(*proceduralIdentity)
                : 0},
           {"object_source", objectSource},
           {"logical_object", logicalObject.value_or(0u)},
           {"object_command", objectCommand.value_or(0u)},
           {"vector_index", vecIdx},
           {"sf", sf ? 1 : 0},
           {"vx", vx},
           {"vy", vy},
           {"vz", vz},
           {"model_source", modelVertexSource},
           {"model_source_carrier",
            static_cast<int64_t>(precise.modelVertexSource)},
           {"transform_source", coherentTransformSource(ctx)},
           {"transform_hash",
            static_cast<int64_t>(precise.projectionTransformValueFingerprint)},
           {"transform_lineage",
            static_cast<int64_t>(projectionTransformLineage(ctx))},
           {"lineage_complete", completeControlLineage ? 1 : 0},
           {"value_flow_complete", valueFlow.complete ? 1 : 0},
           {"model_fingerprint_complete", modelFingerprintComplete ? 1 : 0},
           {"model_fingerprint_schema",
            mips::MIPS_MODEL_FINGERPRINT_SCHEMA_VERSION},
           {"model_source_fingerprint",
            static_cast<int64_t>(constructionLineage.modelSourceFingerprint)},
           {"model_program_fingerprint",
            static_cast<int64_t>(constructionLineage.modelProgramFingerprint)},
           {"interpreter_slice_active", interpreterSlice.active ? 1 : 0},
           {"interpreter_slice_dynamic", interpreterSlice.dynamic ? 1 : 0},
           {"interpreter_slice_entry", interpreterSlice.entry},
           {"interpreter_slice_owner_start", interpreterSlice.ownerStart},
           {"interpreter_slice_owner_end", interpreterSlice.ownerEnd},
           {"model_compact_complete", constructionLineage.complete ? 1 : 0},
           {"model_compact_matches_full", constructionMatchesFull ? 1 : 0},
           {"vxy_expr", static_cast<int64_t>(valueFlow.modelXy)},
           {"vz_expr", static_cast<int64_t>(valueFlow.modelZ)},
           {"rt0_expr", static_cast<int64_t>(valueFlow.transform[0])},
           {"rt1_expr", static_cast<int64_t>(valueFlow.transform[1])},
           {"rt2_expr", static_cast<int64_t>(valueFlow.transform[2])},
           {"rt3_expr", static_cast<int64_t>(valueFlow.transform[3])},
           {"rt4_expr", static_cast<int64_t>(valueFlow.transform[4])},
           {"tr0_expr", static_cast<int64_t>(valueFlow.transform[5])},
           {"tr1_expr", static_cast<int64_t>(valueFlow.transform[6])},
           {"tr2_expr", static_cast<int64_t>(valueFlow.transform[7])},
           {"generation_first", static_cast<int64_t>(firstGeneration)},
           {"generation_last", static_cast<int64_t>(lastGeneration)},
           {"r00", rotation[0][0]},
           {"r01", rotation[0][1]},
           {"r02", rotation[0][2]},
           {"r10", rotation[1][0]},
           {"r11", rotation[1][1]},
           {"r12", rotation[1][2]},
           {"r20", rotation[2][0]},
           {"r21", rotation[2][1]},
           {"r22", rotation[2][2]},
           {"tx", translation[0]},
           {"ty", translation[1]},
           {"tz", translation[2]},
           {"raw_view_x", rawViewX},
           {"raw_view_y", rawViewY},
           {"raw_view_z", rawViewZ},
           {"rt0_writer_pc", ctx->gteControlWriterPc[0]},
           {"rt1_writer_pc", ctx->gteControlWriterPc[1]},
           {"rt2_writer_pc", ctx->gteControlWriterPc[2]},
           {"rt3_writer_pc", ctx->gteControlWriterPc[3]},
           {"rt4_writer_pc", ctx->gteControlWriterPc[4]},
           {"tr0_writer_pc", ctx->gteControlWriterPc[5]},
           {"tr1_writer_pc", ctx->gteControlWriterPc[6]},
           {"tr2_writer_pc", ctx->gteControlWriterPc[7]},
           {"rt0_source", traceSourceAddress(ctx->gteControlSourceToken[0])},
           {"rt1_source", traceSourceAddress(ctx->gteControlSourceToken[1])},
           {"rt2_source", traceSourceAddress(ctx->gteControlSourceToken[2])},
           {"rt3_source", traceSourceAddress(ctx->gteControlSourceToken[3])},
           {"rt4_source", traceSourceAddress(ctx->gteControlSourceToken[4])},
           {"tr0_source", traceSourceAddress(ctx->gteControlSourceToken[5])},
           {"tr1_source", traceSourceAddress(ctx->gteControlSourceToken[6])},
           {"tr2_source", traceSourceAddress(ctx->gteControlSourceToken[7])}});
    }
    precise.modelValueFlowSourceFingerprint =
        modelFingerprintComplete ? constructionLineage.modelSourceFingerprint
                                 : 0;
    precise.modelValueFlowProgramFingerprint =
        modelFingerprintComplete ? constructionLineage.modelProgramFingerprint
                                 : 0;
    const bool signedProjectionFinite =
        std::isfinite(precise.qx) && std::isfinite(precise.qy) &&
        std::isfinite(precise.z) && std::isfinite(precise.nearDepth);
    const uint8_t signedProjectionFlags =
        signedProjectionFinite ? PGXPValue::VALID_SIGNED_PROJECTION : 0;
    const uint8_t intrinsicProvenanceFlags =
        signedProjectionFinite
            ? static_cast<uint8_t>(
                  (completeControlLineage ? PGXPValue::VALID_TRANSFORM_LINEAGE
                                          : 0) |
                  (modelVertexSource >= 0 ? PGXPValue::VALID_MODEL_VERTEX_SOURCE
                                          : 0) |
                  (modelFingerprintComplete ? PGXPValue::VALID_MODEL_VALUE_FLOW
                                            : 0))
            : 0;
    if (signedProjectionFinite) {
      constexpr double maximumProjectionRatio =
          static_cast<double>(0x1FFFFu) / 65536.0;
      // The architectural divider saturates at (just below) 2.0 when H is at
      // least twice the depth, including zero/negative camera-space W. Preserve
      // that donor-compatible screen position alongside the signed homogeneous
      // projection: SwanStation and Beetle both retain GTE saturation for
      // original-game visibility rather than forcing geometric clipping.
      // Positive unsaturated vertices still use fractional viewZ, so preserving
      // transform precision does not inherit the SZ3 quantization.
      const double ratio = projectionPlane >= 2.0 * viewZ
                               ? maximumProjectionRatio
                               : projectionPlane / viewZ;
      precise.x = std::clamp(screenOriginX +
                                 viewX * ratio *
                                     games::crash3::horizontalProjectionScale(),
                             -1024.0, 1023.0);
      precise.y = std::clamp(screenOriginY + viewY * ratio, -1024.0, 1023.0);
      precise.flags = PGXPValue::VALID_XY | signedProjectionFlags |
                      intrinsicProvenanceFlags |
                      (viewZ > 0.0 ? PGXPValue::VALID_Z : 0);
      countPGXPStage(viewZ > 0.0 ? PGXPStage::ProjectionXYZ
                                 : PGXPStage::ProjectionSigned);
    } else {
      precise.x = static_cast<float>(static_cast<int16_t>(precise.value));
      precise.y = static_cast<float>(static_cast<int16_t>(precise.value >> 16));
      precise.flags = PGXPValue::VALID_XY | signedProjectionFlags |
                      intrinsicProvenanceFlags;
      countPGXPStage(signedProjectionFinite ? PGXPStage::ProjectionSigned
                                            : PGXPStage::ProjectionXYOnly);
    }
    recordPGXPProjection(precise);
  }

  if (lastVertex) {
    // Depth queuing: MAC0 = DQB + DQA * quotient
    int64_t mac0val =
        static_cast<int64_t>(static_cast<int32_t>(ctx->cop2c[GTE_DQB])) +
        static_cast<int64_t>(static_cast<int16_t>(ctx->cop2c[GTE_DQA])) *
            quotient;
    setMAC0(ctx, mac0val);
    setIR0(ctx, static_cast<int32_t>(mac0val >> 12), lm);
  }
}

// Color clamp helper

static uint8_t clampColor(CPUContext *ctx, int32_t val, uint32_t satFlag) {
  if (val < 0) {
    ctx->cop2c[GTE_FLAG] |= satFlag;
    return 0;
  }
  if (val > 255) {
    ctx->cop2c[GTE_FLAG] |= satFlag;
    return 255;
  }
  return static_cast<uint8_t>(val);
}

namespace {
void interpolateColor(CPUContext *ctx, const std::array<int64_t, 3> &input,
                      bool sf, bool lm) {
  for (uint32_t channel = 0; channel < 3; ++channel) {
    const auto result = gte_math::interpolateColorChannel(
        input[channel], std::bit_cast<int32_t>(ctx->cop2c[GTE_RFC + channel]),
        static_cast<int16_t>(ctx->cop2d[GTE_IR0]), sf, lm);
    ctx->cop2d[GTE_MAC1 + channel] = static_cast<uint32_t>(result.mac);
    ctx->cop2d[GTE_IR1 + channel] = static_cast<uint32_t>(result.ir);
    ctx->cop2c[GTE_FLAG] |= result.flags >> channel;
  }
}

std::array<int64_t, 3> modulatedColor(const CPUContext *ctx) {
  std::array<int64_t, 3> result{};
  for (uint32_t channel = 0; channel < 3; ++channel)
    result[channel] = int64_t{(ctx->cop2d[GTE_RGBC] >> (channel * 8u)) & 255u} *
                      static_cast<int16_t>(ctx->cop2d[GTE_IR1 + channel]) * 16;
  return result;
}

void pushColorFromMac(CPUContext *ctx) {
  uint32_t rgb = ctx->cop2d[GTE_RGBC] & 0xff000000u;
  for (uint32_t channel = 0; channel < 3; ++channel)
    rgb |= uint32_t{clampColor(
               ctx, std::bit_cast<int32_t>(ctx->cop2d[GTE_MAC1 + channel]) >> 4,
               gte_flag::COLOR_R_SAT >> channel)}
           << (channel * 8u);
  ctx->cop2d[GTE_RGB0] = ctx->cop2d[GTE_RGB1];
  ctx->cop2d[GTE_RGB1] = ctx->cop2d[GTE_RGB2];
  ctx->cop2d[GTE_RGB2] = rgb;
}
} // namespace

// NCS core (single vertex)

void GTE::doNCS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm) {
  // Light matrix x normal -> IR
  doMVMVA(ctx, 1, vecIdx, 3, sf, lm); // LxV, no background translation.
  // Light color matrix x IR -> IR
  doMVMVA(ctx, 2, 3, 1, sf, lm); // LCxIR + BK
  // Color FIFO push
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
}

// NCDS core

void GTE::doNCDS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm) {
  doMVMVA(ctx, 1, vecIdx, 3, sf, lm);
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  interpolateColor(ctx, modulatedColor(ctx), sf, lm);
  // NCDS pushes exactly once. Calling NCS here previously pushed an extra
  // colour and skipped the RGBC modulation before distance interpolation.
  pushColorFromMac(ctx);
}

// NCCS core

void GTE::doNCCS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm) {
  // Light matrix x normal -> IR
  doMVMVA(ctx, 1, vecIdx, 3, sf, lm);
  // Color matrix x IR + BK -> IR
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  // Multiply by RGBC
  uint8_t rgbc_r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t rgbc_g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t rgbc_b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  int64_t mac1 = static_cast<int64_t>(rgbc_r) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR1]) * 16;
  int64_t mac2 = static_cast<int64_t>(rgbc_g) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR2]) * 16;
  int64_t mac3 = static_cast<int64_t>(rgbc_b) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR3]) * 16;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
}

// DPCS core

void GTE::doDPCS(CPUContext *ctx, bool sf, bool lm, uint8_t r, uint8_t g,
                 uint8_t b) {
  interpolateColor(ctx,
                   {int64_t{r} * 65536, int64_t{g} * 65536, int64_t{b} * 65536},
                   sf, lm);
  pushColorFromMac(ctx);
}

// PUBLIC GTE COMMANDS (22 total)

void GTE::RTPS(CPUContext *ctx, bool sf, bool lm) {
  games::crash3::prepareSceneryCullGuardForCommand(*ctx);
  GteTraceScope trace(ctx, 0x01, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doRTPS(ctx, 0, sf, lm, true);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::RTPT(CPUContext *ctx, bool sf, bool lm) {
  games::crash3::captureResidentPresentationAtProjection(*ctx);
  games::crash3::observeResidentPresentationProjectionInputs(*ctx);
  GteTraceScope trace(ctx, 0x30, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doRTPS(ctx, 0, sf, lm, false);
  doRTPS(ctx, 1, sf, lm, false);
  doRTPS(ctx, 2, sf, lm, true);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCLIP(CPUContext *ctx, bool /*sf*/, bool /*lm*/) {
  GteTraceScope trace(ctx, 0x06, false, false);
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t sx0 = lo16(ctx->cop2d[GTE_SXY0]), sy0 = hi16(ctx->cop2d[GTE_SXY0]);
  int16_t sx1 = lo16(ctx->cop2d[GTE_SXY1]), sy1 = hi16(ctx->cop2d[GTE_SXY1]);
  int16_t sx2 = lo16(ctx->cop2d[GTE_SXY2]), sy2 = hi16(ctx->cop2d[GTE_SXY2]);
  int64_t mac0 = static_cast<int64_t>(sx0) * (sy1 - sy2) +
                 static_cast<int64_t>(sx1) * (sy2 - sy0) +
                 static_cast<int64_t>(sx2) * (sy0 - sy1);
  setMAC0(ctx, mac0);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::OP(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x0C, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t r11 = lo16(ctx->cop2c[GTE_RT11RT12]);
  int16_t r22 = lo16(ctx->cop2c[GTE_RT22RT23]);
  int16_t r33 = lo16(ctx->cop2c[GTE_RT33]);
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 =
      static_cast<int64_t>(r22) * ir3 - static_cast<int64_t>(r33) * ir2;
  int64_t mac2 =
      static_cast<int64_t>(r33) * ir1 - static_cast<int64_t>(r11) * ir3;
  int64_t mac3 =
      static_cast<int64_t>(r11) * ir2 - static_cast<int64_t>(r22) * ir1;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::DPCS(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x10, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  uint8_t r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  doDPCS(ctx, sf, lm, r, g, b);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::DPCT(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x2A, sf, lm);
  games::crash3::observeObjectDistanceCue(*ctx);
  ctx->cop2c[GTE_FLAG] = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t r = ctx->cop2d[GTE_RGB0] & 0xFF;
    uint8_t g = (ctx->cop2d[GTE_RGB0] >> 8) & 0xFF;
    uint8_t b = (ctx->cop2d[GTE_RGB0] >> 16) & 0xFF;
    doDPCS(ctx, sf, lm, r, g, b);
  }
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::INTPL(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x11, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  std::array<int64_t, 3> input{};
  for (uint32_t channel = 0; channel < 3; ++channel)
    input[channel] =
        int64_t{static_cast<int16_t>(ctx->cop2d[GTE_IR1 + channel])} * 4096;
  interpolateColor(ctx, input, sf, lm);
  pushColorFromMac(ctx);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::MVMVA(CPUContext *ctx, uint8_t mx, uint8_t mv, uint8_t tv, bool sf,
                bool lm) {
  GteTraceScope trace(ctx, 0x12, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doMVMVA(ctx, mx, mv, tv, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCDS(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x13, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doNCDS(ctx, 0, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCDT(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x16, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doNCDS(ctx, 0, sf, lm);
  doNCDS(ctx, 1, sf, lm);
  doNCDS(ctx, 2, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::CDP(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x14, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  interpolateColor(ctx, modulatedColor(ctx), sf, lm);
  pushColorFromMac(ctx);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCCS(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x1B, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doNCCS(ctx, 0, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCCT(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x3F, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doNCCS(ctx, 0, sf, lm);
  doNCCS(ctx, 1, sf, lm);
  doNCCS(ctx, 2, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::CC(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x1C, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  // Light color matrix x IR + BK
  doMVMVA(ctx, 2, 3, 1, sf, lm);
  // Multiply by RGBC
  uint8_t rgbc_r = ctx->cop2d[GTE_RGBC] & 0xFF;
  uint8_t rgbc_g = (ctx->cop2d[GTE_RGBC] >> 8) & 0xFF;
  uint8_t rgbc_b = (ctx->cop2d[GTE_RGBC] >> 16) & 0xFF;
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  int64_t mac1 = static_cast<int64_t>(rgbc_r) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR1]) * 16;
  int64_t mac2 = static_cast<int64_t>(rgbc_g) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR2]) * 16;
  int64_t mac3 = static_cast<int64_t>(rgbc_b) *
                 static_cast<int16_t>(ctx->cop2d[GTE_IR3]) * 16;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCS(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x1E, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doNCS(ctx, 0, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::NCT(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x20, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  doNCS(ctx, 0, sf, lm);
  doNCS(ctx, 1, sf, lm);
  doNCS(ctx, 2, sf, lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::SQR(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x28, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 = static_cast<int64_t>(ir1) * ir1;
  int64_t mac2 = static_cast<int64_t>(ir2) * ir2;
  int64_t mac3 = static_cast<int64_t>(ir3) * ir3;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::DCPL(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x29, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  interpolateColor(ctx, modulatedColor(ctx), sf, lm);
  pushColorFromMac(ctx);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::AVSZ3(CPUContext *ctx, bool sf, bool lm) {
  games::crash3::prepareSceneryCullGuardForCommand(*ctx);
  GteTraceScope trace(ctx, 0x2D, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t zsf3 = static_cast<int16_t>(ctx->cop2c[GTE_ZSF3]);
  int64_t mac0 =
      static_cast<int64_t>(zsf3) *
      (ctx->cop2d[GTE_SZ1] + ctx->cop2d[GTE_SZ2] + ctx->cop2d[GTE_SZ3]);
  setMAC0(ctx, mac0);
  int32_t otz = static_cast<int32_t>(mac0 >> 12);
  if (otz < 0) {
    otz = 0;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  if (otz > 0xFFFF) {
    otz = 0xFFFF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  ctx->cop2d[GTE_OTZ] = static_cast<uint32_t>(otz);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
  games::crash3::observeSceneryOrderingTableAverage(
      *ctx, static_cast<uint32_t>(ctx->cop2d[GTE_MAC0]) >> 15u, 3u);
}

void GTE::AVSZ4(CPUContext *ctx, bool sf, bool lm) {
  games::crash3::prepareSceneryCullGuardForCommand(*ctx);
  GteTraceScope trace(ctx, 0x2E, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t zsf4 = static_cast<int16_t>(ctx->cop2c[GTE_ZSF4]);
  int64_t mac0 =
      static_cast<int64_t>(zsf4) * (ctx->cop2d[GTE_SZ0] + ctx->cop2d[GTE_SZ1] +
                                    ctx->cop2d[GTE_SZ2] + ctx->cop2d[GTE_SZ3]);
  setMAC0(ctx, mac0);
  int32_t otz = static_cast<int32_t>(mac0 >> 12);
  if (otz < 0) {
    otz = 0;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  if (otz > 0xFFFF) {
    otz = 0xFFFF;
    ctx->cop2c[GTE_FLAG] |= gte_flag::SZ3_OTZ_SAT;
  }
  ctx->cop2d[GTE_OTZ] = static_cast<uint32_t>(otz);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
  games::crash3::observeSceneryOrderingTableAverage(
      *ctx, static_cast<uint32_t>(ctx->cop2d[GTE_MAC0]) >> 15u, 4u);
}

void GTE::GPF(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x3D, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  int16_t ir0 = static_cast<int16_t>(ctx->cop2d[GTE_IR0]);
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 = static_cast<int64_t>(ir0) * ir1;
  int64_t mac2 = static_cast<int64_t>(ir0) * ir2;
  int64_t mac3 = static_cast<int64_t>(ir0) * ir3;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

void GTE::GPL(CPUContext *ctx, bool sf, bool lm) {
  GteTraceScope trace(ctx, 0x3E, sf, lm);
  ctx->cop2c[GTE_FLAG] = 0;
  int shift = sf ? 12 : 0;
  int16_t ir0 = static_cast<int16_t>(ctx->cop2d[GTE_IR0]);
  int16_t ir1 = static_cast<int16_t>(ctx->cop2d[GTE_IR1]);
  int16_t ir2 = static_cast<int16_t>(ctx->cop2d[GTE_IR2]);
  int16_t ir3 = static_cast<int16_t>(ctx->cop2d[GTE_IR3]);
  int64_t mac1 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC1]))
       << shift) +
      static_cast<int64_t>(ir0) * ir1;
  int64_t mac2 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC2]))
       << shift) +
      static_cast<int64_t>(ir0) * ir2;
  int64_t mac3 =
      (static_cast<int64_t>(static_cast<int32_t>(ctx->cop2d[GTE_MAC3]))
       << shift) +
      static_cast<int64_t>(ir0) * ir3;
  setMAC(ctx, mac1, mac2, mac3, sf);
  setIR(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC2]),
        static_cast<int32_t>(ctx->cop2d[GTE_MAC3]), lm);
  uint8_t cd = static_cast<uint8_t>(ctx->cop2d[GTE_RGBC] >> 24);
  uint8_t r = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]) >> 4,
                         gte_flag::COLOR_R_SAT);
  uint8_t g = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]) >> 4,
                         gte_flag::COLOR_G_SAT);
  uint8_t b = clampColor(ctx, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]) >> 4,
                         gte_flag::COLOR_B_SAT);
  pushRGB(ctx, r, g, b, cd);
  ctx->cop2c[GTE_FLAG] |=
      (ctx->cop2c[GTE_FLAG] & 0x7F87E000) ? gte_flag::ERROR_FLAG : 0;
}

} // namespace ps1
