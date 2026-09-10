#pragma once

// ps1Runtime -- Runtime Macros
// These macros are emitted by InstructionEmitter and GteEmitter.
// They bridge recompiled C++ code -> runtime Memory/GTE subsystems.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <runtime/bios_access.h>
#include <runtime/cpu_context.h>
#include <runtime/gte.h>
#include <runtime/memory_access.h>
#include <runtime/metrics.h>
#include <runtime/mips_interpreter.h>

// Lightweight declaration used only by a Crash-owned instruction override.
// Keeping the full profile header out of this bridge means renderer/profile
// iteration does not rebuild the generated multi-megabyte game translation.
namespace ps1::games::crash3 {
uint32_t viewportCullPresentationSxy(uint32_t packedSxy) noexcept;
void observeObjectPrePresentation(recomp_context *ctx) noexcept;
void observeObjectRendererStreams(recomp_context *ctx) noexcept;
void observeSceneryDelta(recomp_context *ctx) noexcept;
int32_t scenerySelectionCount(recomp_context *ctx,
                              uint32_t listAddress) noexcept;
uint32_t scenerySelectionEntry(recomp_context *ctx,
                               uint32_t cursorAddress) noexcept;
} // namespace ps1::games::crash3

// Memory Access Macros
// Used by InstructionEmitter for LB/LH/LW/SB/SH/SW

#define MEM_READ8(ctx, addr) ps1::runtimeMemoryRead8((ctx)->mem, (addr))
#define MEM_READ16(ctx, addr) ps1::runtimeMemoryRead16((ctx)->mem, (addr))
#define MEM_READ32(ctx, addr) ps1::runtimeMemoryRead32((ctx)->mem, (addr))

#define MEM_WRITE8(ctx, addr, val)                                             \
  ps1::runtimeMemoryWrite8((ctx)->mem, (addr), (val))
#define MEM_WRITE16(ctx, addr, val)                                            \
  ps1::runtimeMemoryWrite16((ctx)->mem, (addr), (val))
#define MEM_WRITE32(ctx, addr, val)                                            \
  ps1::runtimeMemoryWrite32((ctx)->mem, (addr), (val))

// Unaligned Load/Store
// Used for LWL/LWR/SWL/SWR instructions

inline uint32_t DO_LWL(recomp_context *ctx, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = ps1::runtimeMemoryRead32(ctx->mem, aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    return (rt & 0x00FFFFFF) | (word << 24);
  case 1:
    return (rt & 0x0000FFFF) | (word << 16);
  case 2:
    return (rt & 0x000000FF) | (word << 8);
  case 3:
    return word;
  default:
    return rt;
  }
}

inline uint32_t DO_LWR(recomp_context *ctx, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = ps1::runtimeMemoryRead32(ctx->mem, aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    return word;
  case 1:
    return (rt & 0xFF000000) | (word >> 8);
  case 2:
    return (rt & 0xFFFF0000) | (word >> 16);
  case 3:
    return (rt & 0xFFFFFF00) | (word >> 24);
  default:
    return rt;
  }
}

inline void DO_SWL(recomp_context *ctx, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = ps1::runtimeMemoryRead32(ctx->mem, aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    word = (word & 0xFFFFFF00) | (rt >> 24);
    break;
  case 1:
    word = (word & 0xFFFF0000) | (rt >> 16);
    break;
  case 2:
    word = (word & 0xFF000000) | (rt >> 8);
    break;
  case 3:
    word = rt;
    break;
  }
  ps1::runtimeMemoryWrite32(ctx->mem, aligned, word);
}

inline void DO_SWR(recomp_context *ctx, uint32_t rt, uint32_t addr) {
  uint32_t aligned = addr & ~3u;
  uint32_t word = ps1::runtimeMemoryRead32(ctx->mem, aligned);
  uint32_t shift = (addr & 3u);

  switch (shift) {
  case 0:
    word = rt;
    break;
  case 1:
    word = (word & 0x000000FF) | (rt << 8);
    break;
  case 2:
    word = (word & 0x0000FFFF) | (rt << 16);
    break;
  case 3:
    word = (word & 0x00FFFFFF) | (rt << 24);
    break;
  }
  ps1::runtimeMemoryWrite32(ctx->mem, aligned, word);
}

// GTE Register Access
// Used by GteEmitter for MFC2/MTC2/CFC2/CTC2

inline uint32_t gte_read_data(ps1::CPUContext *ctx, uint8_t reg) {
  return ps1::GTE::readData(ctx, reg);
}

inline void gte_write_data(ps1::CPUContext *ctx, uint8_t reg, uint32_t val) {
  ps1::GTE::writeData(ctx, reg, val);
}

inline uint32_t gte_read_control(ps1::CPUContext *ctx, uint8_t reg) {
  return ps1::GTE::readControl(ctx, reg);
}

inline void gte_write_control(ps1::CPUContext *ctx, uint8_t reg, uint32_t val) {
  ps1::GTE::writeControl(ctx, reg, val);
}

[[gnu::always_inline]] inline void pgxp_invalidate_gpr(recomp_context *ctx,
                                                       uint8_t reg) {
  if (ps1::pgxpTrackingEnabled() && reg != 0)
    ctx->pgxpGpr[reg & 31].invalidate();
}

inline bool pgxp_has_component(const ps1::PGXPValue &value, uint32_t raw,
                               bool yComponent) {
  return yComponent ? value.hasY(raw) : value.hasX(raw);
}

inline uint16_t pgxp_half(uint32_t value, bool yComponent) {
  return static_cast<uint16_t>(yComponent ? value >> 16 : value);
}

inline float pgxp_component(const ps1::PGXPValue &value, bool yComponent) {
  return yComponent ? value.y : value.x;
}

inline bool pgxp_compatible_provenance(const ps1::PGXPValue &left,
                                       const ps1::PGXPValue &right) {
  const bool leftHasZ = (left.flags & ps1::PGXPValue::VALID_Z) != 0;
  const bool rightHasZ = (right.flags & ps1::PGXPValue::VALID_Z) != 0;
  if (leftHasZ != rightHasZ)
    return false;
  if (leftHasZ && (left.z != right.z || left.origin != right.origin))
    return false;

  const bool leftHasProjective =
      (left.flags & ps1::PGXPValue::VALID_SIGNED_PROJECTION) != 0;
  const bool rightHasProjective =
      (right.flags & ps1::PGXPValue::VALID_SIGNED_PROJECTION) != 0;
  if (leftHasProjective != rightHasProjective)
    return false;
  if (!leftHasProjective)
    return true;
  if (left.projectionId == 0 || left.projectionId != right.projectionId ||
      left.projectionContext != right.projectionContext || left.z != right.z ||
      left.nearDepth != right.nearDepth || left.origin != right.origin ||
      left.projectionTransformValueFingerprint !=
          right.projectionTransformValueFingerprint)
    return false;

  const uint8_t leftIntrinsic =
      left.flags & ps1::PGXPValue::VALID_INTRINSIC_PROVENANCE;
  const uint8_t rightIntrinsic =
      right.flags & ps1::PGXPValue::VALID_INTRINSIC_PROVENANCE;
  if (leftIntrinsic != rightIntrinsic)
    return false;
  if ((leftIntrinsic & ps1::PGXPValue::VALID_TRANSFORM_LINEAGE) != 0 &&
      left.projectionTransformLineage != right.projectionTransformLineage)
    return false;
  if ((leftIntrinsic & ps1::PGXPValue::VALID_MODEL_VERTEX_SOURCE) != 0 &&
      left.modelVertexSource != right.modelVertexSource)
    return false;
  return (leftIntrinsic & ps1::PGXPValue::VALID_MODEL_VALUE_FLOW) == 0 ||
         (left.modelValueFlowSourceFingerprint ==
              right.modelValueFlowSourceFingerprint &&
          left.modelValueFlowProgramFingerprint ==
              right.modelValueFlowProgramFingerprint);
}

inline const ps1::PGXPValue *
pgxp_pick_component(const ps1::PGXPValue &left, uint32_t leftRaw,
                    const ps1::PGXPValue &right, uint32_t rightRaw,
                    uint32_t result, bool yComponent, float &component) {
  const uint16_t resultHalf = pgxp_half(result, yComponent);
  const bool leftMatches = pgxp_has_component(left, leftRaw, yComponent) &&
                           pgxp_half(leftRaw, yComponent) == resultHalf;
  const bool rightMatches = pgxp_has_component(right, rightRaw, yComponent) &&
                            pgxp_half(rightRaw, yComponent) == resultHalf;
  if (!leftMatches && !rightMatches)
    return nullptr;
  if (leftMatches && rightMatches) {
    const float leftComponent = pgxp_component(left, yComponent);
    const float rightComponent = pgxp_component(right, yComponent);
    if (leftComponent != rightComponent ||
        !pgxp_compatible_provenance(left, right))
      return nullptr;
  }
  const ps1::PGXPValue *source = leftMatches ? &left : &right;
  component = pgxp_component(*source, yComponent);
  return source;
}

[[gnu::always_inline]] inline void
pgxp_finish_alu_result(recomp_context *ctx, uint8_t destination,
                       uint32_t result, ps1::PGXPValue &precision) {
  if (destination == 0)
    return;
  precision.value = result;
  ctx->r[destination & 31] = result;
  if ((precision.flags & ps1::PGXPValue::VALID_XY) != 0)
    ctx->pgxpGpr[destination & 31] = precision;
  else
    ctx->pgxpGpr[destination & 31].invalidate();
}

inline void pgxp_move_gpr(recomp_context *ctx, uint8_t destination,
                          uint8_t source, uint32_t value) {
  if (destination == 0)
    return;
  ctx->r[destination & 31] = value;
  if (!ps1::pgxpTrackingEnabled())
    return;
  const ps1::PGXPValue &precision = ctx->pgxpGpr[source & 31];
  if (precision.hasX(value) || precision.hasY(value))
    ctx->pgxpGpr[destination & 31] = precision;
  else
    ctx->pgxpGpr[destination & 31].invalidate();
}

inline uint32_t pgxp_copy_gpr(recomp_context *ctx, uint8_t destination,
                              uint8_t source, uint32_t value) {
  if (ps1::pgxpTrackingEnabled() && destination != 0) {
    const ps1::PGXPValue &precision = ctx->pgxpGpr[source & 31];
    if (precision.hasX(value) || precision.hasY(value))
      ctx->pgxpGpr[destination & 31] = precision;
    else
      ctx->pgxpGpr[destination & 31].invalidate();
  }
  return value;
}

inline void pgxp_andi(recomp_context *ctx, uint8_t destination, uint8_t source,
                      uint16_t immediate) {
  const uint32_t sourceRaw = source == 0 ? 0 : ctx->r[source & 31];
  const uint32_t result = sourceRaw & immediate;
  if (destination == 0)
    return;
  if (!ps1::pgxpTrackingEnabled()) {
    ctx->r[destination & 31] = result;
    return;
  }
  const ps1::PGXPValue &sourcePrecision = ctx->pgxpGpr[source & 31];
  const bool hasX = sourcePrecision.hasX(sourceRaw);
  const bool hasY = sourcePrecision.hasY(sourceRaw);
  if (!hasX && !hasY) {
    ctx->r[destination & 31] = result;
    ctx->pgxpGpr[destination & 31].invalidate();
    return;
  }
  ps1::PGXPValue output;
  output.value = result;
  if (hasX && pgxp_half(sourceRaw, false) == pgxp_half(result, false)) {
    output.x = sourcePrecision.x;
    output.flags |= ps1::PGXPValue::VALID_X;
  }
  if (hasY && pgxp_half(sourceRaw, true) == pgxp_half(result, true)) {
    output.y = sourcePrecision.y;
    output.flags |= ps1::PGXPValue::VALID_Y;
  }
  if ((output.flags & ps1::PGXPValue::VALID_XY) != 0 &&
      (sourcePrecision.flags & ps1::PGXPValue::VALID_Z) != 0) {
    output.z = sourcePrecision.z;
    output.flags |= ps1::PGXPValue::VALID_Z;
    output.origin = sourcePrecision.origin;
  }
  bool copiedProjective = false;
  if ((output.flags & ps1::PGXPValue::VALID_X) != 0 &&
      sourcePrecision.hasProjectiveX(sourceRaw)) {
    output.qx = sourcePrecision.qx;
    output.flags |= ps1::PGXPValue::VALID_QX;
    copiedProjective = true;
  }
  if ((output.flags & ps1::PGXPValue::VALID_Y) != 0 &&
      sourcePrecision.hasProjectiveY(sourceRaw)) {
    output.qy = sourcePrecision.qy;
    output.flags |= ps1::PGXPValue::VALID_QY;
    copiedProjective = true;
  }
  if (copiedProjective) {
    output.z = sourcePrecision.z;
    output.nearDepth = sourcePrecision.nearDepth;
    output.projectionId = sourcePrecision.projectionId;
    output.projectionContext = sourcePrecision.projectionContext;
    output.gameProfileCallsite = sourcePrecision.gameProfileCallsite;
    output.projectionTransformValueFingerprint =
        sourcePrecision.projectionTransformValueFingerprint;
    output.projectionTransformLineage =
        sourcePrecision.projectionTransformLineage;
    output.modelVertexSource = sourcePrecision.modelVertexSource;
    output.modelValueFlowSourceFingerprint =
        sourcePrecision.modelValueFlowSourceFingerprint;
    output.modelValueFlowProgramFingerprint =
        sourcePrecision.modelValueFlowProgramFingerprint;
    output.flags |=
        sourcePrecision.flags & ps1::PGXPValue::VALID_INTRINSIC_PROVENANCE;
    output.origin = sourcePrecision.origin;
  }
  pgxp_finish_alu_result(ctx, destination, result, output);
}

inline void pgxp_addiu(recomp_context *ctx, uint8_t destination, uint8_t source,
                       int16_t immediate) {
  const uint32_t sourceRaw = source == 0 ? 0 : ctx->r[source & 31];
  const uint32_t result =
      sourceRaw + static_cast<uint32_t>(static_cast<int32_t>(immediate));
  if (destination == 0)
    return;
  if (!ps1::pgxpTrackingEnabled()) {
    ctx->r[destination & 31] = result;
    return;
  }
  const ps1::PGXPValue &sourcePrecision = ctx->pgxpGpr[source & 31];
  const bool hasX = sourcePrecision.hasX(sourceRaw);
  const bool hasY = sourcePrecision.hasY(sourceRaw);
  if (!hasX && !hasY) {
    ctx->r[destination & 31] = result;
    ctx->pgxpGpr[destination & 31].invalidate();
    return;
  }
  const float deltaX = static_cast<float>(static_cast<int16_t>(result) -
                                          static_cast<int16_t>(sourceRaw));
  const float deltaY =
      static_cast<float>(static_cast<int16_t>(result >> 16) -
                         static_cast<int16_t>(sourceRaw >> 16));
  ps1::PGXPValue output;
  output.value = result;
  if (hasX) {
    output.x = sourcePrecision.x + deltaX;
    output.flags |= ps1::PGXPValue::VALID_X;
  }
  if (hasY) {
    output.y = sourcePrecision.y + deltaY;
    output.flags |= ps1::PGXPValue::VALID_Y;
  }
  if ((output.flags & ps1::PGXPValue::VALID_XY) != 0 &&
      (sourcePrecision.flags & ps1::PGXPValue::VALID_Z) != 0) {
    output.z = sourcePrecision.z;
    output.flags |= ps1::PGXPValue::VALID_Z;
    output.origin = sourcePrecision.origin;
  }
  bool copiedProjective = false;
  if ((output.flags & ps1::PGXPValue::VALID_X) != 0 &&
      sourcePrecision.hasProjectiveX(sourceRaw)) {
    output.qx = sourcePrecision.qx + deltaX * sourcePrecision.z;
    output.flags |= ps1::PGXPValue::VALID_QX;
    copiedProjective = true;
  }
  if ((output.flags & ps1::PGXPValue::VALID_Y) != 0 &&
      sourcePrecision.hasProjectiveY(sourceRaw)) {
    output.qy = sourcePrecision.qy + deltaY * sourcePrecision.z;
    output.flags |= ps1::PGXPValue::VALID_QY;
    copiedProjective = true;
  }
  if (copiedProjective) {
    output.z = sourcePrecision.z;
    output.nearDepth = sourcePrecision.nearDepth;
    output.projectionId = sourcePrecision.projectionId;
    output.projectionContext = sourcePrecision.projectionContext;
    output.gameProfileCallsite = sourcePrecision.gameProfileCallsite;
    output.projectionTransformValueFingerprint =
        sourcePrecision.projectionTransformValueFingerprint;
    output.projectionTransformLineage =
        sourcePrecision.projectionTransformLineage;
    output.modelVertexSource = sourcePrecision.modelVertexSource;
    output.modelValueFlowSourceFingerprint =
        sourcePrecision.modelValueFlowSourceFingerprint;
    output.modelValueFlowProgramFingerprint =
        sourcePrecision.modelValueFlowProgramFingerprint;
    output.flags |=
        sourcePrecision.flags & ps1::PGXPValue::VALID_INTRINSIC_PROVENANCE;
    output.origin = sourcePrecision.origin;
  }
  pgxp_finish_alu_result(ctx, destination, result, output);
}

inline void pgxp_binary_bitwise(recomp_context *ctx, uint8_t destination,
                                uint8_t leftRegister, uint8_t rightRegister,
                                bool bitwiseOr) {
  const uint32_t leftRaw = leftRegister == 0 ? 0 : ctx->r[leftRegister & 31];
  const uint32_t rightRaw = rightRegister == 0 ? 0 : ctx->r[rightRegister & 31];
  const uint32_t result = bitwiseOr ? leftRaw | rightRaw : leftRaw & rightRaw;
  if (destination == 0)
    return;
  if (!ps1::pgxpTrackingEnabled()) {
    ctx->r[destination & 31] = result;
    return;
  }
  const ps1::PGXPValue &leftPrecision = ctx->pgxpGpr[leftRegister & 31];
  const ps1::PGXPValue &rightPrecision = ctx->pgxpGpr[rightRegister & 31];
  if (((leftPrecision.flags | rightPrecision.flags) &
       ps1::PGXPValue::VALID_XY) == 0) {
    ctx->r[destination & 31] = result;
    ctx->pgxpGpr[destination & 31].invalidate();
    return;
  }
  ps1::PGXPValue output;
  output.value = result;
  const ps1::PGXPValue *xSource =
      pgxp_pick_component(leftPrecision, leftRaw, rightPrecision, rightRaw,
                          result, false, output.x);
  const ps1::PGXPValue *ySource = pgxp_pick_component(
      leftPrecision, leftRaw, rightPrecision, rightRaw, result, true, output.y);
  if (xSource)
    output.flags |= ps1::PGXPValue::VALID_X;
  if (ySource)
    output.flags |= ps1::PGXPValue::VALID_Y;

  const ps1::PGXPValue *provenance = xSource ? xSource : ySource;
  bool compatibleZ = provenance != nullptr &&
                     (provenance->flags & ps1::PGXPValue::VALID_Z) != 0;
  if (compatibleZ && xSource && ySource && xSource != ySource)
    compatibleZ = (ySource->flags & ps1::PGXPValue::VALID_Z) != 0 &&
                  pgxp_compatible_provenance(*xSource, *ySource);
  if (compatibleZ) {
    output.z = provenance->z;
    output.flags |= ps1::PGXPValue::VALID_Z;
    output.origin = provenance->origin;
  }

  const auto sourceRaw = [&](const ps1::PGXPValue *source) {
    return source == &leftPrecision ? leftRaw : rightRaw;
  };
  const bool projectiveX =
      xSource != nullptr && xSource->hasProjectiveX(sourceRaw(xSource));
  const bool projectiveY =
      ySource != nullptr && ySource->hasProjectiveY(sourceRaw(ySource));
  const bool compatibleProjective =
      !(projectiveX && projectiveY && xSource != ySource) ||
      pgxp_compatible_provenance(*xSource, *ySource);
  if (compatibleProjective && (projectiveX || projectiveY)) {
    const ps1::PGXPValue *projectiveSource = projectiveX ? xSource : ySource;
    output.z = projectiveSource->z;
    output.nearDepth = projectiveSource->nearDepth;
    output.projectionId = projectiveSource->projectionId;
    output.projectionContext = projectiveSource->projectionContext;
    output.gameProfileCallsite = projectiveSource->gameProfileCallsite;
    output.projectionTransformValueFingerprint =
        projectiveSource->projectionTransformValueFingerprint;
    output.projectionTransformLineage =
        projectiveSource->projectionTransformLineage;
    output.modelVertexSource = projectiveSource->modelVertexSource;
    output.modelValueFlowSourceFingerprint =
        projectiveSource->modelValueFlowSourceFingerprint;
    output.modelValueFlowProgramFingerprint =
        projectiveSource->modelValueFlowProgramFingerprint;
    output.flags |=
        projectiveSource->flags & ps1::PGXPValue::VALID_INTRINSIC_PROVENANCE;
    output.origin = projectiveSource->origin;
    if (projectiveX) {
      output.qx = xSource->qx;
      output.flags |= ps1::PGXPValue::VALID_QX;
    }
    if (projectiveY) {
      output.qy = ySource->qy;
      output.flags |= ps1::PGXPValue::VALID_QY;
    }
  }
  pgxp_finish_alu_result(ctx, destination, result, output);
}

inline void pgxp_and(recomp_context *ctx, uint8_t destination, uint8_t left,
                     uint8_t right) {
  pgxp_binary_bitwise(ctx, destination, left, right, false);
}

inline void pgxp_or(recomp_context *ctx, uint8_t destination, uint8_t left,
                    uint8_t right) {
  pgxp_binary_bitwise(ctx, destination, left, right, true);
}

// Process-lifetime switch: environment configuration is complete before any
// recompiled guest instruction runs. Keeping it at namespace scope avoids the
// thread-safe local-static guard on every LW in an -O0 diagnostic game TU.
inline const bool pgxpRamLookupDiagnosticsRequested =
    std::getenv("PS1_METRICS") != nullptr ||
    std::getenv("PS1_TRACE_PGXP_VERTICES") != nullptr;

inline uint32_t pgxp_load_word(recomp_context *ctx, uint8_t destination,
                               uint32_t address) {
  const uint32_t value = ps1::runtimeMemoryRead32(ctx->mem, address);
  if (ps1::pgxpTrackingEnabled() && destination != 0) {
    // Preserve exhaustive miss accounting only in explicitly diagnostic
    // processes. Normal play skips the full PGXP lookup for the overwhelmingly
    // common ordinary integer load.
    if (ps1::runtimeMemoryMightHavePrecision(ctx->mem, address) ||
        pgxpRamLookupDiagnosticsRequested) {
      ps1::PGXPValue precision;
      if (ps1::runtimeMemoryLookupPrecision(ctx->mem, address, value,
                                            precision))
        ctx->pgxpGpr[destination & 31] = precision;
    }
  }
  return value;
}

inline void pgxp_store_word(recomp_context *ctx, uint8_t source,
                            uint32_t address, uint32_t value) {
  if (!ps1::pgxpTrackingEnabled()) {
    ps1::runtimeMemoryWrite32(ctx->mem, address, value);
    return;
  }
  const ps1::PGXPValue &precision = ctx->pgxpGpr[source & 31];
  if (precision.hasXY(value))
    ps1::countPGXPStage(ps1::PGXPStage::GprStoreHit);
  ps1::runtimeMemoryWrite32Tracked(
      ctx->mem, address, value, precision.hasXY(value) ? &precision : nullptr);
}

inline uint8_t pgxp_data_slot(uint8_t reg) {
  reg &= 31;
  return reg == ps1::GTE_SXYP ? ps1::GTE_SXY2 : reg;
}

inline uint32_t pgxp_mfc2(recomp_context *ctx, uint8_t destination,
                          uint8_t dataReg) {
  const uint32_t value = ps1::GTE::readData(ctx, dataReg);
  if (!ps1::pgxpTrackingEnabled())
    return value;
  if (destination != 0) {
    const ps1::PGXPValue &precision = ctx->pgxpGteData[pgxp_data_slot(dataReg)];
    if (precision.hasX(value) || precision.hasY(value)) {
      ctx->pgxpGpr[destination & 31] = precision;
      if (pgxp_data_slot(dataReg) >= ps1::GTE_SXY0 &&
          pgxp_data_slot(dataReg) <= ps1::GTE_SXY2) {
        ps1::countPGXPStage(precision.hasXY(value) ? ps1::PGXPStage::Mfc2Hit
                                                   : ps1::PGXPStage::Mfc2Miss);
      }
    } else {
      ctx->pgxpGpr[destination & 31].invalidate();
      if (pgxp_data_slot(dataReg) >= ps1::GTE_SXY0 &&
          pgxp_data_slot(dataReg) <= ps1::GTE_SXY2)
        ps1::countPGXPStage(ps1::PGXPStage::Mfc2Miss);
    }
  }
  return value;
}

inline void pgxp_mtc2(recomp_context *ctx, uint8_t dataReg, uint8_t source,
                      uint32_t value) {
  ps1::GTE::writeData(ctx, dataReg, value);
  if (!ps1::pgxpTrackingEnabled())
    return;
  const ps1::PGXPValue &precision = ctx->pgxpGpr[source & 31];
  if (precision.hasX(value) || precision.hasY(value))
    ctx->pgxpGteData[pgxp_data_slot(dataReg)] = precision;
  else
    ctx->pgxpGteData[pgxp_data_slot(dataReg)].invalidate();
}

inline void pgxp_lwc2(recomp_context *ctx, uint8_t dataReg, uint32_t address) {
  const uint32_t value = ps1::runtimeMemoryRead32(ctx->mem, address);
  ps1::GTE::writeData(ctx, dataReg, value);
  if (!ps1::pgxpTrackingEnabled())
    return;
  ps1::GTE::noteDataSource(ctx, dataReg, address);
  ps1::PGXPValue precision;
  if (ps1::runtimeMemoryLookupPrecision(ctx->mem, address, value, precision))
    ctx->pgxpGteData[pgxp_data_slot(dataReg)] = precision;
  else
    ctx->pgxpGteData[pgxp_data_slot(dataReg)].invalidate();
}

inline void pgxp_swc2(recomp_context *ctx, uint8_t dataReg, uint32_t address) {
  const uint32_t value = ps1::GTE::readData(ctx, dataReg);
  if (!ps1::pgxpTrackingEnabled()) {
    ps1::runtimeMemoryWrite32(ctx->mem, address, value);
    return;
  }
  const ps1::PGXPValue &precision = ctx->pgxpGteData[pgxp_data_slot(dataReg)];
  if (pgxp_data_slot(dataReg) >= ps1::GTE_SXY0 &&
      pgxp_data_slot(dataReg) <= ps1::GTE_SXY2)
    ps1::countPGXPStage(precision.hasXY(value) ? ps1::PGXPStage::Swc2Hit
                                               : ps1::PGXPStage::Swc2Miss);
  ps1::runtimeMemoryWrite32Tracked(
      ctx->mem, address, value, precision.hasXY(value) ? &precision : nullptr);
}

// Control flow and System macros
#define BREAK(ctx) throw ps1::CpuException(ps1::ExceptionCause::Bp)

// SYSCALL: EnterCritical ($a0=1) returns old SR IE bit in $v0,
//          ExitCritical  ($a0=2) is a NOP in HLE.
#define SYSCALL(ctx)                                                           \
  do {                                                                         \
    if ((ctx)->r4 == 1) { /* EnterCritical */                                  \
      (ctx)->r2 = (ctx)->cop0[ps1::COP0_SR] & 0x404;                           \
      (ctx)->cop0[ps1::COP0_SR] &= ~0x404u;                                    \
    } else if ((ctx)->r4 == 2) { /* ExitCritical */                            \
      (ctx)->cop0[ps1::COP0_SR] |= 0x404u;                                     \
    }                                                                          \
  } while (0)

// Forward declaration -- generated by ps1Recomp in recompiled_out.cpp
void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

// Indirect-call diagnostic -- gated on `PS1_INDIRECT_TRACE=1`.  Cached at
// first use so the fast path is a single load+branch.  When a non-zero
// `PS1_INDIRECT_TRACE_RA=0x...` is set, only calls whose return address
// (r31) matches the filter are logged -- surgical inspection of one call
// site instead of drowning in every indirect call in the binary.
namespace ps1_dbg {
inline bool loop_trace_enabled() {
  static const bool v = []() {
    const char *e = std::getenv("PS1_LOOP_TRACE");
    return e && std::strcmp(e, "0") != 0 && e[0] != '\0';
  }();
  return v;
}
inline bool indirect_trace_enabled() {
  static const bool v = []() {
    const char *e = std::getenv("PS1_INDIRECT_TRACE");
    return e && std::strcmp(e, "0") != 0 && e[0] != '\0';
  }();
  return v;
}
inline uint32_t indirect_trace_ra_filter() {
  static const uint32_t f = []() {
    const char *e = std::getenv("PS1_INDIRECT_TRACE_RA");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return f;
}
inline uint32_t function_trace_target() {
  static const uint32_t target = []() {
    const char *e = std::getenv("PS1_TRACE_FUNCTION");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return target;
}
inline uint32_t gte_trace_trigger_pc() {
  static const uint32_t target = []() {
    const char *e = std::getenv("PS1_TRACE_GTE_TRIGGER_PC");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return target;
}
inline uint32_t gpu_trace_trigger_pc() {
  static const uint32_t target = []() {
    const char *e = std::getenv("PS1_TRACE_GPU_TRIGGER_PC");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return target;
}
inline int function_trace_memory_reg() {
  static const int reg = []() {
    const char *e = std::getenv("PS1_TRACE_FUNCTION_MEM_REG");
    if (!e || !*e)
      return -1;
    const long parsed = std::strtol(e, nullptr, 0);
    return parsed >= 0 && parsed < 32 ? static_cast<int>(parsed) : -1;
  }();
  return reg;
}

struct PcTraceTargets {
  std::array<uint32_t, 64> addresses{};
  size_t count = 0;
  uint64_t frameFrom = 0;
  uint64_t frameTo = UINT64_MAX;
};

inline const PcTraceTargets &pc_trace_targets() {
  static const PcTraceTargets targets = []() {
    PcTraceTargets parsed;
    const char *cursor = std::getenv("PS1_TRACE_MIPS_PC");
    while (cursor && *cursor && parsed.count < parsed.addresses.size()) {
      while (*cursor == ',' || *cursor == ';' || *cursor == ' ' ||
             *cursor == '\t')
        ++cursor;
      if (!*cursor)
        break;
      char *end = nullptr;
      const unsigned long value = std::strtoul(cursor, &end, 0);
      if (end == cursor)
        break;
      parsed.addresses[parsed.count++] = static_cast<uint32_t>(value);
      cursor = end;
    }

    auto parseFrame = [](const char *name, uint64_t fallback) {
      const char *value = std::getenv(name);
      if (!value || !*value)
        return fallback;
      char *end = nullptr;
      const unsigned long long result = std::strtoull(value, &end, 10);
      return end != value && *end == '\0' ? static_cast<uint64_t>(result)
                                          : fallback;
    };
    parsed.frameFrom = parseFrame("PS1_TRACE_MIPS_FRAME_FROM", 0);
    parsed.frameTo = parseFrame("PS1_TRACE_MIPS_FRAME_TO", UINT64_MAX);
    return parsed;
  }();
  return targets;
}

inline bool pc_trace_matches(uint32_t pc) {
  const auto &targets = pc_trace_targets();
  if (targets.count == 0)
    return false;
  const uint64_t frame = ps1::metrics::traceFrame();
  if (frame < targets.frameFrom || frame > targets.frameTo)
    return false;
  for (size_t i = 0; i < targets.count; ++i) {
    if (targets.addresses[i] == pc)
      return true;
  }
  return false;
}

// Generated code expands these hooks at every guest instruction/function or
// branch boundary. Reading one process-lifetime flag keeps the ordinary path
// to a single predictable load/branch; parsing targets and calling the richer
// helpers is paid only by an explicitly requested diagnostic run.
inline const bool instruction_trace_requested = [] {
  return pc_trace_targets().count != 0 || gte_trace_trigger_pc() != 0 ||
         gpu_trace_trigger_pc() != 0;
}();
inline const bool function_trace_requested = function_trace_target() != 0;
inline const bool loop_trace_requested = loop_trace_enabled();
inline const bool indirect_trace_requested = indirect_trace_enabled();
} // namespace ps1_dbg

// Instruction-boundary checkpoint trace. A comma-separated target list makes
// one run capture the same semantic transition points as the instrumented
// emulator donors. The hook is emitted before instructions (including delay
// slots), so its register state has the same meaning as an interpreter's PC
// callback.
#define PS1_PC_TRACE_HOOK(ctx, addr)                                           \
  do {                                                                         \
    if (ps1_dbg::instruction_trace_requested) [[unlikely]] {                   \
      const uint32_t _checkpoint_pc = static_cast<uint32_t>(addr);             \
      if (ps1_dbg::gte_trace_trigger_pc() == _checkpoint_pc)                   \
        ps1::metrics::triggerGteTraceForCurrentFrame();                        \
      if (ps1_dbg::gpu_trace_trigger_pc() == _checkpoint_pc)                   \
        ps1::metrics::triggerGpuWordTraceForCurrentFrame();                    \
      if (ps1_dbg::pc_trace_matches(_checkpoint_pc)) {                         \
        ps1::metrics::trace("mips.pc", {{"pc", _checkpoint_pc},                \
                                        {"ra", (ctx)->r[31]},                  \
                                        {"sp", (ctx)->r[29]},                  \
                                        {"a0", (ctx)->r[4]},                   \
                                        {"a1", (ctx)->r[5]},                   \
                                        {"a2", (ctx)->r[6]},                   \
                                        {"a3", (ctx)->r[7]},                   \
                                        {"s0", (ctx)->r[16]},                  \
                                        {"s1", (ctx)->r[17]},                  \
                                        {"s2", (ctx)->r[18]},                  \
                                        {"s3", (ctx)->r[19]},                  \
                                        {"v0", (ctx)->r[2]},                   \
                                        {"v1", (ctx)->r[3]},                   \
                                        {"hi", (ctx)->hi},                     \
                                        {"lo", (ctx)->lo}});                   \
      }                                                                        \
    }                                                                          \
  } while (0)

// Targeted function-entry trace for donor comparison. The optional memory
// register records three words from a caller-selected pointer without making
// generic tracing dereference arbitrary guest addresses.
#define PS1_FUNCTION_TRACE_HOOK(ctx, addr)                                     \
  do {                                                                         \
    if (ps1_dbg::function_trace_requested &&                                   \
        ps1_dbg::function_trace_target() == static_cast<uint32_t>(addr))       \
        [[unlikely]] {                                                         \
      const int _mem_reg = ps1_dbg::function_trace_memory_reg();               \
      const uint32_t _mem_addr =                                               \
          _mem_reg >= 0 ? (ctx)->r[static_cast<size_t>(_mem_reg)] : 0u;        \
      ps1::metrics::trace(                                                     \
          "mips.function_entry",                                               \
          {{"pc", static_cast<uint32_t>(addr)},                                \
           {"ra", (ctx)->r[31]},                                               \
           {"sp", (ctx)->r[29]},                                               \
           {"a0", (ctx)->r[4]},                                                \
           {"a1", (ctx)->r[5]},                                                \
           {"a2", (ctx)->r[6]},                                                \
           {"a3", (ctx)->r[7]},                                                \
           {"s0", (ctx)->r[16]},                                               \
           {"s1", (ctx)->r[17]},                                               \
           {"s2", (ctx)->r[18]},                                               \
           {"s3", (ctx)->r[19]},                                               \
           {"mem_reg", _mem_reg},                                              \
           {"mem_addr", _mem_addr},                                            \
           {"mem0", _mem_reg >= 0                                              \
                        ? ps1::runtimeMemoryRead32((ctx)->mem, _mem_addr)      \
                        : 0u},                                                 \
           {"mem4", _mem_reg >= 0                                              \
                        ? ps1::runtimeMemoryRead32((ctx)->mem, _mem_addr + 4)  \
                        : 0u},                                                 \
           {"mem8", _mem_reg >= 0                                              \
                        ? ps1::runtimeMemoryRead32((ctx)->mem, _mem_addr + 8)  \
                        : 0u}});                                               \
    }                                                                          \
  } while (0)

// Reports a backward branch once it has executed enough times to be a likely
// stalled wait. Each expansion owns its counter, so the printed guest PC
// identifies the exact loop without flooding normal execution or adding any
// output unless PS1_LOOP_TRACE=1 is requested.
#define PS1_LOOP_TRACE_HOOK(ctx, addr)                                         \
  do {                                                                         \
    if (ps1_dbg::loop_trace_requested) [[unlikely]] {                          \
      static uint32_t _ps1_loop_iters = 0;                                     \
      if (++_ps1_loop_iters == (1u << 20)) {                                   \
        std::fprintf(stderr,                                                   \
                     "[LOOP] guest_pc=0x%08X sp=0x%08X "                       \
                     "drain_block_depth=%u\n",                                 \
                     static_cast<uint32_t>(addr), (ctx)->r[29],                \
                     (ctx)->callbackDrainBlockDepth);                          \
      }                                                                        \
    }                                                                          \
  } while (0)

#define PS1_INDIRECT_TRACE_HOOK(ctx, addr)                                     \
  do {                                                                         \
    if (ps1_dbg::indirect_trace_requested) [[unlikely]] {                      \
      uint32_t _ra_f = ps1_dbg::indirect_trace_ra_filter();                    \
      if (_ra_f == 0u || _ra_f == (ctx)->r[31]) {                              \
        std::fprintf(stderr, "[INDIRECT] RA=0x%08X -> 0x%08X\n", (ctx)->r[31], \
                     static_cast<uint32_t>(addr));                             \
      }                                                                        \
    }                                                                          \
  } while (0)

#define PS1_INDIRECT_TRACE_RETURN_HOOK(ctx, call_ra, addr)                     \
  do {                                                                         \
    if (ps1_dbg::indirect_trace_requested) [[unlikely]] {                      \
      uint32_t _ra_f = ps1_dbg::indirect_trace_ra_filter();                    \
      if (_ra_f == 0u || _ra_f == static_cast<uint32_t>(call_ra)) {            \
        std::fprintf(stderr,                                                   \
                     "[INDIRECT-RET] RA=0x%08X <- 0x%08X "                     \
                     "sp=0x%08X scratch_next=0x%08X\n",                        \
                     static_cast<uint32_t>(call_ra),                           \
                     static_cast<uint32_t>(addr), (ctx)->r[29],                \
                     ps1::runtimeMemoryRead32((ctx)->mem, 0x1F80000C));        \
      }                                                                        \
    }                                                                          \
  } while (0)

#define CALL_INDIRECT(ctx, addr)                                               \
  do {                                                                         \
    const uint32_t _indirect_addr = static_cast<uint32_t>(addr);               \
    const uint32_t _indirect_ra = (ctx)->r[31];                                \
    PS1_INDIRECT_TRACE_HOOK(ctx, _indirect_addr);                              \
    recomp_dispatch(rdram, ctx, _indirect_addr);                               \
    PS1_INDIRECT_TRACE_RETURN_HOOK(ctx, _indirect_ra, _indirect_addr);         \
  } while (0)
#define JUMP_INDIRECT(ctx, addr)                                               \
  do {                                                                         \
    PS1_INDIRECT_TRACE_HOOK(ctx, addr);                                        \
    recomp_dispatch(rdram, ctx, static_cast<uint32_t>(addr));                  \
    return;                                                                    \
  } while (0)
#define COP0_RFE(ctx) /* NOP for now */
