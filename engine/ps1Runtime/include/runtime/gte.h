#pragma once
/**
 * @file gte.h
 * @brief PS1 GTE (Geometry Transform Engine) -- 3D coprocessor on COP2.
 *
 * Implements all 22 GTE commands with PS1-accurate fixed-point math.
 * Recompiled MIPS reaches the GTE through `cop2` register reads/writes and
 * `cfc2`/`ctc2`/`mfc2`/`mtc2` plus the COP2 command word; ps1Recomp's
 * `gte_emitter.cpp` maps each command to the matching helper here.
 *
 * Key conventions:
 *   - Matrices/vectors are 1.3.12 fixed-point (signed 16-bit).
 *   - Translation vectors are 1.31.0 (signed 32-bit).
 *   - MAC0-MAC3 are 32-bit accumulators with no clamp on overflow.
 *   - IR0-IR3 are 16-bit intermediate results, clamped.
 *   - FLAG (`cop2c[31]`) carries overflow / underflow / divide bits.
 *   - `sf` bit: shift-fraction (`>> 12` when set).
 *   - `lm` bit: saturate negative IR values to 0 when set.
 */

#include <cstdint>
#include <runtime/cpu_context.h>

namespace ps1 {

// GTE Register Indices (Data registers cop2d[])

enum GteDataReg : uint8_t {
  GTE_VXY0 = 0,
  GTE_VZ0 = 1,
  GTE_VXY1 = 2,
  GTE_VZ1 = 3,
  GTE_VXY2 = 4,
  GTE_VZ2 = 5,
  GTE_RGBC = 6,
  GTE_OTZ = 7,
  GTE_IR0 = 8,
  GTE_IR1 = 9,
  GTE_IR2 = 10,
  GTE_IR3 = 11,
  GTE_SXY0 = 12,
  GTE_SXY1 = 13,
  GTE_SXY2 = 14,
  GTE_SXYP = 15,
  GTE_SZ0 = 16,
  GTE_SZ1 = 17,
  GTE_SZ2 = 18,
  GTE_SZ3 = 19,
  GTE_RGB0 = 20,
  GTE_RGB1 = 21,
  GTE_RGB2 = 22,
  GTE_MAC0 = 24,
  GTE_MAC1 = 25,
  GTE_MAC2 = 26,
  GTE_MAC3 = 27,
  GTE_IRGB = 28,
  GTE_ORGB = 29,
  GTE_LZCS = 30,
  GTE_LZCR = 31,
};

// GTE Register Indices (Control registers cop2c[])

enum GteControlReg : uint8_t {
  GTE_RT11RT12 = 0,
  GTE_RT13RT21 = 1,
  GTE_RT22RT23 = 2,
  GTE_RT31RT32 = 3,
  GTE_RT33 = 4,
  GTE_TRX = 5,
  GTE_TRY = 6,
  GTE_TRZ = 7,
  GTE_L11L12 = 8,
  GTE_L13L21 = 9,
  GTE_L22L23 = 10,
  GTE_L31L32 = 11,
  GTE_L33 = 12,
  GTE_RBK = 13,
  GTE_GBK = 14,
  GTE_BBK = 15,
  GTE_LR1LR2 = 16,
  GTE_LR3LG1 = 17,
  GTE_LG2LG3 = 18,
  GTE_LB1LB2 = 19,
  GTE_LB3 = 20,
  GTE_RFC = 21,
  GTE_GFC = 22,
  GTE_BFC = 23,
  GTE_OFX = 24,
  GTE_OFY = 25,
  GTE_H = 26,
  GTE_DQA = 27,
  GTE_DQB = 28,
  GTE_ZSF3 = 29,
  GTE_ZSF4 = 30,
  GTE_FLAG = 31,
};

// FLAG register bits

namespace gte_flag {
constexpr uint32_t IR0_SATURATED = (1u << 12);
constexpr uint32_t SY2_SATURATED = (1u << 13);
constexpr uint32_t SX2_SATURATED = (1u << 14);
constexpr uint32_t MAC0_UF = (1u << 15);
constexpr uint32_t MAC0_OF = (1u << 16);
constexpr uint32_t DIV_OVERFLOW = (1u << 17);
constexpr uint32_t SZ3_OTZ_SAT = (1u << 18);
constexpr uint32_t COLOR_B_SAT = (1u << 19);
constexpr uint32_t COLOR_G_SAT = (1u << 20);
constexpr uint32_t COLOR_R_SAT = (1u << 21);
constexpr uint32_t IR3_SATURATED = (1u << 22);
constexpr uint32_t IR2_SATURATED = (1u << 23);
constexpr uint32_t IR1_SATURATED = (1u << 24);
constexpr uint32_t MAC3_UF = (1u << 25);
constexpr uint32_t MAC2_UF = (1u << 26);
constexpr uint32_t MAC1_UF = (1u << 27);
constexpr uint32_t MAC3_OF = (1u << 28);
constexpr uint32_t MAC2_OF = (1u << 29);
constexpr uint32_t MAC1_OF = (1u << 30);
constexpr uint32_t ERROR_FLAG = (1u << 31);
} // namespace gte_flag

// GTE Runtime Class

class GTE {
public:
  // Versioned categorical provenance of the projection rotation/translation
  // writers. It contains no numeric transform values and is shared by PGXP
  // presentation dispatch and its pre-construction admission gate.
  static uint64_t projectionTransformLineage(const CPUContext *ctx) noexcept;
  static bool
  projectionTransformLineageComplete(const CPUContext *ctx) noexcept;

  /// Register access with side effects
  static uint32_t readData(CPUContext *ctx, uint8_t reg);
  static void writeData(CPUContext *ctx, uint8_t reg, uint32_t val);
  static void noteDataSource(CPUContext *ctx, uint8_t reg,
                             uint32_t sourceAddress);
  static uint32_t readControl(CPUContext *ctx, uint8_t reg);
  static void writeControl(CPUContext *ctx, uint8_t reg, uint32_t val,
                           uint32_t sourceAddress = UINT32_MAX);

  /// GTE Commands (all 22)
  static void RTPS(CPUContext *ctx, bool sf, bool lm);
  static void NCLIP(CPUContext *ctx, bool sf, bool lm);
  static void OP(CPUContext *ctx, bool sf, bool lm);
  static void DPCS(CPUContext *ctx, bool sf, bool lm);
  static void INTPL(CPUContext *ctx, bool sf, bool lm);
  static void MVMVA(CPUContext *ctx, uint8_t mx, uint8_t mv, uint8_t tv,
                    bool sf, bool lm);
  static void NCDS(CPUContext *ctx, bool sf, bool lm);
  static void CDP(CPUContext *ctx, bool sf, bool lm);
  static void NCDT(CPUContext *ctx, bool sf, bool lm);
  static void NCCS(CPUContext *ctx, bool sf, bool lm);
  static void CC(CPUContext *ctx, bool sf, bool lm);
  static void NCS(CPUContext *ctx, bool sf, bool lm);
  static void NCT(CPUContext *ctx, bool sf, bool lm);
  static void SQR(CPUContext *ctx, bool sf, bool lm);
  static void DCPL(CPUContext *ctx, bool sf, bool lm);
  static void DPCT(CPUContext *ctx, bool sf, bool lm);
  static void AVSZ3(CPUContext *ctx, bool sf, bool lm);
  static void AVSZ4(CPUContext *ctx, bool sf, bool lm);
  static void RTPT(CPUContext *ctx, bool sf, bool lm);
  static void GPF(CPUContext *ctx, bool sf, bool lm);
  static void GPL(CPUContext *ctx, bool sf, bool lm);
  static void NCCT(CPUContext *ctx, bool sf, bool lm);

private:
  // Internal helpers

  /// Get 16-bit signed values packed in a 32-bit register
  static int16_t lo16(uint32_t v) { return static_cast<int16_t>(v & 0xFFFF); }
  static int16_t hi16(uint32_t v) { return static_cast<int16_t>(v >> 16); }

  /// Read input vector (0=V0, 1=V1, 2=V2, 3=IR)
  static void getVector(CPUContext *ctx, uint8_t v, int16_t &vx, int16_t &vy,
                        int16_t &vz);

  /// Read 3x3 matrix (0=Rotation, 1=Light, 2=Color)
  static void getMatrix(CPUContext *ctx, uint8_t m, int16_t mat[3][3]);

  /// Read translation vector (0=TR, 1=BK, 2=FC, 3=None)
  static void getTranslation(CPUContext *ctx, uint8_t tv, int32_t tr[3]);

  /// Set MAC1-MAC3 and clamp to IR1-IR3
  static void setMAC(CPUContext *ctx, int64_t mac1, int64_t mac2, int64_t mac3,
                     bool sf);
  static void setIR(CPUContext *ctx, int32_t ir1, int32_t ir2, int32_t ir3,
                    bool lm);

  /// MAC0 / IR0 set
  static void setMAC0(CPUContext *ctx, int64_t val);
  static void setIR0(CPUContext *ctx, int32_t val, bool lm);

  /// Clamp and set FLAG bits
  static int32_t clampIR(CPUContext *ctx, int32_t val, uint8_t irIdx, bool lm);
  static void checkMAC(CPUContext *ctx, int64_t val, uint8_t macIdx);

  /// Push SXY FIFO (3 stages)
  static void pushSXY(CPUContext *ctx, int32_t sx, int32_t sy);

  /// Push SZ FIFO (4 stages)
  static void pushSZ(CPUContext *ctx, int32_t sz);

  /// Push RGB FIFO (3 stages)
  static void pushRGB(CPUContext *ctx, uint8_t r, uint8_t g, uint8_t b,
                      uint8_t cd);

  /// GTE unsigned Newton-Raphson division
  static uint32_t divide(CPUContext *ctx, uint16_t h, uint16_t sz3);

  /// Internal: matrix x vector + translation -> MAC/IR
  static void doMVMVA(CPUContext *ctx, uint8_t mx, uint8_t mv, uint8_t tv,
                      bool sf, bool lm);

  /// Internal: single perspective transform (used by RTPS/RTPT)
  static void doRTPS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm,
                     bool lastVertex);

  /// Internal: normal color single
  static void doNCS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm);

  /// Internal: normal color depth single
  static void doNCDS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm);

  /// Internal: normal color color single
  static void doNCCS(CPUContext *ctx, uint8_t vecIdx, bool sf, bool lm);

  /// Internal: depth cue single
  static void doDPCS(CPUContext *ctx, bool sf, bool lm, uint8_t r, uint8_t g,
                     uint8_t b);
};

} // namespace ps1
