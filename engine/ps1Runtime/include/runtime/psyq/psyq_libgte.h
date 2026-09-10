#pragma once
/**
 * @file psyq_libgte.h
 * @brief HLE for PsyQ libgte (GTE library).
 *
 * Two layers of helpers:
 *   - "Setter" wrappers (SetGeomOffset/SetRotMatrix/SetBackColor/...) that
 *     just move data from PS1 RAM into `cop2c[]`/`cop2d[]` register slots.
 *   - "Compute" wrappers (RotMatrix/MulMatrix/RotTrans/RotTransPers) that
 *     either build a MATRIX in PS1 RAM (matrix builders) or drive the
 *     existing GTE backend in `gte.cpp` for per-vertex transforms.
 *
 * PsyQ ABI:
 *   - Args in $a0..$a3 (then stack).  32-bit return in $v0.
 *   - MATRIX:  short m[3][3] (offset 0..17), pad (18..19), long t[3] (20..31).
 *   - SVECTOR: short vx, vy, vz, pad (8 bytes).
 *   - VECTOR:  long  vx, vy, vz, pad (16 bytes).
 *
 * Group 0.7 covered SetGeomOffset / SetGeomScreen / SetDQA.  Group 1.C below
 * adds matrix and per-vertex helpers required by Crash Bandicoot's boot path.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

// Group 0.7
void hle_libgte_SetGeomOffset(recomp_context *ctx);
void hle_libgte_SetGeomScreen(recomp_context *ctx);
void hle_libgte_SetDQA(recomp_context *ctx);

// Group 1.C -- control-register loaders
void hle_libgte_InitGeom(recomp_context *ctx);
void hle_libgte_SetRotMatrix(recomp_context *ctx);
void hle_libgte_SetTransMatrix(recomp_context *ctx);
void hle_libgte_SetLightMatrix(recomp_context *ctx);
void hle_libgte_SetColorMatrix(recomp_context *ctx);
void hle_libgte_SetBackColor(recomp_context *ctx);
void hle_libgte_SetFarColor(recomp_context *ctx);

// Group 1.C -- matrix builders (operate on PS1 RAM)
void hle_libgte_RotMatrix(recomp_context *ctx);
void hle_libgte_TransMatrix(recomp_context *ctx);
void hle_libgte_ScaleMatrix(recomp_context *ctx);
void hle_libgte_MulMatrix(recomp_context *ctx);

// Group 1.C -- per-vertex transform wrappers
void hle_libgte_RotTrans(recomp_context *ctx);
void hle_libgte_RotTransPers(recomp_context *ctx);

void psyq_register_libgte();

} // namespace ps1::psyq
