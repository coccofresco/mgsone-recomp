#pragma once

#include "runtime/mips_fingerprint.h"

#include <cstdint>
#include <span>

namespace ps1 {
struct CPUContext;
}
struct recomp_context;

namespace ps1::mips {

// Compact, value-independent construction lineage for presentation dispatch.
// It computes the same versioned Merkle pair as the full value-flow DAG, but
// retains only live register/GTE/memory fingerprints. It never supplies a
// numeric transform, object identity or presentation factor.
struct GteProjectionConstructionLineage {
  uint64_t modelSourceFingerprint = 0;
  uint64_t modelProgramFingerprint = 0;
  bool complete = false;
};

// Exact, value-independent admission point proven by an offline full-DAG
// capture. The instruction is the earliest reachable model-construction
// operation; sourceRegister carries the physical guest source address which
// distinguishes the admitted construction path before that operation runs.
// Numeric geometry, screen position and sampled depth are never selectors.
struct ConstructionLineageSeed {
  uint32_t pc = 0;
  uint32_t rawInstruction = 0;
  uint32_t sourceAddress = 0;
  uint8_t sourceRegister = 0;

  friend bool operator==(const ConstructionLineageSeed &,
                         const ConstructionLineageSeed &) = default;
};

bool constructionLineageEnabled() noexcept;
void setConstructionLineageEnabled(bool enabled) noexcept;
void setConstructionLineageEnabledForTesting(bool enabled) noexcept;
void resetConstructionLineageForTesting() noexcept;

// Optional fail-closed admission gate for live presentation manifests. An
// empty filter tracks every transform. With a non-empty filter, compact model
// construction starts only after a GTE projection reports one of the exact
// categorical transform lineages requested by the manifest.
void setConstructionLineageTransformFilter(
    std::span<const uint64_t> transformLineages);
void setConstructionLineageSeedFilter(
    std::span<const ConstructionLineageSeed> seeds);
void beginConstructionLineageAdmissionSlice();
bool constructionLineageSeedProbeRequired() noexcept;
bool constructionLineageSeedAdmitted() noexcept;
bool tryAdmitConstructionLineageSeed(CPUContext *ctx, uint32_t pc,
                                     uint32_t rawInstruction) noexcept;
void refreshConstructionLineageTransformAdmission(CPUContext *ctx);
void noteConstructionLineageProjectionTransform(uint64_t transformLineage);

void beginConstructionLineageSlice(recomp_context *ctx, uint32_t entryPc);
void endConstructionLineageSlice(recomp_context *ctx);
void beforeConstructionLineageInstruction(recomp_context *ctx, uint32_t pc,
                                          uint32_t raw);
void afterConstructionLineageInstruction(recomp_context *ctx, uint32_t pc,
                                         uint32_t raw);
void noteConstructionLineageRegisterConstant(recomp_context *ctx, uint8_t reg,
                                             uint32_t value, uint32_t pc);
void noteConstructionLineageExternalCall(recomp_context *ctx,
                                         uint32_t targetPc);

GteProjectionConstructionLineage
captureGteProjectionConstructionLineage(CPUContext *ctx, uint8_t vectorIndex);

} // namespace ps1::mips
