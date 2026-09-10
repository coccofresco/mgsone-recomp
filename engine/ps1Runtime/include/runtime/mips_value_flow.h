#pragma once

#include <array>
#include <cstdint>

namespace ps1 {
struct CPUContext;
}
struct recomp_context;

namespace ps1::mips {

using MipsValueExpressionId = uint64_t;
inline constexpr uint32_t MIPS_MODEL_FINGERPRINT_SCHEMA_VERSION = 1;

// Stable numeric values are part of the JSONL schema consumed by
// tools/depth_family_trace.py. Add new operations at the end.
enum class MipsValueOperation : uint8_t {
  Boundary = 0,
  Constant = 1,
  MemoryLoad = 2,
  Add = 3,
  Subtract = 4,
  ShiftLeft = 5,
  ShiftRightLogical = 6,
  ShiftRightArithmetic = 7,
  BitAnd = 8,
  BitOr = 9,
  BitXor = 10,
  BitNor = 11,
  MultiplySignedLow = 12,
  MultiplySignedHigh = 13,
  MultiplyUnsignedLow = 14,
  MultiplyUnsignedHigh = 15,
  DivideSignedQuotient = 16,
  DivideSignedRemainder = 17,
  DivideUnsignedQuotient = 18,
  DivideUnsignedRemainder = 19,
  SetLessThanSigned = 20,
  SetLessThanUnsigned = 21,
  LoadWordLeft = 22,
  LoadWordRight = 23,
  GteDataWrite = 24,
  GteControlWrite = 25,
  GteCommandInput = 26,
  GteCommandResult = 27,
};

enum class MipsValueBoundary : uint8_t {
  InterpreterEntry = 1,
  UntrackedMutation = 2,
  ExternalCall = 3,
  ExistingGteState = 4,
};

struct MipsValueExpression {
  MipsValueExpressionId id = 0;
  MipsValueOperation operation = MipsValueOperation::Boundary;
  uint32_t pc = 0;
  uint32_t rawInstruction = 0;
  uint32_t value = 0;
  MipsValueExpressionId lhs = 0;
  MipsValueExpressionId rhs = 0;
  int64_t immediate = 0;
  uint32_t address = 0;
  uint8_t width = 0;
  bool signedValue = false;
  // Complete means closed over exact constants and memory loads. Boundary
  // leaves remain inspectable but can never certify a presentation gauge.
  bool complete = false;
  // Versioned Merkle fingerprints exclude sampled result values. The source
  // form retains physical memory addresses; the program form omits only those
  // dynamic addresses. They identify exact construction paths, not objects.
  uint64_t sourceFingerprint = 0;
  uint64_t programFingerprint = 0;
};

struct GteProjectionValueFlow {
  MipsValueExpressionId modelXy = 0;
  MipsValueExpressionId modelZ = 0;
  std::array<MipsValueExpressionId, 8> transform{};
  uint64_t modelSourceFingerprint = 0;
  uint64_t modelProgramFingerprint = 0;
  bool modelFingerprintComplete = false;
  bool complete = false;
};

// This instrumentation is disabled in ordinary runs. It is enabled only by
// PS1_TRACE_MIPS_VALUE_FLOW=1 together with PS1_TRACE_JSONL, or explicitly by
// tests. Optional inclusive PS1_TRACE_MIPS_VALUE_FLOW_FRAME_FROM/TO bounds
// keep discovery captures dormant until the scene under study;
// PS1_TRACE_ARM_AFTER_LOAD=1 is the frame-independent checkpoint gate. The
// interpreter uses a separate template specialization, so ordinary runs pay
// no per-instruction tracking cost.
bool valueFlowEnabled() noexcept;
void setValueFlowEnabledForTesting(bool enabled) noexcept;
void resetValueFlowForTesting() noexcept;

void beginValueFlowSlice(recomp_context *ctx, uint32_t entryPc);
void endValueFlowSlice(recomp_context *ctx);
void beforeValueFlowInstruction(recomp_context *ctx, uint32_t pc, uint32_t raw);
void afterValueFlowInstruction(recomp_context *ctx, uint32_t pc, uint32_t raw);
void noteValueFlowRegisterConstant(recomp_context *ctx, uint8_t reg,
                                   uint32_t value, uint32_t pc);
void noteValueFlowExternalCall(recomp_context *ctx, uint32_t targetPc);

// Returns current projection roots and, when emitTrace is true, emits every
// reachable expression once as mips.value_expr before gte.projection.
GteProjectionValueFlow captureGteProjectionValueFlow(CPUContext *ctx,
                                                     uint8_t vectorIndex,
                                                     bool emitTrace = true);

// Diagnostic/test inspection. IDs from an older slice deliberately expire.
bool inspectMipsValueExpression(MipsValueExpressionId id,
                                MipsValueExpression &expression) noexcept;

} // namespace ps1::mips
