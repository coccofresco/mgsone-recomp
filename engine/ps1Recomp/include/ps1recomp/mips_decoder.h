#pragma once

// ps1Recomp -- MIPS I Decoder
// Decodes R3000A instructions (MIPS I + COP0 + COP2/GTE)

#include <cstdint>
#include <string_view>

namespace ps1recomp {

// Instruction IDs

enum class InstrId : uint8_t {
    // Invalid / Unknown
    INVALID = 0,

    // R-type ALU
    ADD, ADDU, SUB, SUBU,
    AND, OR, XOR, NOR,
    SLT, SLTU,

    // Shifts
    SLL, SRL, SRA,
    SLLV, SRLV, SRAV,

    // Multiply / Divide
    MULT, MULTU, DIV, DIVU,
    MFHI, MTHI, MFLO, MTLO,

    // I-type ALU
    ADDI, ADDIU,
    ANDI, ORI, XORI,
    SLTI, SLTIU,
    LUI,

    // Loads
    LB, LBU, LH, LHU, LW,
    LWL, LWR,

    // Stores
    SB, SH, SW,
    SWL, SWR,

    // Branches
    BEQ, BNE,
    BLEZ, BGTZ,
    BLTZ, BGEZ,
    BLTZAL, BGEZAL,

    // Jumps
    J, JAL, JR, JALR,

    // System
    SYSCALL, BREAK,

    // COP0
    MFC0, MTC0, RFE,

    // COP2 / GTE register moves
    MFC2, MTC2, CFC2, CTC2,
    LWC2, SWC2,

    // COP2 / GTE commands
    GTE_RTPS,    // Perspective transform (single)
    GTE_NCLIP,   // Normal clipping
    GTE_OP,      // Outer product
    GTE_DPCS,    // Depth cue (single)
    GTE_INTPL,   // Interpolation
    GTE_MVMVA,   // Matrix-vector multiply
    GTE_NCDS,    // Normal color depth (single)
    GTE_CDP,     // Color depth cue
    GTE_NCDT,    // Normal color depth (triple)
    GTE_NCCS,    // Normal color color (single)
    GTE_CC,      // Color color
    GTE_NCS,     // Normal color (single)
    GTE_NCT,     // Normal color (triple)
    GTE_SQR,     // Square of vector
    GTE_DCPL,    // Depth cue color light
    GTE_DPCT,    // Depth cue (triple)
    GTE_AVSZ3,   // Average Z (3 values)
    GTE_AVSZ4,   // Average Z (4 values)
    GTE_RTPT,    // Perspective transform (triple)
    GTE_GPF,     // General purpose interpolation
    GTE_GPL,     // General purpose interpolation + base
    GTE_NCCT,    // Normal color color (triple)

    // NOP (alias for SLL $0, $0, 0)
    NOP,

    COUNT  // Total count
};

// Instruction Categories

enum class InstrCategory : uint8_t {
    ALU,        // Arithmetic/logic/shift
    Memory,     // Load/store
    Branch,     // Conditional branches
    Jump,       // J, JAL, JR, JALR
    MulDiv,     // MULT, DIV, MFHI, MFLO, etc.
    System,     // SYSCALL, BREAK
    COP0,       // MFC0, MTC0, RFE
    GTE,        // COP2 register moves + commands
    Special,    // NOP, INVALID
};

// Decoded Instruction

struct Instruction {
    InstrId     id       = InstrId::INVALID;
    InstrCategory category = InstrCategory::Special;
    uint32_t    raw      = 0;       // Original 32-bit word

    // Decoded fields (valid depending on format)
    uint8_t     rs       = 0;       // Source register 1 (bits 25-21)
    uint8_t     rt       = 0;       // Source register 2 / dest (bits 20-16)
    uint8_t     rd       = 0;       // Destination register (bits 15-11)
    uint8_t     shamt    = 0;       // Shift amount (bits 10-6)
    int16_t     imm16    = 0;       // Immediate (bits 15-0, sign-extended)
    uint32_t    target26 = 0;       // Jump target (bits 25-0)

    // Helpers
    bool isValid() const { return id != InstrId::INVALID; }
    bool isNOP() const { return id == InstrId::NOP; }
    bool isBranch() const { return category == InstrCategory::Branch; }
    bool isJump() const { return category == InstrCategory::Jump; }
    bool isMemory() const { return category == InstrCategory::Memory; }
    bool isGTE() const { return category == InstrCategory::GTE; }
    bool isStore() const;
    bool isLoad() const;
    bool hasBranchDelaySlot() const { return isBranch() || isJump(); }

    /// Compute full jump target given the PC of this instruction
    uint32_t jumpTarget(uint32_t pc) const {
        return (pc & 0xF0000000) | (target26 << 2);
    }

    /// Compute branch target given the PC of this instruction
    uint32_t branchTarget(uint32_t pc) const {
        return pc + 4 + (static_cast<int32_t>(imm16) << 2);
    }
};

// Decoder

class MipsDecoder {
public:
    /// Decode a single 32-bit MIPS instruction
    static Instruction decode(uint32_t raw);

    /// Get human-readable name for an instruction
    static std::string_view instrName(InstrId id);

    /// Get human-readable name for a category
    static std::string_view categoryName(InstrCategory cat);
};

// Register Names

/// GPR names ($zero, $at, $v0-v1, $a0-a3, $t0-t9, $s0-s7, $k0-k1, $gp, $sp, $fp, $ra)
std::string_view gprName(uint8_t reg);

/// COP0 register names
std::string_view cop0Name(uint8_t reg);

} // namespace ps1recomp
