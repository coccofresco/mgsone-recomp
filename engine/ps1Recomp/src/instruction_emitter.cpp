// ps1Recomp -- Instruction Emitter Implementation
// Translates decoded MIPS I instructions to C++ code using runtime macros

#include "ps1recomp/instruction_emitter.h"
#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <set>

namespace ps1recomp {

std::vector<uint32_t>
detectArithmeticJumpTargets(const std::vector<uint32_t> &instructions,
                            size_t jrInstrIdx, uint32_t functionAddress) {
  if (jrInstrIdx >= instructions.size())
    return {};

  const Instruction jr = MipsDecoder::decode(instructions[jrInstrIdx]);
  if (jr.id != InstrId::JR || jr.rs == 31)
    return {};

  // Find `addu/subu $target, $target, $scaled` immediately upstream.
  int combineIdx = -1;
  bool subtract = false;
  uint8_t scaledReg = 0;
  for (int j = static_cast<int>(jrInstrIdx) - 1;
       j >= 0 && j >= static_cast<int>(jrInstrIdx) - 8; --j) {
    const Instruction inst = MipsDecoder::decode(instructions[j]);
    if ((inst.id == InstrId::ADDU || inst.id == InstrId::SUBU) &&
        inst.rd == jr.rs && inst.rs == jr.rs) {
      combineIdx = j;
      subtract = inst.id == InstrId::SUBU;
      scaledReg = inst.rt;
      break;
    }
  }
  if (combineIdx < 0)
    return {};

  // Recover the first `$scaled = $index << shift` assignment.  Most PsyQ
  // Duff devices use a four-byte stride, but the byte-copy tail in Crash 3
  // selects one of five inline handlers spaced 0x20 bytes apart.  Later
  // self-shifts may further shape an older Duff layout, so only accept an
  // assignment whose input is not the scaled register itself.
  int scaleIdx = -1;
  uint8_t indexReg = 0;
  uint8_t scaleShift = 0;
  for (int j = combineIdx - 1; j >= 0 && j >= combineIdx - 8; --j) {
    const Instruction inst = MipsDecoder::decode(instructions[j]);
    if (inst.id == InstrId::SLL && inst.rd == scaledReg &&
        inst.rt != scaledReg && inst.shamt < 31) {
      scaleIdx = j;
      indexReg = inst.rt;
      scaleShift = inst.shamt;
      break;
    }
  }
  if (scaleIdx < 0)
    return {};

  // Follow arithmetic performed on the scaled temporary before it is
  // combined with the constant base.  Alignment-specialized PsyQ copies use
  // `(index << 2) + index`, followed by `<< 1`, i.e. a ten-byte stride.
  int64_t scaleMultiplier = INT64_C(1) << scaleShift;
  for (int j = scaleIdx + 1; j < combineIdx; ++j) {
    const Instruction inst = MipsDecoder::decode(instructions[j]);
    if (inst.id == InstrId::SLL && inst.rd == scaledReg) {
      if (inst.rt != scaledReg || inst.shamt >= 31)
        return {};
      scaleMultiplier <<= inst.shamt;
    } else if ((inst.id == InstrId::ADDU || inst.id == InstrId::ADD) &&
               inst.rd == scaledReg) {
      if ((inst.rs == scaledReg && inst.rt == indexReg) ||
          (inst.rs == indexReg && inst.rt == scaledReg)) {
        ++scaleMultiplier;
      } else if (inst.rs == scaledReg && inst.rt == scaledReg) {
        scaleMultiplier *= 2;
      } else {
        return {};
      }
    } else if ((inst.id == InstrId::SUBU || inst.id == InstrId::SUB) &&
               inst.rd == scaledReg) {
      if (inst.rs == scaledReg && inst.rt == indexReg)
        --scaleMultiplier;
      else if (inst.rs == indexReg && inst.rt == scaledReg)
        scaleMultiplier = 1 - scaleMultiplier;
      else
        return {};
    }
    if (scaleMultiplier <= 0 || scaleMultiplier > (INT64_C(1) << 30))
      return {};
  }

  // Recover the constant base from LUI + optional ADDIU/ADDI.
  bool haveLui = false;
  uint32_t base = 0;
  int16_t low = 0;
  for (int j = combineIdx - 1; j >= 0 && j >= combineIdx - 16; --j) {
    const Instruction inst = MipsDecoder::decode(instructions[j]);
    if ((inst.id == InstrId::ADDIU || inst.id == InstrId::ADDI) &&
        inst.rt == jr.rs && inst.rs == jr.rs) {
      low = inst.imm16;
    } else if (inst.id == InstrId::LUI && inst.rt == jr.rs) {
      base = (static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16)) << 16) +
             static_cast<uint32_t>(static_cast<int32_t>(low));
      haveLui = true;
      break;
    }
  }
  if (!haveLui)
    return {};

  // A nearby SLTI(U) bounds the index and therefore the set of labels.  The
  // test can live at the head of a sizeable alignment-specialized copy
  // routine, well before the final indirect tail.
  uint32_t bound = 0;
  for (int j = scaleIdx - 1; j >= 0 && j >= scaleIdx - 128; --j) {
    const Instruction inst = MipsDecoder::decode(instructions[j]);
    if ((inst.id == InstrId::SLTIU || inst.id == InstrId::SLTI) &&
        inst.rs == indexReg) {
      bound = static_cast<uint16_t>(inst.imm16);
      break;
    }
  }
  if (bound == 0 || bound > 4096)
    return {};

  const uint64_t functionEnd =
      static_cast<uint64_t>(functionAddress) + instructions.size() * 4ull;
  std::vector<uint32_t> targets;
  const int64_t stride = scaleMultiplier;
  for (uint32_t i = 0; i < bound; ++i) {
    const int64_t target = static_cast<int64_t>(base) +
                           (subtract ? -static_cast<int64_t>(i) * stride
                                     : static_cast<int64_t>(i) * stride);
    // A MIPS instruction target must be word-aligned.  A ten-byte Duff
    // stride therefore only yields callable labels for even indices.
    if ((target & 3) == 0 && target >= functionAddress &&
        static_cast<uint64_t>(target) < functionEnd)
      targets.push_back(static_cast<uint32_t>(target));
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

std::vector<uint32_t> completeBranchVeneerTargets(
    const std::vector<uint32_t> &instructions, uint32_t functionAddress,
    const std::vector<uint32_t> &recoveredTargets) {
  struct VeneerShape {
    InstrId id = InstrId::INVALID;
    uint32_t destination = 0;

    bool operator<(const VeneerShape &other) const {
      if (id != other.id)
        return id < other.id;
      return destination < other.destination;
    }
  };

  const uint64_t functionEnd =
      static_cast<uint64_t>(functionAddress) + instructions.size() * 4ull;
  const auto veneerShapeAt = [&](uint32_t address,
                                 VeneerShape &shape) -> bool {
    if (address < functionAddress || address + 8ull > functionEnd ||
        ((address - functionAddress) & 3u) != 0)
      return false;
    const size_t index = (address - functionAddress) / 4;
    const Instruction branch = MipsDecoder::decode(instructions[index]);

    // These are the unconditional branch encodings emitted by assemblers for
    // short local veneers. Link variants are included: writing $ra is part of
    // the veneer semantics and does not make the destination conditional.
    const bool unconditional =
        (branch.id == InstrId::BEQ && branch.rs == branch.rt) ||
        ((branch.id == InstrId::BGEZ || branch.id == InstrId::BGEZAL) &&
         branch.rs == 0) ||
        (branch.id == InstrId::BLEZ && branch.rs == 0);
    if (!unconditional)
      return false;

    shape.id = branch.id;
    shape.destination = branch.branchTarget(address);
    return true;
  };
  const auto registerMoveDestinationAt = [&](uint32_t address,
                                             uint8_t &destination) -> bool {
    if (address < functionAddress || address + 4ull > functionEnd ||
        ((address - functionAddress) & 3u) != 0)
      return false;
    const Instruction move = MipsDecoder::decode(
        instructions[(address - functionAddress) / 4]);
    if ((move.id != InstrId::ADDU && move.id != InstrId::OR) || move.rd == 0 ||
        (move.rs != 0 && move.rt != 0))
      return false;
    destination = move.rd;
    return true;
  };

  std::vector<uint32_t> completed = recoveredTargets;
  std::sort(completed.begin(), completed.end());
  completed.erase(std::unique(completed.begin(), completed.end()),
                  completed.end());

  std::map<VeneerShape, std::vector<uint32_t>> families;
  for (uint32_t target : completed) {
    VeneerShape shape;
    if (veneerShapeAt(target, shape))
      families[shape].push_back(target);
  }

  for (auto &[shape, members] : families) {
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());

    // Two independently recovered adjacent veneers are the intrinsic proof
    // that this is a family rather than two unrelated nearby branches.
    bool hasAdjacentPair = false;
    for (size_t i = 1; i < members.size(); ++i) {
      if (members[i] - members[i - 1] == 8u) {
        hasAdjacentPair = true;
        break;
      }
    }
    if (!hasAdjacentPair)
      continue;

    uint32_t first = members.front();
    uint32_t last = members.back();
    for (;;) {
      if (first < functionAddress + 8u)
        break;
      VeneerShape candidate;
      if (!veneerShapeAt(first - 8u, candidate) || candidate.id != shape.id ||
          candidate.destination != shape.destination)
        break;
      first -= 8u;
      completed.push_back(first);
    }
    for (;;) {
      if (static_cast<uint64_t>(last) + 8ull >= functionEnd)
        break;
      VeneerShape candidate;
      if (!veneerShapeAt(last + 8u, candidate) || candidate.id != shape.id ||
          candidate.destination != shape.destination)
        break;
      last += 8u;
      completed.push_back(last);
    }

    // A common hand-written layout omits the final branch veneer: its last
    // register move sits immediately before the common destination and falls
    // through to it. Require every branch delay slot plus that terminal word
    // to be a register copy into the same destination register. This derives
    // the extra callable entry from the assembly shape without accepting an
    // arbitrary instruction which merely happens to precede a branch target.
    const uint32_t terminal = last + 8u;
    if (static_cast<uint64_t>(terminal) + 4ull == shape.destination &&
        static_cast<uint64_t>(terminal) + 4ull <= functionEnd) {
      uint8_t moveDestination = 0;
      bool uniformMoves =
          registerMoveDestinationAt(first + 4u, moveDestination);
      for (uint32_t veneer = first + 8u;
           uniformMoves && veneer <= last; veneer += 8u) {
        uint8_t candidateDestination = 0;
        uniformMoves = registerMoveDestinationAt(veneer + 4u,
                                                  candidateDestination) &&
                       candidateDestination == moveDestination;
      }
      uint8_t terminalDestination = 0;
      if (uniformMoves &&
          registerMoveDestinationAt(terminal, terminalDestination) &&
          terminalDestination == moveDestination) {
        completed.push_back(terminal);
      }
    }
  }

  // Some hand-written jump tables are arrays of adjacent unconditional
  // branch veneers whose destinations intentionally differ.  Grouping by
  // destination cannot complete those arrays: only the first few selector
  // values may have been recovered even though the remaining entries have
  // the same intrinsic two-word layout.  Two recovered entries bracketing an
  // uninterrupted run, the same unconditional branch encoding and identical
  // delay-slot instructions establish the array shape without assuming dead
  // fallthrough is code.
  // Once established, extend only across the contiguous run of that exact
  // shape.  Crash 3's hand-written geometry dispatcher is one example, but
  // the rule is executable-agnostic.
  const auto matchingDelayVeneerAt = [&](uint32_t address, InstrId expectedId,
                                         uint32_t expectedDelay) -> bool {
    VeneerShape shape;
    if (!veneerShapeAt(address, shape) || shape.id != expectedId)
      return false;
    const size_t delayIndex = (address - functionAddress) / 4u + 1u;
    return delayIndex < instructions.size() &&
           instructions[delayIndex] == expectedDelay;
  };

  std::vector<uint32_t> veneerSeeds = completed;
  for (size_t index = 0; index < instructions.size(); ++index) {
    const uint32_t address =
        functionAddress + static_cast<uint32_t>(index * 4u);
    const Instruction instruction = MipsDecoder::decode(instructions[index]);
    uint32_t target = 0;
    bool hasDirectTarget = false;
    if (instruction.category == InstrCategory::Branch) {
      target = instruction.branchTarget(address);
      hasDirectTarget = true;
    } else if (instruction.category == InstrCategory::Jump &&
               instruction.id == InstrId::J) {
      target = instruction.jumpTarget(address);
      hasDirectTarget = true;
    }
    if (hasDirectTarget && target >= functionAddress && target < functionEnd)
      veneerSeeds.push_back(target);
  }
  std::sort(veneerSeeds.begin(), veneerSeeds.end());
  veneerSeeds.erase(std::unique(veneerSeeds.begin(), veneerSeeds.end()),
                    veneerSeeds.end());

  for (size_t index = 1; index < veneerSeeds.size(); ++index) {
    const uint32_t left = veneerSeeds[index - 1u];
    const uint32_t right = veneerSeeds[index];
    if (right <= left || ((right - left) & 7u) != 0u)
      continue;
    VeneerShape leftShape;
    VeneerShape rightShape;
    const size_t leftDelayIndex = (left - functionAddress) / 4u + 1u;
    if (leftDelayIndex >= instructions.size())
      continue;
    const uint32_t delayWord = instructions[leftDelayIndex];
    if (!veneerShapeAt(left, leftShape) ||
        !veneerShapeAt(right, rightShape) || leftShape.id != rightShape.id ||
        !matchingDelayVeneerAt(left, leftShape.id, delayWord) ||
        !matchingDelayVeneerAt(right, leftShape.id, delayWord)) {
      continue;
    }

    bool contiguousArray = true;
    for (uint32_t candidate = left; candidate <= right; candidate += 8u) {
      if (!matchingDelayVeneerAt(candidate, leftShape.id, delayWord)) {
        contiguousArray = false;
        break;
      }
    }
    if (!contiguousArray)
      continue;

    for (uint32_t candidate = left; candidate <= right; candidate += 8u)
      completed.push_back(candidate);
    uint32_t first = left;
    while (first >= functionAddress + 8u &&
           matchingDelayVeneerAt(first - 8u, leftShape.id, delayWord)) {
      first -= 8u;
      completed.push_back(first);
    }
    uint32_t last = right;
    while (static_cast<uint64_t>(last) + 8ull < functionEnd &&
           matchingDelayVeneerAt(last + 8u, leftShape.id, delayWord)) {
      last += 8u;
      completed.push_back(last);
    }
  }

  // Hand-written dispatchers also place sibling handler bodies back-to-back,
  // with each body ending in the same unconditional branch.  The instruction
  // after that branch's delay slot is then the next sibling entry, even though
  // ordinary CFG reachability quite correctly considers it dead fallthrough.
  // Recover only holes bracketed by two independently known sibling entries:
  // one may be an ordinary direct branch target and the other a jump-table or
  // donor-recovered target.  This filled interval is intrinsic to the assembly
  // layout and does not assume that every post-branch word is executable.
  std::set<uint32_t> knownEntries(completed.begin(), completed.end());
  std::map<uint32_t, std::vector<uint32_t>> siblingEntriesByDestination;
  for (size_t index = 0; index < instructions.size(); ++index) {
    const uint32_t address =
        functionAddress + static_cast<uint32_t>(index * 4u);
    const Instruction instruction = MipsDecoder::decode(instructions[index]);
    if (instruction.category == InstrCategory::Branch) {
      const uint32_t target = instruction.branchTarget(address);
      if (target >= functionAddress && target < functionEnd)
        knownEntries.insert(target);
    } else if (instruction.category == InstrCategory::Jump &&
               instruction.id == InstrId::J) {
      const uint32_t target = instruction.jumpTarget(address);
      if (target >= functionAddress && target < functionEnd)
        knownEntries.insert(target);
    }

    VeneerShape shape;
    if (veneerShapeAt(address, shape) &&
        static_cast<uint64_t>(address) + 8ull < functionEnd) {
      siblingEntriesByDestination[shape.destination].push_back(address + 8u);
    }
  }

  for (auto &[destination, siblings] : siblingEntriesByDestination) {
    (void)destination;
    std::sort(siblings.begin(), siblings.end());
    siblings.erase(std::unique(siblings.begin(), siblings.end()),
                   siblings.end());
    if (siblings.size() < 3u)
      continue;

    size_t firstKnown = siblings.size();
    size_t lastKnown = 0u;
    size_t knownCount = 0u;
    for (size_t index = 0; index < siblings.size(); ++index) {
      if (!knownEntries.contains(siblings[index]))
        continue;
      firstKnown = std::min(firstKnown, index);
      lastKnown = std::max(lastKnown, index);
      ++knownCount;
    }
    if (knownCount < 2u || firstKnown >= lastKnown)
      continue;
    for (size_t index = firstKnown; index <= lastKnown; ++index)
      completed.push_back(siblings[index]);
  }

  std::sort(completed.begin(), completed.end());
  completed.erase(std::unique(completed.begin(), completed.end()),
                  completed.end());
  return completed;
}

// Register Reference

std::string InstructionEmitter::reg(uint8_t r) {
  if (r == 0)
    return "(int32_t)0"; // $zero is always 0
  return fmt::format("ctx->r{}", r);
}

// Returns an expression suitable as a write destination.
// Writes to $zero are discarded in MIPS, so we wrap in (void).
std::string InstructionEmitter::regWrite(uint8_t r) {
  if (r == 0)
    return "/* $zero discard */"; // Will be used as: /* $zero discard */ = expr
                                  // -> becomes a comment
  return fmt::format("ctx->r{}", r);
}

// Wraps an assignment: if dest is $zero, discard the result
std::string InstructionEmitter::assignReg(uint8_t r, const std::string &expr) {
  if (r == 0)
    return fmt::format("(void)({}); /* $zero */", expr);
  return fmt::format("pgxp_invalidate_gpr(ctx, {}); ctx->r{} = {};", r, r,
                     expr);
}

std::string InstructionEmitter::label(uint32_t addr) {
  return fmt::format("L_{:08X}", addr);
}

std::string InstructionEmitter::defaultFuncName(uint32_t addr) const {
  if (m_resolver)
    return m_resolver(addr);
  return fmt::format("func_{:08X}", addr);
}

// Signed/Unsigned cast helpers

static std::string s32(const std::string &val) {
  return fmt::format("(int32_t)({})", val);
}

static std::string u32(const std::string &val) {
  return fmt::format("(uint32_t)({})", val);
}

// Delay Slot Conflict Detection
// Returns the GPR index written by an instruction, or -1 if none.
// Used to detect when a delay slot instruction modifies a register
// that the preceding branch/jump reads for its condition/target.

static int getDestGPR(const Instruction &inst) {
  if (inst.isNOP() || !inst.isValid())
    return -1;

  switch (inst.category) {
  case InstrCategory::ALU:
    // R-type (ADD..SLTU, SLL..SRAV) writes rd; I-type writes rt
    if (inst.id <= InstrId::SLTU ||
        (inst.id >= InstrId::SLL && inst.id <= InstrId::SRAV))
      return inst.rd;
    return inst.rt;
  case InstrCategory::Memory:
    return inst.isLoad() ? static_cast<int>(inst.rt) : -1;
  case InstrCategory::MulDiv:
    if (inst.id == InstrId::MFHI || inst.id == InstrId::MFLO)
      return inst.rd;
    return -1; // MULT, DIV etc. write HI/LO, not GPR
  case InstrCategory::COP0:
    if (inst.id == InstrId::MFC0)
      return inst.rt;
    return -1;
  case InstrCategory::GTE:
    if (inst.id == InstrId::MFC2 || inst.id == InstrId::CFC2)
      return inst.rt;
    return -1;
  case InstrCategory::Jump:
    if (inst.id == InstrId::JAL)
      return 31;
    if (inst.id == InstrId::JALR)
      return inst.rd;
    return -1;
  default:
    return -1;
  }
}

// Replace all occurrences of `from` with `to` in `str`, but only when the
// match is NOT followed by an ASCII digit (avoids replacing "ctx->r1" inside
// "ctx->r12").
static void replaceRegRef(std::string &str, const std::string &from,
                          const std::string &to) {
  size_t pos = 0;
  while ((pos = str.find(from, pos)) != std::string::npos) {
    size_t end = pos + from.size();
    // Make sure we're not matching a prefix of a longer register name
    if (end < str.size() && str[end] >= '0' && str[end] <= '9') {
      pos = end;
      continue;
    }
    str.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// ALU Emitter

std::string InstructionEmitter::emitALU(const Instruction &inst) const {
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);

  switch (inst.id) {
  // R-type ALU -- emit as unsigned to avoid C++ signed-overflow UB.
  // MIPS ADDU/SUBU are explicitly modular; PSY-Q ADD/SUB also rely on
  // modular wrap in practice (no overflow trap delivered to PSX user code).
  // Casting through uint32_t makes the wraparound well-defined; the trailing
  // `(int32_t)` cast is a no-op bit reinterpret on a 2's-complement target.
  case InstrId::ADD:
  case InstrId::ADDU:
    if (inst.rs == 0)
      return fmt::format("pgxp_move_gpr(ctx, {}, {}, {});", inst.rd, inst.rt,
                         rt);
    if (inst.rt == 0)
      return fmt::format("pgxp_move_gpr(ctx, {}, {}, {});", inst.rd, inst.rs,
                         rs);
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} + {})", u32(rs), u32(rt)));
  case InstrId::SUB:
  case InstrId::SUBU:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} - {})", u32(rs), u32(rt)));
  case InstrId::AND:
    return fmt::format("pgxp_and(ctx, {}, {}, {});", inst.rd, inst.rs, inst.rt);
  case InstrId::OR:
    if (inst.rs == 0)
      return fmt::format("pgxp_move_gpr(ctx, {}, {}, {});", inst.rd, inst.rt,
                         rt);
    if (inst.rt == 0)
      return fmt::format("pgxp_move_gpr(ctx, {}, {}, {});", inst.rd, inst.rs,
                         rs);
    return fmt::format("pgxp_or(ctx, {}, {}, {});", inst.rd, inst.rs, inst.rt);
  case InstrId::XOR:
    return assignReg(inst.rd, fmt::format("{} ^ {}", rs, rt));
  case InstrId::NOR:
    return assignReg(inst.rd, fmt::format("~({} | {})", rs, rt));
  case InstrId::SLT:
    return assignReg(inst.rd,
                     fmt::format("({} < {}) ? 1 : 0", s32(rs), s32(rt)));
  case InstrId::SLTU:
    return assignReg(inst.rd,
                     fmt::format("({} < {}) ? 1 : 0", u32(rs), u32(rt)));

  // I-type ALU -- same modular-wrap reasoning as ADD/ADDU above.
  // Cast `inst.imm16` (signed 16-bit) to int32_t first so the sign-extension
  // happens before the unsigned conversion; the immediate is treated as a
  // signed offset by every PSY-Q caller that uses ADDIU.
  case InstrId::ADDI:
  case InstrId::ADDIU:
    return fmt::format("pgxp_addiu(ctx, {}, {}, {});", inst.rt, inst.rs,
                       static_cast<int32_t>(inst.imm16));
  case InstrId::ANDI:
    return fmt::format("pgxp_andi(ctx, {}, {}, 0x{:04X});", inst.rt, inst.rs,
                       static_cast<uint16_t>(inst.imm16));
  case InstrId::ORI:
    return assignReg(inst.rt, fmt::format("{} | 0x{:04X}", reg(inst.rs),
                                          static_cast<uint16_t>(inst.imm16)));
  case InstrId::XORI:
    return assignReg(inst.rt, fmt::format("{} ^ 0x{:04X}", reg(inst.rs),
                                          static_cast<uint16_t>(inst.imm16)));
  case InstrId::SLTI:
    return assignReg(inst.rt, fmt::format("({} < {}) ? 1 : 0",
                                          s32(reg(inst.rs)), inst.imm16));
  case InstrId::SLTIU:
    return assignReg(
        inst.rt,
        fmt::format("({} < {}) ? 1 : 0", u32(reg(inst.rs)),
                    static_cast<uint32_t>(static_cast<int32_t>(inst.imm16))));
  case InstrId::LUI:
    return assignReg(inst.rt, fmt::format("0x{:04X}0000",
                                          static_cast<uint16_t>(inst.imm16)));

  default:
    return fmt::format("// UNKNOWN ALU: {}", MipsDecoder::instrName(inst.id));
  }
}

// Shift Emitter

std::string InstructionEmitter::emitShift(const Instruction &inst) const {
  auto rt = reg(inst.rt);
  auto rs = reg(inst.rs);

  switch (inst.id) {
  case InstrId::SLL:
    if (inst.shamt == 0)
      return fmt::format("pgxp_move_gpr(ctx, {}, {}, {});", inst.rd, inst.rt,
                         rt);
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} << {})", u32(rt), inst.shamt));
  case InstrId::SRL:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} >> {})", u32(rt), inst.shamt));
  case InstrId::SRA:
    return assignReg(inst.rd, fmt::format("{} >> {}", s32(rt), inst.shamt));
  case InstrId::SLLV:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} << ({} & 31))", u32(rt), rs));
  case InstrId::SRLV:
    return assignReg(inst.rd,
                     fmt::format("(int32_t)({} >> ({} & 31))", u32(rt), rs));
  case InstrId::SRAV:
    return assignReg(inst.rd, fmt::format("{} >> ({} & 31)", s32(rt), rs));
  default:
    return fmt::format("// UNKNOWN SHIFT: {}", MipsDecoder::instrName(inst.id));
  }
}

// Multiply / Divide Emitter

std::string InstructionEmitter::emitMulDiv(const Instruction &inst) const {
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);
  auto rd = reg(inst.rd);

  switch (inst.id) {
  case InstrId::MULT:
    return fmt::format(
        "{{ int64_t result = (int64_t){} * (int64_t){}; "
        "ctx->lo = (int32_t)result; ctx->hi = (int32_t)(result >> 32); }}",
        s32(rs), s32(rt));
  case InstrId::MULTU:
    return fmt::format(
        "{{ uint64_t result = (uint64_t){} * (uint64_t){}; "
        "ctx->lo = (int32_t)result; ctx->hi = (int32_t)(result >> 32); }}",
        u32(rs), u32(rt));
  case InstrId::DIV:
    return fmt::format(
        "if ({} != 0) {{ ctx->lo = {} / {}; ctx->hi = {} % {}; }}", s32(rt),
        s32(rs), s32(rt), s32(rs), s32(rt));
  case InstrId::DIVU:
    return fmt::format("if ({} != 0) {{ ctx->lo = (int32_t)({} / {}); "
                       "ctx->hi = (int32_t)({} % {}); }}",
                       u32(rt), u32(rs), u32(rt), u32(rs), u32(rt));
  case InstrId::MFHI:
    return assignReg(inst.rd, "ctx->hi");
  case InstrId::MFLO:
    return assignReg(inst.rd, "ctx->lo");
  case InstrId::MTHI:
    return fmt::format("ctx->hi = {};", rs);
  case InstrId::MTLO:
    return fmt::format("ctx->lo = {};", rs);
  default:
    return fmt::format("// UNKNOWN MULDIV: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// Load Emitter

std::string InstructionEmitter::emitLoad(const Instruction &inst) const {
  auto base = reg(inst.rs);
  auto offset = inst.imm16;

  switch (inst.id) {
  case InstrId::LB:
    return assignReg(inst.rt, fmt::format("(int8_t)MEM_READ8(ctx, {} + {})",
                                          s32(base), offset));
  case InstrId::LBU:
    return assignReg(inst.rt, fmt::format("(uint8_t)MEM_READ8(ctx, {} + {})",
                                          s32(base), offset));
  case InstrId::LH:
    return assignReg(inst.rt, fmt::format("(int16_t)MEM_READ16(ctx, {} + {})",
                                          s32(base), offset));
  case InstrId::LHU:
    return assignReg(inst.rt, fmt::format("(uint16_t)MEM_READ16(ctx, {} + {})",
                                          s32(base), offset));
  case InstrId::LW:
    return assignReg(inst.rt, fmt::format("pgxp_load_word(ctx, {}, {} + {})",
                                          inst.rt, s32(base), offset));
  case InstrId::LWL:
    return assignReg(inst.rt, fmt::format("DO_LWL(ctx, {}, {} + {})",
                                          reg(inst.rt), s32(base), offset));
  case InstrId::LWR:
    return assignReg(inst.rt, fmt::format("DO_LWR(ctx, {}, {} + {})",
                                          reg(inst.rt), s32(base), offset));
  default:
    return fmt::format("// UNKNOWN LOAD: {}", MipsDecoder::instrName(inst.id));
  }
}

// Store Emitter

std::string InstructionEmitter::emitStore(const Instruction &inst) const {
  auto rt = reg(inst.rt);
  auto base = reg(inst.rs);
  auto offset = inst.imm16;

  switch (inst.id) {
  case InstrId::SB:
    return fmt::format("MEM_WRITE8(ctx, {} + {}, {});", s32(base), offset, rt);
  case InstrId::SH:
    return fmt::format("MEM_WRITE16(ctx, {} + {}, {});", s32(base), offset, rt);
  case InstrId::SW:
    return fmt::format("pgxp_store_word(ctx, {}, {} + {}, {});", inst.rt,
                       s32(base), offset, rt);
  case InstrId::SWL:
    return fmt::format("DO_SWL(ctx, {}, {} + {});", rt, s32(base), offset);
  case InstrId::SWR:
    return fmt::format("DO_SWR(ctx, {}, {} + {});", rt, s32(base), offset);
  default:
    return fmt::format("// UNKNOWN STORE: {}", MipsDecoder::instrName(inst.id));
  }
}

// Branch Emitter

std::string InstructionEmitter::emitBranch(const Instruction &inst,
                                           uint32_t pc) const {
  auto rs = reg(inst.rs);
  auto rt = reg(inst.rt);
  auto target = label(inst.branchTarget(pc));

  switch (inst.id) {
  case InstrId::BEQ:
    return fmt::format("if ({} == {}) goto {};", s32(rs), s32(rt), target);
  case InstrId::BNE:
    return fmt::format("if ({} != {}) goto {};", s32(rs), s32(rt), target);
  case InstrId::BLEZ:
    return fmt::format("if ({} <= 0) goto {};", s32(rs), target);
  case InstrId::BGTZ:
    return fmt::format("if ({} > 0) goto {};", s32(rs), target);
  case InstrId::BLTZ:
    return fmt::format("if ({} < 0) goto {};", s32(rs), target);
  case InstrId::BGEZ:
    return fmt::format("if ({} >= 0) goto {};", s32(rs), target);
  case InstrId::BLTZAL:
    return fmt::format("ctx->r31 = 0x{:08X}; if ({} < 0) goto {};", pc + 8,
                       s32(rs), target);
  case InstrId::BGEZAL:
    return fmt::format("ctx->r31 = 0x{:08X}; if ({} >= 0) goto {};", pc + 8,
                       s32(rs), target);
  default:
    return fmt::format("// UNKNOWN BRANCH: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// Jump Emitter

std::string InstructionEmitter::emitJump(const Instruction &inst,
                                         uint32_t pc) const {
  switch (inst.id) {
  case InstrId::J:
    return fmt::format("goto {};", label(inst.jumpTarget(pc)));
  case InstrId::JAL: {
    auto target = inst.jumpTarget(pc);
    auto name = defaultFuncName(target);
    // `jal` writes $ra = PC+8 on hardware. The C++ call returns on its own, so
    // it is tempting to skip this -- but $ra is data as much as it is control
    // flow: callees save it to the stack, pass it on, and jump through it. A
    // stale $ra from some earlier indirect call then travels into memory and
    // comes back as a garbage pointer. JALR below already does this; JAL did
    // not, across every one of its call sites.
    if (name.empty()) {
      return fmt::format(
          "ctx->r31 = 0x{:08X}; recomp_dispatch(rdram, ctx, 0x{:08X});", pc + 8,
          target);
    }
    return fmt::format("ctx->r31 = 0x{:08X}; {}(rdram, ctx);", pc + 8, name);
  }
  case InstrId::JR:
    if (inst.rs == 31) {
      return "return;";
    }
    return fmt::format("JUMP_INDIRECT(ctx, {});", reg(inst.rs));
  case InstrId::JALR:
    return fmt::format("ctx->r{} = 0x{:08X}; CALL_INDIRECT(ctx, {});", inst.rd,
                       pc + 8, reg(inst.rs));
  default:
    return fmt::format("// UNKNOWN JUMP: {}", MipsDecoder::instrName(inst.id));
  }
}

// COP0 Emitter

std::string InstructionEmitter::emitCOP0(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::MFC0:
    return assignReg(inst.rt, fmt::format("ctx->cop0[{}] /* {} */", inst.rd,
                                          cop0Name(inst.rd)));
  case InstrId::MTC0:
    return fmt::format("ctx->cop0[{}] = {}; // {}", inst.rd, reg(inst.rt),
                       cop0Name(inst.rd));
  case InstrId::RFE:
    return "COP0_RFE(ctx);";
  default:
    return fmt::format("// UNKNOWN COP0: {}", MipsDecoder::instrName(inst.id));
  }
}

// GTE Register Move Emitter

std::string InstructionEmitter::emitGTEMove(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::MFC2:
    return assignReg(inst.rt,
                     fmt::format("pgxp_mfc2(ctx, {}, {})", inst.rt, inst.rd));
  case InstrId::MTC2:
    return fmt::format("pgxp_mtc2(ctx, {}, {}, {});", inst.rd, inst.rt,
                       reg(inst.rt));
  case InstrId::CFC2:
    return assignReg(inst.rt,
                     fmt::format("gte_read_control(ctx, {})", inst.rd));
  case InstrId::CTC2:
    return fmt::format("gte_write_control(ctx, {}, {});", inst.rd,
                       reg(inst.rt));
  case InstrId::LWC2:
    return fmt::format("pgxp_lwc2(ctx, {}, {} + {});", inst.rt,
                       s32(reg(inst.rs)), inst.imm16);
  case InstrId::SWC2:
    return fmt::format("pgxp_swc2(ctx, {}, {} + {});", inst.rt,
                       s32(reg(inst.rs)), inst.imm16);
  default:
    return fmt::format("// UNKNOWN GTE MOVE: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// System Emitter

std::string InstructionEmitter::emitSystem(const Instruction &inst) const {
  switch (inst.id) {
  case InstrId::SYSCALL:
    return "SYSCALL(ctx);";
  case InstrId::BREAK:
    return "BREAK(ctx);";
  default:
    return fmt::format("// UNKNOWN SYSTEM: {}",
                       MipsDecoder::instrName(inst.id));
  }
}

// Main Dispatch

std::string InstructionEmitter::emitInstruction(const Instruction &inst,
                                                uint32_t pc) const {
  if (const auto replacement = m_instructionOverrides.find(pc);
      replacement != m_instructionOverrides.end())
    return replacement->second;

  if (inst.isNOP())
    return "// NOP";
  if (!inst.isValid())
    return fmt::format("// INVALID: 0x{:08X}", inst.raw);

  // Skip writes to $zero
  bool writesToZero = false;
  switch (inst.category) {
  case InstrCategory::ALU:
    // R-type writes to rd, I-type writes to rt
    if (inst.id <= InstrId::SLTU)
      writesToZero = (inst.rd == 0);
    else
      writesToZero = (inst.rt == 0);
    break;
  case InstrCategory::Memory:
    if (inst.isLoad())
      writesToZero = (inst.rt == 0);
    break;
  case InstrCategory::MulDiv:
    if (inst.id == InstrId::MFHI || inst.id == InstrId::MFLO)
      writesToZero = (inst.rd == 0);
    break;
  default:
    break;
  }
  if (writesToZero)
    return "// NOP (writes to $zero)";

  switch (inst.category) {
  case InstrCategory::ALU: {
    // Shifts are ALU-category but have separate emitter
    if (inst.id >= InstrId::SLL && inst.id <= InstrId::SRAV)
      return emitShift(inst);
    return emitALU(inst);
  }
  case InstrCategory::Memory:
    return inst.isStore() ? emitStore(inst) : emitLoad(inst);
  case InstrCategory::Branch:
    return emitBranch(inst, pc);
  case InstrCategory::Jump:
    return emitJump(inst, pc);
  case InstrCategory::MulDiv:
    return emitMulDiv(inst);
  case InstrCategory::System:
    return emitSystem(inst);
  case InstrCategory::COP0:
    return emitCOP0(inst);
  case InstrCategory::GTE: {
    const std::string pcPrefix = fmt::format("ctx->pc = 0x{:08X}; ", pc);
    // GTE commands vs register moves
    if (inst.id >= InstrId::GTE_RTPS && inst.id <= InstrId::GTE_NCCT) {
      bool sf = (inst.raw & (1 << 19)) != 0;
      bool lm = (inst.raw & (1 << 10)) != 0;
      std::string name_str{MipsDecoder::instrName(inst.id)};
      if (name_str.substr(0, 4) == "GTE_")
        name_str = name_str.substr(4);

      if (inst.id == InstrId::GTE_MVMVA) {
        uint8_t mx = (inst.raw >> 17) & 3;
        uint8_t mv = (inst.raw >> 15) & 3;
        uint8_t tv = (inst.raw >> 13) & 3;
        return pcPrefix +
               fmt::format("ps1::GTE::MVMVA(ctx, {}, {}, {}, {}, {}); // MVMVA",
                           mx, mv, tv, sf ? "true" : "false",
                           lm ? "true" : "false");
      }
      return pcPrefix + fmt::format("ps1::GTE::{}(ctx, {}, {}); // GTE cmd",
                                    name_str, sf ? "true" : "false",
                                    lm ? "true" : "false");
    }
    return pcPrefix + emitGTEMove(inst);
  }
  case InstrCategory::Special:
    return fmt::format("// {}: 0x{:08X}", MipsDecoder::instrName(inst.id),
                       inst.raw);
  default:
    return fmt::format("// UNHANDLED: 0x{:08X}", inst.raw);
  }
}

// Function Emitter

std::string InstructionEmitter::emitFunction(const RecompFunction &func) const {
  std::string result;

  // Function signature
  result += fmt::format("void {}(uint8_t* rdram, recomp_context* ctx) {{\n",
                        func.name);
  result +=
      fmt::format("    PS1_FUNCTION_TRACE_HOOK(ctx, 0x{:08X}u);\n",
                  func.entryAddress == 0 ? func.address : func.entryAddress);
  result += "    const bool _native_resume_root = "
            "ctx->nativeResumePc != 0;\n";

  uint32_t pc = func.address;
  size_t numInstr = func.instructions.size();
  const uint32_t callableEntry =
      func.entryAddress == 0 ? func.address : func.entryAddress;

  // Some hand-written PS1 routines preserve their incoming return address in
  // $at, make nested JAL calls (which overwrite $ra), then return with
  // `jr $at`. A recompiled JAL is already a native C++ call, so dispatching
  // that saved guest return address would execute the caller's continuation
  // recursively and then resume it a second time when the C++ call returns.
  // Recognize the conventional leading `move $at,$ra` and treat `jr $at` as
  // the same native return as `jr $ra`.
  int returnAliasReg = -1;
  size_t aliasScanStart = 0;
  if (callableEntry >= func.address) {
    const size_t entryIdx = (callableEntry - func.address) / 4;
    if (entryIdx < numInstr)
      aliasScanStart = entryIdx;
  }
  const size_t aliasScanEnd = std::min<size_t>(numInstr, aliasScanStart + 4);
  for (size_t i = aliasScanStart; i < aliasScanEnd; ++i) {
    const Instruction candidate = MipsDecoder::decode(func.instructions[i]);
    const bool copiesRa =
        (candidate.id == InstrId::ADDU || candidate.id == InstrId::OR) &&
        candidate.rd != 0 &&
        ((candidate.rs == 31 && candidate.rt == 0) ||
         (candidate.rs == 0 && candidate.rt == 31));
    if (copiesRa) {
      returnAliasReg = candidate.rd;
      break;
    }
  }

  // Cooperative callback drains use the current guest $sp when they invoke
  // the VBlank/CD callback. Some hand-written renderer routines temporarily
  // repurpose $sp as a linked-list cursor; injecting a drain into one of
  // their backward loops makes the callback push a frame over the list node
  // itself. Only stack-frame-style ADDI(U) adjustments are safe evidence that
  // $sp still carries its ABI meaning throughout this function.
  bool spUsedAsGeneralPurpose = false;
  bool hasStackAdjustment = false;
  bool hasTerminalBreak = false;
  for (uint32_t raw : func.instructions) {
    const Instruction candidate = MipsDecoder::decode(raw);
    if (candidate.id == InstrId::BREAK)
      hasTerminalBreak = true;
    if (getDestGPR(candidate) != 29)
      continue;
    const bool stackAdjustment =
        (candidate.id == InstrId::ADDI || candidate.id == InstrId::ADDIU) &&
        candidate.rs == 29 && candidate.rt == 29;
    hasStackAdjustment = hasStackAdjustment || stackAdjustment;
    if (!stackAdjustment) {
      spUsedAsGeneralPurpose = true;
    }
  }

  // A root CRT entry commonly establishes $sp from the executable's linker
  // symbols and terminates with BREAK after main returns. That is a real stack
  // initialization, not a temporary data-cursor dynamic extent.
  const bool initializesRootStack =
      spUsedAsGeneralPurpose && !hasStackAdjustment && hasTerminalBreak;

  if (spUsedAsGeneralPurpose && !initializesRootStack) {
    // This guard is intentionally transitive: an ordinary descendant may
    // contain a legitimate polling loop, but callbacks are still unsafe if
    // an assembly ancestor lent it a non-stack $sp value.
    result += "    recomp_callback_drain_guard "
              "_callback_drain_guard(ctx);\n";
  }

  // A backward branch is not necessarily a busy-wait. Games also use them
  // for render-list construction, memcpy-style loops, and other stateful
  // work which must not be interrupted halfway through. Re-entering a game
  // callback from one of those loops can expose a partially initialized
  // object to the callback. Restrict cooperative callback drains to loops
  // that look like read-only status polling: they must read memory and may
  // only perform local computation/control flow. Stores, calls/jumps,
  // system/COP operations, and invalid words make the loop unsafe.
  auto isReadOnlyPollingLoop = [&](size_t branchIdx,
                                   const Instruction &branch) {
    if (branch.id == InstrId::BLTZAL || branch.id == InstrId::BGEZAL)
      return false;

    const uint32_t branchAddr =
        func.address + static_cast<uint32_t>(branchIdx * 4);
    const uint32_t target = branch.branchTarget(branchAddr);
    if (target > branchAddr || target < func.address ||
        ((target - func.address) & 3u) != 0)
      return false;

    const size_t firstIdx = (target - func.address) / 4;
    if (firstIdx > branchIdx || branchIdx + 1 >= numInstr)
      return false;

    // Include the branch delay slot: it executes before the yield point and
    // can itself make an otherwise read-only loop stateful.
    for (size_t loopIdx = firstIdx; loopIdx <= branchIdx + 1; ++loopIdx) {
      const Instruction candidate =
          MipsDecoder::decode(func.instructions[loopIdx]);
      if (!candidate.isValid() || candidate.isStore())
        return false;
      switch (candidate.category) {
      case InstrCategory::Jump:
      case InstrCategory::MulDiv:
      case InstrCategory::System:
      case InstrCategory::COP0:
      case InstrCategory::GTE:
        return false;
      default:
        break;
      }
      if (candidate.id == InstrId::BLTZAL || candidate.id == InstrId::BGEZAL)
        return false;
    }

    // An idle thread can spin on a register loaded before the loop. With
    // only NOPs around the branch, no work is in flight; interrupts are the
    // only way any other guest thread can run (MGS USA's SIO idle task).
    bool idleLoop = true;
    for (size_t loopIdx = firstIdx; loopIdx <= branchIdx + 1; ++loopIdx) {
      if (loopIdx != branchIdx && func.instructions[loopIdx] != 0) {
        idleLoop = false;
        break;
      }
    }
    if (idleLoop) return true;

    // Track whether the actual branch condition depends on a memory value
    // read during this iteration. A read-only counted scan (for example an
    // audio-table reduction) is not polling and must not become a callback
    // yield point merely because it happens to contain loads.
    std::array<bool, 32> memoryTaint{};
    for (size_t loopIdx = firstIdx; loopIdx < branchIdx; ++loopIdx) {
      const Instruction candidate =
          MipsDecoder::decode(func.instructions[loopIdx]);
      const int dest = getDestGPR(candidate);
      if (dest <= 0)
        continue;

      bool tainted = false;
      if (candidate.isLoad()) {
        tainted = true;
      } else if (candidate.category == InstrCategory::ALU) {
        const bool registerForm =
            candidate.id >= InstrId::ADD && candidate.id <= InstrId::SRAV;
        if (registerForm) {
          tainted = memoryTaint[candidate.rs] || memoryTaint[candidate.rt];
        } else if (candidate.id != InstrId::LUI) {
          tainted = memoryTaint[candidate.rs];
        }
      }
      memoryTaint[static_cast<size_t>(dest)] = tainted;
    }

    bool conditionTainted = memoryTaint[branch.rs];
    if (branch.id == InstrId::BEQ || branch.id == InstrId::BNE)
      conditionTainted = conditionTainted || memoryTaint[branch.rt];
    return conditionTainted;
  };

  // Pass 1: Pre-scan ALL instructions
  // Scan every instruction to find all branch/jump targets.
  // For targets within the emitted instruction range: mark as label.
  // For anything else (OOB or in gap beyond loaded instrs): dispatch.
  //
  // IMPORTANT: After an unconditional jump (JR/JALR) + delay slot,
  // subsequent bytes may be DATA (jump tables, padding), not code.
  // We skip unreachable regions to avoid misinterpreting data as branches.
  std::set<uint32_t> oob_set; // deduplicated OOB targets
  // Copy isLabelTarget so we can add new targets discovered in data-as-code
  std::vector<bool> labelTargets = func.isLabelTarget;
  if (labelTargets.size() < numInstr) {
    labelTargets.resize(numInstr, false);
  }

  if (callableEntry != func.address && callableEntry >= func.address) {
    const size_t entryIdx = (callableEntry - func.address) / 4;
    if (entryIdx < labelTargets.size()) {
      labelTargets[entryIdx] = true;
    }
  }

  auto classifyTarget = [&](uint32_t target) {
    // Check if target falls within the actual emitted instructions
    if (target >= func.address) {
      size_t idx = (target - func.address) / 4;
      if (idx < labelTargets.size()) {
        // In-range of emitted instructions: mark as label
        labelTargets[idx] = true;
        return;
      }
    }
    // OOB: outside function OR in the gap beyond loaded instructions
    oob_set.insert(target);
  };

  // First pass: classify ALL targets from ALL instructions.
  // Even data words might appear as instructions in Pass 2 if reachability
  // diverges, so we must ensure every possible target has a label.
  for (size_t i = 0; i < numInstr; ++i) {
    uint32_t addr = func.address + static_cast<uint32_t>(i * 4);
    Instruction inst = MipsDecoder::decode(func.instructions[i]);

    if (inst.category == InstrCategory::Branch) {
      classifyTarget(inst.branchTarget(addr));
    } else if (inst.category == InstrCategory::Jump && inst.id == InstrId::J) {
      classifyTarget(inst.jumpTarget(addr));
    }
  }

  // Register every recovered indirect target as one semantic set. A veneer
  // family can be observed across several JR sites (or through a donor
  // profile rather than a statically recoverable table), so completing each
  // table independently would miss the evidence supplied by its neighbours.
  std::vector<uint32_t> recoveredIndirectTargets = func.indirectTargets;
  for (const auto &jt : func.jumpTables) {
    recoveredIndirectTargets.insert(recoveredIndirectTargets.end(),
                                    jt.targets.begin(), jt.targets.end());
  }
  // Analyzer-proven local labels are equally strong seeds for structural
  // completion.  In hand-written dispatch arrays they may be discovered as
  // callable/CFG entries even when no individual JumpTable object owns them.
  for (size_t targetIdx = 0; targetIdx < labelTargets.size(); ++targetIdx) {
    if (labelTargets[targetIdx]) {
      recoveredIndirectTargets.push_back(
          func.address + static_cast<uint32_t>(targetIdx * 4u));
    }
  }
  recoveredIndirectTargets = completeBranchVeneerTargets(
      func.instructions, func.address, recoveredIndirectTargets);
  for (uint32_t target : recoveredIndirectTargets)
    classifyTarget(target);

  std::set<uint32_t> entryAliases;
  for (uint32_t target : func.entryAliases) {
    if (target < func.address || ((target - func.address) & 3u) != 0)
      continue;
    const size_t idx = (target - func.address) / 4;
    if (idx >= labelTargets.size())
      continue;
    labelTargets[idx] = true;
    entryAliases.insert(target);
  }

  if (!entryAliases.empty()) {
    result += "    if (ctx->nativeEntryPc != 0) {\n"
              "      const uint32_t _native_entry_pc = "
              "ctx->nativeEntryPc;\n"
              "      ctx->nativeEntryPc = 0;\n"
              "      switch (_native_entry_pc) {\n";
    for (uint32_t target : entryAliases) {
      result += fmt::format("      case 0x{:08X}u: goto {};\n", target,
                            label(target));
    }
    result += "      default:\n"
              "        fmt::print(stderr, \"[Dispatch] invalid native "
              "entry PC 0x{:08X}\\n\", _native_entry_pc);\n"
              "        std::abort();\n"
              "      }\n"
              "    }\n";
  }

  std::set<uint32_t> resumeTargets;
  for (uint32_t target : func.resumeTargets) {
    if (target < func.address || ((target - func.address) & 3u) != 0)
      continue;
    const size_t idx = (target - func.address) / 4;
    if (idx >= labelTargets.size())
      continue;
    labelTargets[idx] = true;
    resumeTargets.insert(target);
  }

  if (!resumeTargets.empty()) {
    result += "    if (_native_resume_root) {\n"
              "      const uint32_t _native_resume_pc = "
              "ctx->nativeResumePc;\n"
              "      ctx->nativeResumePc = 0;\n"
              "      switch (_native_resume_pc) {\n";
    for (uint32_t target : resumeTargets) {
      result += fmt::format("      case 0x{:08X}u: goto {};\n", target,
                            label(target));
    }
    result += "      default:\n"
              "        fmt::print(stderr, \"[SaveState] invalid native "
              "resume PC 0x{:08X}\\n\", _native_resume_pc);\n"
              "        std::abort();\n"
              "      }\n"
              "    }\n";
  }

  if (callableEntry != func.address && callableEntry >= func.address) {
    // The C++ symbol still represents the configured function entry. The
    // preceding bytes only exist so computed dispatch can goto shared
    // handlers without leaving this C++ function. A savestate continuation
    // must select its resume label before this normal-entry jump.
    result += fmt::format("    goto {}; // logical function entry\n",
                          label(callableEntry));
  }

  // A shared assembly helper can be entered with an ordinary branch after
  // its caller has placed a continuation in $ra.  Once both pieces live in
  // the same emitted C++ function, the helper's `jr $ra` must resume at that
  // local label; a plain C++ `return` would skip the caller's assembly
  // epilogue.  Recover constant continuations built by the common
  // `lui $ra, upper; addiu/ori $ra, $ra, lower` sequence, plus the implicit
  // PC+8 continuations of REGIMM link branches.
  std::set<uint32_t> localReturnTargets;
  auto recordLocalReturnTarget = [&](uint32_t target) {
    if (target < func.address || ((target - func.address) & 3u) != 0)
      return;
    const size_t idx = (target - func.address) / 4;
    if (idx >= labelTargets.size())
      return;
    labelTargets[idx] = true;
    localReturnTargets.insert(target);
  };

  for (size_t i = 0; i < numInstr; ++i) {
    const Instruction inst = MipsDecoder::decode(func.instructions[i]);
    const uint32_t addr = func.address + static_cast<uint32_t>(i * 4);
    if (inst.id == InstrId::BLTZAL || inst.id == InstrId::BGEZAL)
      recordLocalReturnTarget(addr + 8);

    if (inst.id == InstrId::JAL) {
      const uint32_t target = inst.jumpTarget(addr);
      if (target >= func.address && ((target - func.address) & 3u) == 0 &&
          (target - func.address) / 4 < labelTargets.size() &&
          labelTargets[(target - func.address) / 4]) {
        // Keep the architectural call and return inside this emitted owner.
        // Requiring a target the CFG already owns avoids treating an ordinary
        // ABI callee as local merely because broad analyzer expansion happens
        // to place its bytes in this C++ body. Calling a second C++ owner for
        // a genuinely shared region loses tail branches from the helper back
        // into its caller and turns an ordinary renderer loop into unbounded
        // native recursion.
        recordLocalReturnTarget(addr + 8);
      }
    }

    if (inst.id != InstrId::LUI || inst.rt != 31 || i + 1 >= numInstr)
      continue;
    const Instruction low = MipsDecoder::decode(func.instructions[i + 1]);
    if (low.rt != 31 || low.rs != 31)
      continue;

    uint32_t target = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16))
                      << 16;
    if (low.id == InstrId::ADDI || low.id == InstrId::ADDIU) {
      target += static_cast<uint32_t>(static_cast<int32_t>(low.imm16));
    } else if (low.id == InstrId::ORI) {
      target |= static_cast<uint16_t>(low.imm16);
    } else {
      continue;
    }
    recordLocalReturnTarget(target);
  }

  // A computed JR may deliberately select PC+8, the instruction immediately
  // after its delay slot. Hand-written renderers use this as one entry in a
  // register-driven dispatch even though ordinary linear reachability treats
  // everything after JR as data. Keep that adjacent entry available as code
  // and as a local indirect target. JR $ra (and a recognized saved-$ra alias)
  // remains a true function return and must not make trailing literal data
  // reachable.
  for (size_t i = 0; i + 2 < numInstr; ++i) {
    const Instruction inst = MipsDecoder::decode(func.instructions[i]);
    if (inst.id == InstrId::JR && inst.rs != 31 &&
        static_cast<int>(inst.rs) != returnAliasReg) {
      labelTargets[i + 2] = true;
    }
  }

  // Checkpoints are needed at callable entries and CFG labels, not at every
  // translated instruction. This keeps generated games compact while still
  // covering function boundaries, branch continuations and profiled computed
  // entries. A callable entry that is already a label is traced by the label
  // below, including on later local loop-backs.
  if (callableEntry >= func.address) {
    const size_t entryIdx = (callableEntry - func.address) / 4;
    if (entryIdx < labelTargets.size() && !labelTargets[entryIdx]) {
      result += fmt::format("    PS1_PC_TRACE_HOOK(ctx, 0x{:08X}u);\n",
                            callableEntry);
    }
  }

  // Pass 2: Emit code
  // Track reachability: after JR/JALR + delay slot, treat subsequent
  // words as data comments until the next branch target label.
  bool reachable = true;
  int skipAfterJump = 0;

  for (size_t i = 0; i < numInstr; ++i) {
    uint32_t addr = func.address + static_cast<uint32_t>(i * 4);

    // Emit label if this address is a branch target
    if (i < labelTargets.size() && labelTargets[i]) {
      result += fmt::format("{}:\n", label(addr));
      result += fmt::format("    PS1_PC_TRACE_HOOK(ctx, 0x{:08X}u);\n", addr);
      reachable = true;
      skipAfterJump = 0;
    }

    if (skipAfterJump > 0) {
      skipAfterJump--;
      if (skipAfterJump == 0) {
        reachable = false;
      }
    }

    if (!reachable) {
      // Emit as data comment instead of decoding
      result +=
          fmt::format("    // .word 0x{:08X} (data)\n", func.instructions[i]);
      continue;
    }

    Instruction inst = MipsDecoder::decode(func.instructions[i]);
    std::string code = emitInstruction(inst, addr);

    if (inst.id == InstrId::JAL) {
      const uint32_t target = inst.jumpTarget(addr);
      if (target >= func.address && ((target - func.address) & 3u) == 0 &&
          (target - func.address) / 4 < labelTargets.size() &&
          labelTargets[(target - func.address) / 4]) {
        code = fmt::format(
            "ctx->r31 = 0x{:08X}; goto {}; // local architectural JAL",
            addr + 8, label(target));
      }
    }

    if (inst.id == InstrId::JR && inst.rs == 31 &&
        !localReturnTargets.empty()) {
      std::string localReturn = "{ // architectural $ra may resume locally\n";
      for (uint32_t target : localReturnTargets) {
        localReturn += fmt::format("    if (ctx->r31 == 0x{:08X}u) goto {};\n",
                                   target, label(target));
      }
      localReturn += "    if (_native_resume_root) { "
                     "recomp_resume_dispatch(rdram, ctx, ctx->r31); "
                     "return; }\n";
      localReturn += "    return;\n";
      localReturn += "    }";
      code = std::move(localReturn);
    }

    if (inst.id == InstrId::JR && static_cast<int>(inst.rs) == returnAliasReg) {
      code = "if (_native_resume_root) { recomp_resume_dispatch(rdram, ctx, " +
             reg(static_cast<uint8_t>(returnAliasReg)) +
             "); return; } return; // return through saved $ra alias";
    } else if (inst.id == InstrId::JR && inst.rs == 31 &&
               localReturnTargets.empty()) {
      code = "if (_native_resume_root) { recomp_resume_dispatch(rdram, ctx, "
             "ctx->r31); return; } return;";
    }

    // Indirect-jump override.
    //
    // A detected jump table gives us precise targets, but hand-written
    // renderer regions also pass local labels through registers without a
    // conventional table. Test every known local CFG label before falling
    // back to the global dispatcher. This is essential when several callable
    // entries share one assembly body: an internal `jr $rx` must remain a C++
    // goto rather than requiring every private loop label to be a globally
    // callable function.
    if (inst.id == InstrId::JR && inst.rs != 31 &&
        static_cast<int>(inst.rs) != returnAliasReg) {
      std::set<uint32_t> indirectTargets;
      for (const auto &jt : func.jumpTables) {
        if (jt.jrInstrIdx == i && !jt.targets.empty()) {
          const std::vector<uint32_t> targets = completeBranchVeneerTargets(
              func.instructions, func.address, jt.targets);
          indirectTargets.insert(targets.begin(), targets.end());
          break;
        }
      }
      for (size_t targetIdx = 0; targetIdx < labelTargets.size(); ++targetIdx) {
        if (labelTargets[targetIdx]) {
          indirectTargets.insert(func.address +
                                 static_cast<uint32_t>(targetIdx * 4));
        }
      }

      if (!indirectTargets.empty()) {
        std::string sw;
        sw += fmt::format("{{ // local indirect dispatch ({} targets)\n",
                          indirectTargets.size());
        sw += fmt::format(
            "    uint32_t _sw_target = static_cast<uint32_t>({});\n",
            reg(inst.rs));
        for (uint32_t target : indirectTargets) {
          sw += fmt::format("    if (_sw_target == 0x{:08X}u) goto {};\n",
                            target, label(target));
        }
        // The fallback can be an external trampoline which immediately
        // executes `jr $ra`.  Native dispatch represents that architectural
        // return by returning to this C++ frame, so translate a local $ra
        // back into the corresponding label before leaving the owner.
        //
        // Crash 3's renderer reaches 0x8003F67C this way after a local JAL at
        // 0x8003FDB8.  Returning from the C++ owner with $ra=0x8003FDC0 skips
        // the rest of the renderer and drops the complete actor primitive
        // stream.
        sw += "    PS1_INDIRECT_TRACE_HOOK(ctx, _sw_target);\n";
        sw += "    recomp_dispatch(rdram, ctx, _sw_target);\n";
        for (uint32_t target : localReturnTargets) {
          sw += fmt::format(
              "    if (ctx->r31 == 0x{:08X}u) goto {};\n", target,
              label(target));
        }
        sw += "    if (_native_resume_root) { "
              "recomp_resume_dispatch(rdram, ctx, ctx->r31); return; }\n";
        sw += "    return; // completed external indirect tail\n";
        sw += "    }";
        code = sw;
      }
    }

    // Handle branch delay slots: if this instruction has a delay slot,
    // emit the next instruction BEFORE the branch/jump.
    //
    // IMPORTANT -- MIPS delay slot semantics:
    //   In real hardware the branch condition (or jump target register)
    //   is evaluated BEFORE the delay slot executes.  If the delay slot
    //   writes to a register that the branch/jump reads, we must save
    //   that register to a temporary before the delay slot runs, then
    //   use the saved value in the branch/jump.
    if (inst.hasBranchDelaySlot() && i + 1 < numInstr) {
      Instruction delayInst = MipsDecoder::decode(func.instructions[i + 1]);
      std::string delayCode = emitInstruction(delayInst, addr + 4);

      int destGPR = getDestGPR(delayInst);

      // Detect conflict: delay slot writes to a register that the
      // branch condition or jump target reads.
      // For conditional branches: rs (and rt for BEQ/BNE).
      // For JR/JALR: rs (the target register).
      std::set<int> conflictRegs;
      if (destGPR > 0) { // r0 is hardwired to 0, no conflict possible
        if (inst.isBranch()) {
          if (destGPR == static_cast<int>(inst.rs))
            conflictRegs.insert(destGPR);
          if ((inst.id == InstrId::BEQ || inst.id == InstrId::BNE) &&
              destGPR == static_cast<int>(inst.rt))
            conflictRegs.insert(destGPR);
        } else if (inst.id == InstrId::JR || inst.id == InstrId::JALR) {
          if (destGPR == static_cast<int>(inst.rs))
            conflictRegs.insert(destGPR);
        }
      }

      // Auto-inject drainPendingCallbacks for read-only polling loops. A
      // generic backward branch is not sufficient: stateful loops must stay
      // atomic with respect to guest callbacks.
      std::string yieldCode;
      std::string loopTraceCode;
      if (inst.isBranch()) {
        uint32_t brTarget = inst.branchTarget(addr);
        if (brTarget <= addr) {
          loopTraceCode =
              fmt::format("    PS1_LOOP_TRACE_HOOK(ctx, 0x{:08X}u);\n", addr);
          if (!spUsedAsGeneralPurpose && isReadOnlyPollingLoop(i, inst)) {
            yieldCode = fmt::format("    if (ctx->bios) "
                        "ps1::runtimeBiosDrainPendingCallbacks(ctx->bios, 0x{:08X}u);\n", addr);
          }
        }
      }

      result += yieldCode;
      if (!conflictRegs.empty()) {
        // Save conflicting registers, run delay slot, then branch/jump
        // using the saved values.  Wrap in a block scope so that the
        // local temporaries don't get crossed by gotos elsewhere in the
        // function (C++ forbids goto jumping over an initialiser).
        result += "    {\n";
        for (int r : conflictRegs) {
          result += fmt::format(
              "      uint32_t _ds_r{} = ctx->r{}; // save for branch\n", r, r);
        }
        result += fmt::format("      {} // delay slot\n", delayCode);
        std::string branchCode = code;
        for (int r : conflictRegs) {
          replaceRegRef(branchCode, fmt::format("ctx->r{}", r),
                        fmt::format("_ds_r{}", r));
        }
        result += loopTraceCode;
        result += fmt::format("      {}\n", branchCode);
        result += "    }\n";
      } else {
        // No conflict -- emit delay slot then branch/jump (original order)
        result += fmt::format("    {} // delay slot\n", delayCode);
        result += loopTraceCode;
        result += fmt::format("    {}\n", code);
      }
      // A delay slot can also be somebody else's branch target. It is emitted
      // inline above (before the transfer), and the main loop is about to skip
      // its natural position -- which would take its label with it, leaving
      // `goto L_<ds>` with no definition anywhere. The post-pass then turns
      // that into a dispatch to an address inside this very function, and the
      // runtime can only fail on it.
      //
      // Measured case: Crash SCUS-94900 func_8003A144, where 0x8003A264 is the
      // delay slot of the branch at 0x8003A260 *and* the target of branches at
      // 0x8003A1E0 and 0x8003A258.
      //
      // So re-emit it at its natural position, labelled, for control arriving
      // from elsewhere. The fall-through path already ran it inline, so it
      // jumps over the copy.
      const bool dsIsBranchTarget =
          (i + 1) < labelTargets.size() && labelTargets[i + 1];
      if (dsIsBranchTarget) {
        const uint32_t dsAddr = addr + 4;
        const bool guardFallThrough = inst.isBranch();
        const std::string done = fmt::format("L_dsdone_{:08X}", dsAddr);
        if (guardFallThrough)
          result += fmt::format("    goto {};\n", done);
        result += fmt::format("{}:\n", label(dsAddr));
        result += fmt::format("    {} // delay slot, also a branch target\n",
                              delayCode);
        if (guardFallThrough)
          result += fmt::format("{}: ;\n", done);
      }

      ++i; // Skip delay slot instruction
    } else {
      result += fmt::format("    {}\n", code);
    }

    // Track unconditional jumps for reachability.
    // Only JR makes subsequent code unreachable -- JALR is a function call
    // that returns, so code after JALR is still reachable.
    if (inst.category == InstrCategory::Jump && inst.id == InstrId::JR) {
      skipAfterJump = 1; // delay slot was already consumed above, skip 1 more
    }
  }

  // Emit OOB target labels.
  //
  // An out-of-range branch is not always a tail call. Hand-written PS1
  // assembly can put a continuation address in $ra, branch to a shared code
  // fragment, and rely on that fragment's `jr $ra` to resume in this body.
  // A native recompiled callee represents `jr $ra` by returning to C++, so
  // after recomp_dispatch returns we must translate a local architectural
  // return address back into a local goto. Otherwise the containing C++
  // function returns too early and leaves every register that its assembly
  // epilogue would have restored in a transient state.
  //
  // Crash 3 exercises this in func_80039FA0: it sets $ra=0x8003A240 and
  // branches to the shared copy helper at 0x8003A7C4. Losing that continuation
  // leaves $sp pointing into the compressed model stream.
  std::vector<uint32_t> localResumeTargets;
  localResumeTargets.reserve(labelTargets.size());
  for (size_t i = 0; i < labelTargets.size(); ++i) {
    if (labelTargets[i]) {
      localResumeTargets.push_back(func.address + static_cast<uint32_t>(i * 4));
    }
  }

  auto emitOobDispatch = [&](uint32_t target) {
    result +=
        fmt::format("    recomp_dispatch(rdram, ctx, 0x{:08X});\n", target);
    for (uint32_t resume : localResumeTargets) {
      result += fmt::format("    if (ctx->r31 == 0x{:08X}u) goto {};\n", resume,
                            label(resume));
    }
    result += "    if (_native_resume_root) { "
              "recomp_resume_dispatch(rdram, ctx, ctx->r31); return; }\n";
    result += "    return;\n";
  };

  // Always use recomp_dispatch for OOB -- we can't trust that the target
  // corresponds to a known function.
  for (uint32_t target : oob_set) {
    result += fmt::format("{}:\n", label(target));
    emitOobDispatch(target);
  }

  // Post-pass: catch-all for undefined labels
  // Scan the generated code for all "goto L_XXXXXXXX" references
  // and ensure every one has a matching "L_XXXXXXXX:" definition.
  // This catches any edge cases the pre-scan missed.
  {
    std::set<uint32_t> referenced;
    std::set<uint32_t> defined;

    // Find all "goto L_" references
    size_t pos = 0;
    while ((pos = result.find("goto L_", pos)) != std::string::npos) {
      pos += 7; // skip "goto L_"
      if (pos + 8 <= result.size()) {
        try {
          uint32_t addr = std::stoul(result.substr(pos, 8), nullptr, 16);
          referenced.insert(addr);
        } catch (...) {
        }
      }
      pos += 8;
    }

    // Find all "L_XXXXXXXX:" label definitions
    pos = 0;
    while ((pos = result.find("L_", pos)) != std::string::npos) {
      size_t lpos = pos;
      pos += 2; // skip "L_"
      if (pos + 8 <= result.size() && pos + 8 < result.size() &&
          result[pos + 8] == ':') {
        // Check this isn't preceded by "goto " (i.e., it's a label def)
        if (lpos == 0 || result[lpos - 1] == '\n' || result[lpos - 1] == ' ') {
          try {
            uint32_t addr = std::stoul(result.substr(pos, 8), nullptr, 16);
            defined.insert(addr);
          } catch (...) {
          }
        }
      }
    }

    // Emit dispatch for any referenced-but-undefined labels
    for (uint32_t addr : referenced) {
      if (defined.find(addr) == defined.end()) {
        result += fmt::format("{}:\n", label(addr));
        emitOobDispatch(addr);
      }
    }
  }

  result += "}\n";
  return result;
}

} // namespace ps1recomp
