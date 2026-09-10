#pragma once

#include "runtime/mips_value_flow.h"

#include <cstdint>

namespace ps1::mips::fingerprint {

struct Pair {
  uint64_t source = 0;
  uint64_t program = 0;
  bool complete = false;

  friend bool operator==(const Pair &, const Pair &) = default;
};

inline constexpr uint64_t OFFSET = UINT64_C(14695981039346656037);
inline constexpr uint64_t PRIME = UINT64_C(1099511628211);
inline constexpr uint32_t SOURCE_EXPRESSION_DOMAIN = UINT32_C(0x4d564653);
inline constexpr uint32_t PROGRAM_EXPRESSION_DOMAIN = UINT32_C(0x4d564650);
inline constexpr uint32_t SOURCE_MODEL_DOMAIN = UINT32_C(0x4d564d53);
inline constexpr uint32_t PROGRAM_MODEL_DOMAIN = UINT32_C(0x4d564d50);

inline void appendByte(uint64_t &hash, uint8_t value) {
  hash ^= value;
  hash *= PRIME;
}

inline void append32(uint64_t &hash, uint32_t value) {
  for (uint8_t byte = 0; byte < 4; ++byte)
    appendByte(hash, static_cast<uint8_t>(value >> (byte * 8u)));
}

inline void append64(uint64_t &hash, uint64_t value) {
  for (uint8_t byte = 0; byte < 8; ++byte)
    appendByte(hash, static_cast<uint8_t>(value >> (byte * 8u)));
}

inline bool hasSemanticImmediate(MipsValueOperation operation) {
  switch (operation) {
  case MipsValueOperation::ShiftLeft:
  case MipsValueOperation::ShiftRightLogical:
  case MipsValueOperation::ShiftRightArithmetic:
  case MipsValueOperation::LoadWordLeft:
  case MipsValueOperation::LoadWordRight:
  case MipsValueOperation::GteDataWrite:
  case MipsValueOperation::GteControlWrite:
  case MipsValueOperation::GteCommandInput:
  case MipsValueOperation::GteCommandResult:
    return true;
  default:
    return false;
  }
}

inline bool isShift(MipsValueOperation operation) {
  return operation == MipsValueOperation::ShiftLeft ||
         operation == MipsValueOperation::ShiftRightLogical ||
         operation == MipsValueOperation::ShiftRightArithmetic;
}

inline uint32_t physicalAddress(uint32_t address) {
  return address & 0x1fffffffu;
}

inline uint64_t expressionOne(bool includeMemoryAddress,
                              MipsValueOperation operation, uint32_t pc,
                              uint32_t rawInstruction, uint32_t value,
                              bool lhsPresent, uint64_t lhs, bool rhsPresent,
                              uint64_t rhs, int64_t immediate, uint32_t address,
                              uint8_t width, bool signedValue) {
  uint64_t hash = OFFSET;
  append32(hash, includeMemoryAddress ? SOURCE_EXPRESSION_DOMAIN
                                      : PROGRAM_EXPRESSION_DOMAIN);
  append32(hash, MIPS_MODEL_FINGERPRINT_SCHEMA_VERSION);
  appendByte(hash, static_cast<uint8_t>(operation));
  append32(hash, pc);
  append32(hash, rawInstruction);

  if (operation == MipsValueOperation::Constant) {
    appendByte(hash, 1u);
    append32(hash, value);
  } else if (operation == MipsValueOperation::MemoryLoad) {
    appendByte(hash, 2u);
    appendByte(hash, includeMemoryAddress ? 1u : 0u);
    if (includeMemoryAddress)
      append32(hash, physicalAddress(address));
    appendByte(hash, width);
    appendByte(hash, signedValue ? 1u : 0u);
  } else if (hasSemanticImmediate(operation)) {
    appendByte(hash, 3u);
    const bool retainImmediate = !(isShift(operation) && rhsPresent);
    appendByte(hash, retainImmediate ? 1u : 0u);
    if (retainImmediate)
      append64(hash, static_cast<uint64_t>(immediate));
  } else {
    appendByte(hash, 0u);
  }

  appendByte(hash, lhsPresent ? 1u : 0u);
  if (lhsPresent)
    append64(hash, lhs);
  appendByte(hash, rhsPresent ? 1u : 0u);
  if (rhsPresent)
    append64(hash, rhs);
  return hash;
}

inline Pair expression(MipsValueOperation operation, uint32_t pc,
                       uint32_t rawInstruction, uint32_t value, bool lhsPresent,
                       Pair lhs, bool rhsPresent, Pair rhs, int64_t immediate,
                       uint32_t address, uint8_t width, bool signedValue,
                       bool complete) {
  if (!complete)
    return {};
  Pair result;
  result.complete = true;
  result.source = expressionOne(true, operation, pc, rawInstruction, value,
                                lhsPresent, lhs.source, rhsPresent, rhs.source,
                                immediate, address, width, signedValue);
  result.program = expressionOne(
      false, operation, pc, rawInstruction, value, lhsPresent, lhs.program,
      rhsPresent, rhs.program, immediate, address, width, signedValue);
  return result;
}

inline Pair combineModel(Pair xy, Pair z) {
  if (!xy.complete || !z.complete)
    return {};
  Pair result;
  result.complete = true;
  result.source = OFFSET;
  append32(result.source, SOURCE_MODEL_DOMAIN);
  append32(result.source, MIPS_MODEL_FINGERPRINT_SCHEMA_VERSION);
  append64(result.source, xy.source);
  append64(result.source, z.source);
  result.program = OFFSET;
  append32(result.program, PROGRAM_MODEL_DOMAIN);
  append32(result.program, MIPS_MODEL_FINGERPRINT_SCHEMA_VERSION);
  append64(result.program, xy.program);
  append64(result.program, z.program);
  return result;
}

} // namespace ps1::mips::fingerprint
