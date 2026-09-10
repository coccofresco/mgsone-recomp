#pragma once

// ps1Recomp -- Instruction Emitter
// Translates decoded MIPS I instructions to C++ code

#include "mips_decoder.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ps1recomp {

// Function context for emission

/// Detected jump table for an indirect JR instruction.
/// When a JR $rx (rx != $ra) is preceded by the canonical
///   LUI + ADDIU + ADDU + LW + JR  pattern, we can replace the
///   JUMP_INDIRECT with a static switch/goto over the table entries.
struct JumpTableEntry {
  uint32_t jrInstrIdx;           ///< Index in instructions[] of the JR instruction
  std::vector<uint32_t> targets; ///< Jump target addresses read from the ELF data
};

/// Detect a computed in-function dispatch of the form
/// `target = constant_base +/- (index << shift); jr target`.
///
/// Hand-written PsyQ routines use this Duff's-device pattern instead of a
/// memory-backed jump table.  The returned addresses are restricted to the
/// current function and become ordinary C++ labels during emission.
std::vector<uint32_t>
detectArithmeticJumpTargets(const std::vector<uint32_t> &instructions,
                            size_t jrInstrIdx, uint32_t functionAddress);

/// Complete a recovered indirect-target set from intrinsic hand-written
/// assembly structure. This covers contiguous two-instruction branch veneers
/// and holes between sibling handler bodies which all tail-branch to the same
/// destination. Multiple independently known members are required before a
/// family is completed, so ordinary adjacent branches remain untouched.
std::vector<uint32_t> completeBranchVeneerTargets(
    const std::vector<uint32_t> &instructions, uint32_t functionAddress,
    const std::vector<uint32_t> &recoveredTargets);

/// Represents a function being recompiled
struct RecompFunction {
  std::string name;
  uint32_t address = 0;
  /// Callable entry when the emitted region starts earlier to include shared
  /// dispatch handlers. Zero means the same address as `address`.
  uint32_t entryAddress = 0;
  uint32_t size = 0;
  std::vector<uint32_t> instructions; // Raw 32-bit words

  /// Whether a given address within this function is a branch/jump target
  /// (used for label generation)
  std::vector<bool> isLabelTarget;

  /// Detected jump tables for indirect jumps within this function
  std::vector<JumpTableEntry> jumpTables;

  /// Data-driven indirect targets observed by more than one executable donor.
  /// These are entries inside a shared hand-written assembly region, not
  /// standalone ABI functions. They become local labels for every computed JR
  /// in the emitted region.
  std::vector<uint32_t> indirectTargets;

  /// PC+8 continuations of architectural call instructions. Savestate load
  /// can enter one of these after the old native C++ stack is unwound.
  std::vector<uint32_t> resumeTargets;

  /// Configured callable PCs inside this emitted assembly body. Dispatch uses
  /// a one-shot label selector while preserving the live native call stack.
  std::vector<uint32_t> entryAliases;
};

// Function call resolver

/// Callback to resolve a JAL target address to a function name. Returning an
/// empty string requests a runtime recomp_dispatch call for an unresolved
/// target instead of emitting a reference to a non-existent C++ symbol.
using FuncResolver = std::function<std::string(uint32_t address)>;

// Instruction Emitter

class InstructionEmitter {
public:
  /// Set the function call resolver (for JAL targets -> names)
  void setFuncResolver(FuncResolver resolver) {
    m_resolver = std::move(resolver);
  }

  /// Install exact, config-owned C++ replacements for individual guest
  /// instructions. Control-flow discovery still uses the decoded MIPS word;
  /// only its emitted operation changes. This keeps game-specific patches out
  /// of the generic decoder while preserving delay-slot and label handling.
  void setInstructionOverrides(std::map<uint32_t, std::string> overrides) {
    m_instructionOverrides = std::move(overrides);
  }

  /// Emit a single instruction as C++ code
  /// @param inst  Decoded instruction
  /// @param pc    Address of this instruction
  /// @return C++ code line(s) as string
  std::string emitInstruction(const Instruction &inst, uint32_t pc) const;

  /// Emit an entire function as C++ code
  /// @param func  Function context with instructions
  /// @return Complete C++ function body
  std::string emitFunction(const RecompFunction &func) const;

  // Individual emitters (public for testing)

  std::string emitALU(const Instruction &inst) const;
  std::string emitShift(const Instruction &inst) const;
  std::string emitMulDiv(const Instruction &inst) const;
  std::string emitLoad(const Instruction &inst) const;
  std::string emitStore(const Instruction &inst) const;
  std::string emitBranch(const Instruction &inst, uint32_t pc) const;
  std::string emitJump(const Instruction &inst, uint32_t pc) const;
  std::string emitCOP0(const Instruction &inst) const;
  std::string emitGTEMove(const Instruction &inst) const;
  std::string emitSystem(const Instruction &inst) const;

  // Helpers

  /// Format a GPR reference: ctx->r{N}
  static std::string reg(uint8_t r);

  /// Format a GPR write reference (discards writes to $zero)
  static std::string regWrite(uint8_t r);

  /// Generate an assignment to a register (discards if $zero)
  static std::string assignReg(uint8_t r, const std::string &expr);

  /// Format a label: L_{address:08X}
  static std::string label(uint32_t addr);

private:
  FuncResolver m_resolver;
  std::map<uint32_t, std::string> m_instructionOverrides;

  std::string defaultFuncName(uint32_t addr) const;
};

} // namespace ps1recomp
