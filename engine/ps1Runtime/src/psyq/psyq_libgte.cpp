#include "runtime/psyq/psyq_libgte.h"
#include "runtime/gte.h"
#include "runtime/gte_control_provenance.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ps1::psyq {

namespace {

// PsyQ MATRIX struct layout: short m[3][3] @ 0..17, pad @ 18..19,
// long t[3] @ 20..31.  Total 32 bytes.
constexpr uint32_t MATRIX_T_OFFSET = 20;

// 1.3.12 fixed-point unit value (= 1.0).
constexpr int32_t ONE_12 = 4096;

// PsyQ angle convention: 4096 units == full rotation (2pi rad).
// csin/ccos return 1.3.12 fixed-point (sin(90 deg) = 0x1000).
int16_t psyq_sin(int16_t a) {
  double rad = static_cast<double>(a) * 2.0 * M_PI / 4096.0;
  double s = std::sin(rad) * 4096.0;
  s = std::clamp(s, -32768.0, 32767.0);
  return static_cast<int16_t>(std::lround(s));
}

int16_t psyq_cos(int16_t a) {
  double rad = static_cast<double>(a) * 2.0 * M_PI / 4096.0;
  double c = std::cos(rad) * 4096.0;
  c = std::clamp(c, -32768.0, 32767.0);
  return static_cast<int16_t>(std::lround(c));
}

inline int16_t readShort(recomp_context *ctx, uint32_t addr) {
  return static_cast<int16_t>(ctx->mem->read16(addr));
}

inline int32_t readLong(recomp_context *ctx, uint32_t addr) {
  return static_cast<int32_t>(ctx->mem->read32(addr));
}

inline void writeShort(recomp_context *ctx, uint32_t addr, int16_t val) {
  ctx->mem->write16(addr, static_cast<uint16_t>(val));
}

inline void writeLong(recomp_context *ctx, uint32_t addr, int32_t val) {
  ctx->mem->write32(addr, static_cast<uint32_t>(val));
}

inline void writeProjectedSxy(recomp_context *ctx, uint32_t addr,
                              uint32_t val) {
  const PGXPValue &precision = ctx->pgxpGteData[GTE_SXY2];
  ctx->mem->write32Tracked(addr, val,
                           precision.hasXY(val) ? &precision : nullptr);
}

// Read 9 shorts (m[3][3]) from the PsyQ MATRIX at `p`.
void readMatrix3x3(recomp_context *ctx, uint32_t p, int16_t m[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      m[i][j] = readShort(ctx, p + (i * 3 + j) * 2);
}

void writeMatrix3x3(recomp_context *ctx, uint32_t p, const int16_t m[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      writeShort(ctx, p + (i * 3 + j) * 2, m[i][j]);
}

// 3x3 matrix multiply in 1.3.12 fixed-point: out = a x b.
// Each element is computed in 32-bit then >>12 to preserve scale.  Values
// outside int16 range are saturated (matches PsyQ behaviour qualitatively;
// the GTE proper uses 1.3.12 throughout, not int16).
void matMul3x3(const int16_t a[3][3], const int16_t b[3][3],
               int16_t out[3][3]) {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      int32_t s = 0;
      for (int k = 0; k < 3; ++k)
        s += static_cast<int32_t>(a[i][k]) * static_cast<int32_t>(b[k][j]);
      s >>= 12;
      s = std::clamp<int32_t>(s, -32768, 32767);
      out[i][j] = static_cast<int16_t>(s);
    }
  }
}

} // namespace

// Group 0.7

// SetGeomOffset(ofx, ofy):
//   PsyQ stores OFX/OFY as 1/65536 fixed-point screen offsets.  The libgte
//   wrapper does:  cop2c[OFX] = ofx << 16; cop2c[OFY] = ofy << 16;
void hle_libgte_SetGeomOffset(recomp_context *ctx) {
  int32_t ofx = static_cast<int32_t>(ctx->r[A0]);
  int32_t ofy = static_cast<int32_t>(ctx->r[A1]);
  ctx->cop2c[GTE_OFX] = static_cast<uint32_t>(ofx) << 16;
  ctx->cop2c[GTE_OFY] = static_cast<uint32_t>(ofy) << 16;
}

// SetGeomScreen(h): projection-plane distance, 16-bit unsigned.
void hle_libgte_SetGeomScreen(recomp_context *ctx) {
  ctx->cop2c[GTE_H] = ctx->r[A0] & 0xFFFFu;
}

// SetDQA(dqa): depth-cue gradient, 16-bit signed (stored as 32-bit).
void hle_libgte_SetDQA(recomp_context *ctx) {
  GTE::writeControl(ctx, GTE_DQA, ctx->r[A0]);
}

// Group 1.C -- control-register loaders

// InitGeom(): the COP0/COP2-enable part is unnecessary in the recompiled
// runtime, but the PsyQ routine also programs the GTE projection defaults.
// These constants are taken from the game's linked PsyQ InitGeom routine at
// 0x8004F4E0.  Omitting them leaves DQA/DQB at zero, which changes RTPS/RTPT
// MAC0, FLAG and IR0 even when the projected screen coordinates still match.
void hle_libgte_InitGeom(recomp_context *ctx) {
  GTE::writeControl(ctx, GTE_ZSF3, 0x0155u);
  GTE::writeControl(ctx, GTE_ZSF4, 0x0100u);
  GTE::writeControl(ctx, GTE_H, 0x03E8u);
  GTE::writeControl(ctx, GTE_DQA, 0xEF9Eu);
  GTE::writeControl(ctx, GTE_DQB, 0x01400000u);
  GTE::writeControl(ctx, GTE_OFX, 0u);
  GTE::writeControl(ctx, GTE_OFY, 0u);
}

// SetRotMatrix(MATRIX *m): copy m->m[3][3] into cop2c[0..4] (RT11RT12 ..
// RT33).  Real PsyQ uses 5 lwc2 instructions; we replicate by reading 5
// 32-bit words from the PsyQ MATRIX struct in PS1 RAM.  Bytes 18..19 (the
// pad word between m[][] and t[]) end up in the high half of cop2c[GTE_RT33];
// only the low 16 bits are read by the GTE so this is harmless.
void hle_libgte_SetRotMatrix(recomp_context *ctx) {
  const uint32_t p = ctx->r[A0];
  for (uint8_t reg = GTE_RT11RT12; reg <= GTE_RT33; ++reg) {
    const uint32_t source = p + reg * 4u;
    writeGteControlFromHost(ctx, reg, ctx->mem->read32(source),
                            HostGteControlWriter::PsyqSetRotationMatrix, source);
  }
}

// SetTransMatrix(MATRIX *m): copy m->t[3] into cop2c[5..7] (TRX/TRY/TRZ).
void hle_libgte_SetTransMatrix(recomp_context *ctx) {
  const uint32_t p = ctx->r[A0];
  for (uint8_t reg = GTE_TRX; reg <= GTE_TRZ; ++reg) {
    const uint32_t source = p + MATRIX_T_OFFSET + (reg - GTE_TRX) * 4u;
    writeGteControlFromHost(ctx, reg, ctx->mem->read32(source),
                            HostGteControlWriter::PsyqSetTranslationMatrix,
                            source);
  }
}

// SetLightMatrix(MATRIX *m): copy m->m[3][3] into cop2c[8..12] (L11..L33).
void hle_libgte_SetLightMatrix(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  ctx->cop2c[GTE_L11L12] = ctx->mem->read32(p + 0);
  ctx->cop2c[GTE_L13L21] = ctx->mem->read32(p + 4);
  ctx->cop2c[GTE_L22L23] = ctx->mem->read32(p + 8);
  ctx->cop2c[GTE_L31L32] = ctx->mem->read32(p + 12);
  ctx->cop2c[GTE_L33] = ctx->mem->read32(p + 16);
}

// SetColorMatrix(MATRIX *m): copy m->m[3][3] into cop2c[16..20] (LR1..LB3).
void hle_libgte_SetColorMatrix(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  ctx->cop2c[GTE_LR1LR2] = ctx->mem->read32(p + 0);
  ctx->cop2c[GTE_LR3LG1] = ctx->mem->read32(p + 4);
  ctx->cop2c[GTE_LG2LG3] = ctx->mem->read32(p + 8);
  ctx->cop2c[GTE_LB1LB2] = ctx->mem->read32(p + 12);
  ctx->cop2c[GTE_LB3] = ctx->mem->read32(p + 16);
}

// SetBackColor(rbk, gbk, bbk): cop2c[13..15] = RBK / GBK / BBK.
void hle_libgte_SetBackColor(recomp_context *ctx) {
  ctx->cop2c[GTE_RBK] = ctx->r[A0];
  ctx->cop2c[GTE_GBK] = ctx->r[A1];
  ctx->cop2c[GTE_BBK] = ctx->r[A2];
}

// SetFarColor(rfc, gfc, bfc): cop2c[21..23] = RFC / GFC / BFC.
void hle_libgte_SetFarColor(recomp_context *ctx) {
  ctx->cop2c[GTE_RFC] = ctx->r[A0];
  ctx->cop2c[GTE_GFC] = ctx->r[A1];
  ctx->cop2c[GTE_BFC] = ctx->r[A2];
}

// Group 1.C -- matrix builders

// RotMatrix(SVECTOR *r, MATRIX *m): build a YXZ Tait-Bryan rotation matrix
// (M = Rz x Rx x Ry) into m->m[3][3] from Euler angles in r->{vx,vy,vz}.
// PsyQ stores angles as 1/4096 of a full rotation; sin/cos produce 1.3.12.
//
// For r = (0,0,0) this yields the identity matrix -- the simplest test case.
// Returns m in $v0 (PsyQ contract).
void hle_libgte_RotMatrix(recomp_context *ctx) {
  uint32_t rPtr = ctx->r[A0];
  uint32_t mPtr = ctx->r[A1];

  int16_t rx = readShort(ctx, rPtr + 0);
  int16_t ry = readShort(ctx, rPtr + 2);
  int16_t rz = readShort(ctx, rPtr + 4);

  int16_t cx = psyq_cos(rx), sx = psyq_sin(rx);
  int16_t cy = psyq_cos(ry), sy = psyq_sin(ry);
  int16_t cz = psyq_cos(rz), sz = psyq_sin(rz);

  const int16_t Rx[3][3] = {
      {static_cast<int16_t>(ONE_12), 0, 0},
      {0, cx, static_cast<int16_t>(-sx)},
      {0, sx, cx},
  };
  const int16_t Ry[3][3] = {
      {cy, 0, sy},
      {0, static_cast<int16_t>(ONE_12), 0},
      {static_cast<int16_t>(-sy), 0, cy},
  };
  const int16_t Rz[3][3] = {
      {cz, static_cast<int16_t>(-sz), 0},
      {sz, cz, 0},
      {0, 0, static_cast<int16_t>(ONE_12)},
  };

  int16_t tmp[3][3], out[3][3];
  matMul3x3(Rx, Ry, tmp);
  matMul3x3(Rz, tmp, out);
  writeMatrix3x3(ctx, mPtr, out);

  ctx->r[V0] = mPtr;
}

// TransMatrix(MATRIX *m, VECTOR *v): m->t[i] = v->v[i] (3 longs).  Returns m.
void hle_libgte_TransMatrix(recomp_context *ctx) {
  uint32_t mPtr = ctx->r[A0];
  uint32_t vPtr = ctx->r[A1];
  for (int i = 0; i < 3; ++i)
    writeLong(ctx, mPtr + MATRIX_T_OFFSET + i * 4, readLong(ctx, vPtr + i * 4));
  ctx->r[V0] = mPtr;
}

// ScaleMatrix(MATRIX *m, VECTOR *v): scale row i of m by v[i] (1.3.12),
// i.e. M' = diag(vx, vy, vz) x M.  Returns m.
void hle_libgte_ScaleMatrix(recomp_context *ctx) {
  uint32_t mPtr = ctx->r[A0];
  uint32_t vPtr = ctx->r[A1];
  int32_t scale[3] = {
      readLong(ctx, vPtr + 0),
      readLong(ctx, vPtr + 4),
      readLong(ctx, vPtr + 8),
  };
  int16_t m[3][3];
  readMatrix3x3(ctx, mPtr, m);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      int32_t s = (static_cast<int32_t>(m[i][j]) * scale[i]) >> 12;
      s = std::clamp<int32_t>(s, -32768, 32767);
      m[i][j] = static_cast<int16_t>(s);
    }
  }
  writeMatrix3x3(ctx, mPtr, m);
  ctx->r[V0] = mPtr;
}

// MulMatrix(MATRIX *m0, MATRIX *m1): m0 = m0 x m1 (rotation parts only).
// Returns m0.  Translation field (m0->t) is left untouched.
void hle_libgte_MulMatrix(recomp_context *ctx) {
  uint32_t p0 = ctx->r[A0];
  uint32_t p1 = ctx->r[A1];
  int16_t a[3][3], b[3][3], out[3][3];
  readMatrix3x3(ctx, p0, a);
  readMatrix3x3(ctx, p1, b);
  matMul3x3(a, b, out);
  writeMatrix3x3(ctx, p0, out);
  ctx->r[V0] = p0;
}

// Group 1.C -- per-vertex transform wrappers

namespace {

// Load an SVECTOR at `p` into V0 (cop2d VXY0/VZ0).  Mirrors gte_ldv0:
// two lwc2 from offset 0 and 4 (vz + pad packed in the second word).
void ldv0(recomp_context *ctx, uint32_t p) {
  GTE::writeData(ctx, GTE_VXY0, ctx->mem->read32(p + 0));
  GTE::noteDataSource(ctx, GTE_VXY0, p + 0);
  GTE::writeData(ctx, GTE_VZ0, ctx->mem->read32(p + 4));
  GTE::noteDataSource(ctx, GTE_VZ0, p + 4);
}

} // namespace

// RotTrans(SVECTOR *v0, VECTOR *v1, long *flag):
//   v1 = rotation_matrix x v0 + translation
//   *flag = GTE FLAG register
// Drives the GTE backend with MVMVA(mx=RT, mv=V0, tv=TR, sf=1, lm=0).
void hle_libgte_RotTrans(recomp_context *ctx) {
  uint32_t vIn = ctx->r[A0];
  uint32_t vOut = ctx->r[A1];
  uint32_t flagP = ctx->r[A2];

  ldv0(ctx, vIn);
  GTE::MVMVA(ctx, /*mx=*/0, /*mv=*/0, /*tv=*/0, /*sf=*/true, /*lm=*/false);

  writeLong(ctx, vOut + 0, static_cast<int32_t>(ctx->cop2d[GTE_MAC1]));
  writeLong(ctx, vOut + 4, static_cast<int32_t>(ctx->cop2d[GTE_MAC2]));
  writeLong(ctx, vOut + 8, static_cast<int32_t>(ctx->cop2d[GTE_MAC3]));
  if (flagP)
    writeLong(ctx, flagP, static_cast<int32_t>(ctx->cop2c[GTE_FLAG]));
}

// RotTransPers(SVECTOR *v0, long *sxy, long *p, long *flag) -> long:
//   Apply rotate + translate + perspective-project (RTPS) to v0.
//   *sxy  = SXY2 (packed screen XY of the projected point)
//   *p    = MAC0 (depth-queueing factor: DQB + DQA x N/Z)
//   *flag = GTE FLAG register
//   return: SZ3 >> 2 (Z-sort key)
void hle_libgte_RotTransPers(recomp_context *ctx) {
  uint32_t vIn = ctx->r[A0];
  uint32_t sxyP = ctx->r[A1];
  uint32_t pP = ctx->r[A2];
  uint32_t flagP = ctx->r[A3];

  ldv0(ctx, vIn);
  GTE::RTPS(ctx, /*sf=*/true, /*lm=*/false);

  if (sxyP)
    writeProjectedSxy(ctx, sxyP, ctx->cop2d[GTE_SXY2]);
  if (pP)
    writeLong(ctx, pP, static_cast<int32_t>(ctx->cop2d[GTE_MAC0]));
  if (flagP)
    writeLong(ctx, flagP, static_cast<int32_t>(ctx->cop2c[GTE_FLAG]));

  ctx->r[V0] = ctx->cop2d[GTE_SZ3] >> 2;
}

// Registry

void psyq_register_libgte() {
  // 0.7
  psyq_register("libgte_SetGeomOffset", &hle_libgte_SetGeomOffset);
  psyq_register("libgte_SetGeomScreen", &hle_libgte_SetGeomScreen);
  psyq_register("libgte_SetDQA", &hle_libgte_SetDQA);

  // 1.C -- control-register loaders
  psyq_register("libgte_InitGeom", &hle_libgte_InitGeom);
  psyq_register("libgte_SetRotMatrix", &hle_libgte_SetRotMatrix);
  psyq_register("libgte_SetTransMatrix", &hle_libgte_SetTransMatrix);
  psyq_register("libgte_SetLightMatrix", &hle_libgte_SetLightMatrix);
  psyq_register("libgte_SetColorMatrix", &hle_libgte_SetColorMatrix);
  psyq_register("libgte_SetBackColor", &hle_libgte_SetBackColor);
  psyq_register("libgte_SetFarColor", &hle_libgte_SetFarColor);

  // 1.C -- matrix builders
  psyq_register("libgte_RotMatrix", &hle_libgte_RotMatrix);
  psyq_register("libgte_TransMatrix", &hle_libgte_TransMatrix);
  psyq_register("libgte_ScaleMatrix", &hle_libgte_ScaleMatrix);
  psyq_register("libgte_MulMatrix", &hle_libgte_MulMatrix);

  // 1.C -- per-vertex transform wrappers
  psyq_register("libgte_RotTrans", &hle_libgte_RotTrans);
  psyq_register("libgte_RotTransPers", &hle_libgte_RotTransPers);
}

} // namespace ps1::psyq
