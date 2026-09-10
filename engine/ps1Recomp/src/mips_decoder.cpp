// ps1Recomp -- MIPS I Decoder Implementation
// Decodes R3000A instructions: MIPS I + COP0 + COP2/GTE

#include "ps1recomp/mips_decoder.h"

namespace ps1recomp {

// Field Extraction

static constexpr uint8_t  extractOpcode(uint32_t raw)   { return (raw >> 26) & 0x3F; }
static constexpr uint8_t  extractRs(uint32_t raw)       { return (raw >> 21) & 0x1F; }
static constexpr uint8_t  extractRt(uint32_t raw)       { return (raw >> 16) & 0x1F; }
static constexpr uint8_t  extractRd(uint32_t raw)       { return (raw >> 11) & 0x1F; }
static constexpr uint8_t  extractShamt(uint32_t raw)    { return (raw >>  6) & 0x1F; }
static constexpr uint8_t  extractFunct(uint32_t raw)    { return  raw        & 0x3F; }
static constexpr int16_t  extractImm16(uint32_t raw)    { return static_cast<int16_t>(raw & 0xFFFF); }
static constexpr uint32_t extractTarget26(uint32_t raw) { return raw & 0x03FFFFFF; }

// Decode: SPECIAL (opcode 0x00)

static Instruction decodeSpecial(uint32_t raw) {
    Instruction inst;
    inst.raw    = raw;
    inst.rs     = extractRs(raw);
    inst.rt     = extractRt(raw);
    inst.rd     = extractRd(raw);
    inst.shamt  = extractShamt(raw);

    uint8_t funct = extractFunct(raw);

    switch (funct) {
        // Shifts (immediate)
        case 0x00:  // SLL
            if (raw == 0) { inst.id = InstrId::NOP; inst.category = InstrCategory::Special; return inst; }
            inst.id = InstrId::SLL; break;
        case 0x02: inst.id = InstrId::SRL; break;
        case 0x03: inst.id = InstrId::SRA; break;

        // Shifts (variable)
        case 0x04: inst.id = InstrId::SLLV; break;
        case 0x06: inst.id = InstrId::SRLV; break;
        case 0x07: inst.id = InstrId::SRAV; break;

        // Jumps (register)
        case 0x08: inst.id = InstrId::JR;   inst.category = InstrCategory::Jump; return inst;
        case 0x09: inst.id = InstrId::JALR; inst.category = InstrCategory::Jump; return inst;

        // System
        case 0x0C: inst.id = InstrId::SYSCALL; inst.category = InstrCategory::System; return inst;
        case 0x0D: inst.id = InstrId::BREAK;   inst.category = InstrCategory::System; return inst;

        // HI/LO moves
        case 0x10: inst.id = InstrId::MFHI; inst.category = InstrCategory::MulDiv; return inst;
        case 0x11: inst.id = InstrId::MTHI; inst.category = InstrCategory::MulDiv; return inst;
        case 0x12: inst.id = InstrId::MFLO; inst.category = InstrCategory::MulDiv; return inst;
        case 0x13: inst.id = InstrId::MTLO; inst.category = InstrCategory::MulDiv; return inst;

        // Multiply / Divide
        case 0x18: inst.id = InstrId::MULT;  inst.category = InstrCategory::MulDiv; return inst;
        case 0x19: inst.id = InstrId::MULTU; inst.category = InstrCategory::MulDiv; return inst;
        case 0x1A: inst.id = InstrId::DIV;   inst.category = InstrCategory::MulDiv; return inst;
        case 0x1B: inst.id = InstrId::DIVU;  inst.category = InstrCategory::MulDiv; return inst;

        // ALU R-type
        case 0x20: inst.id = InstrId::ADD;  break;
        case 0x21: inst.id = InstrId::ADDU; break;
        case 0x22: inst.id = InstrId::SUB;  break;
        case 0x23: inst.id = InstrId::SUBU; break;
        case 0x24: inst.id = InstrId::AND;  break;
        case 0x25: inst.id = InstrId::OR;   break;
        case 0x26: inst.id = InstrId::XOR;  break;
        case 0x27: inst.id = InstrId::NOR;  break;
        case 0x2A: inst.id = InstrId::SLT;  break;
        case 0x2B: inst.id = InstrId::SLTU; break;

        default:
            inst.id = InstrId::INVALID;
            inst.category = InstrCategory::Special;
            return inst;
    }

    inst.category = InstrCategory::ALU;
    return inst;
}

// Decode: REGIMM (opcode 0x01)

static Instruction decodeRegimm(uint32_t raw) {
    Instruction inst;
    inst.raw   = raw;
    inst.rs    = extractRs(raw);
    inst.imm16 = extractImm16(raw);
    inst.category = InstrCategory::Branch;

    uint8_t rt = extractRt(raw);

    switch (rt) {
        case 0x00: inst.id = InstrId::BLTZ;   break;
        case 0x01: inst.id = InstrId::BGEZ;   break;
        case 0x10: inst.id = InstrId::BLTZAL; break;
        case 0x11: inst.id = InstrId::BGEZAL; break;
        default:
            inst.id = InstrId::INVALID;
            inst.category = InstrCategory::Special;
            break;
    }

    return inst;
}

// Decode: COP0 (opcode 0x10)

static Instruction decodeCOP0(uint32_t raw) {
    Instruction inst;
    inst.raw = raw;
    inst.rs  = extractRs(raw);
    inst.rt  = extractRt(raw);
    inst.rd  = extractRd(raw);
    inst.category = InstrCategory::COP0;

    uint8_t rs = inst.rs; // COP sub-opcode in rs field

    switch (rs) {
        case 0x00: inst.id = InstrId::MFC0; break; // Move from COP0
        case 0x04: inst.id = InstrId::MTC0; break; // Move to COP0
        default:
            // Check for RFE (bits 0-5 = 0x10, rs = 0x10)
            if (rs == 0x10 && extractFunct(raw) == 0x10) {
                inst.id = InstrId::RFE;
            } else {
                inst.id = InstrId::INVALID;
            }
            break;
    }

    return inst;
}

// Decode: COP2 / GTE (opcode 0x12)

static Instruction decodeCOP2(uint32_t raw) {
    Instruction inst;
    inst.raw = raw;
    inst.rs  = extractRs(raw);
    inst.rt  = extractRt(raw);
    inst.rd  = extractRd(raw);
    inst.category = InstrCategory::GTE;

    uint8_t rs = inst.rs; // COP sub-opcode

    // Register move instructions (bit 25 = 0)
    if (!(raw & (1 << 25))) {
        switch (rs) {
            case 0x00: inst.id = InstrId::MFC2; break; // Move from GTE data reg
            case 0x02: inst.id = InstrId::CFC2; break; // Move from GTE control reg
            case 0x04: inst.id = InstrId::MTC2; break; // Move to GTE data reg
            case 0x06: inst.id = InstrId::CTC2; break; // Move to GTE control reg
            default:   inst.id = InstrId::INVALID; break;
        }
        return inst;
    }

    // GTE commands (bit 25 = 1) -- decode funct bits 0-5
    uint8_t cmd = raw & 0x3F;

    switch (cmd) {
        case 0x01: inst.id = InstrId::GTE_RTPS;  break;
        case 0x06: inst.id = InstrId::GTE_NCLIP; break;
        case 0x0C: inst.id = InstrId::GTE_OP;    break;
        case 0x10: inst.id = InstrId::GTE_DPCS;  break;
        case 0x11: inst.id = InstrId::GTE_INTPL; break;
        case 0x12: inst.id = InstrId::GTE_MVMVA; break;
        case 0x13: inst.id = InstrId::GTE_NCDS;  break;
        case 0x14: inst.id = InstrId::GTE_CDP;   break;
        case 0x16: inst.id = InstrId::GTE_NCDT;  break;
        case 0x1B: inst.id = InstrId::GTE_NCCS;  break;
        case 0x1C: inst.id = InstrId::GTE_CC;    break;
        case 0x1E: inst.id = InstrId::GTE_NCS;   break;
        case 0x20: inst.id = InstrId::GTE_NCT;   break;
        case 0x28: inst.id = InstrId::GTE_SQR;   break;
        case 0x29: inst.id = InstrId::GTE_DCPL;  break;
        case 0x2A: inst.id = InstrId::GTE_DPCT;  break;
        case 0x2D: inst.id = InstrId::GTE_AVSZ3; break;
        case 0x2E: inst.id = InstrId::GTE_AVSZ4; break;
        case 0x30: inst.id = InstrId::GTE_RTPT;  break;
        case 0x3D: inst.id = InstrId::GTE_GPF;   break;
        case 0x3E: inst.id = InstrId::GTE_GPL;   break;
        case 0x3F: inst.id = InstrId::GTE_NCCT;  break;
        default:   inst.id = InstrId::INVALID;    break;
    }

    return inst;
}

// Main Decoder

Instruction MipsDecoder::decode(uint32_t raw) {
    uint8_t opcode = extractOpcode(raw);

    switch (opcode) {
        // SPECIAL group
        case 0x00: return decodeSpecial(raw);

        // REGIMM group
        case 0x01: return decodeRegimm(raw);

        // J-type
        case 0x02: {
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::J;
            inst.category = InstrCategory::Jump;
            inst.target26 = extractTarget26(raw);
            return inst;
        }
        case 0x03: {
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::JAL;
            inst.category = InstrCategory::Jump;
            inst.target26 = extractTarget26(raw);
            return inst;
        }

        // Branches
        case 0x04: { // BEQ
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::BEQ;
            inst.category = InstrCategory::Branch;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x05: { // BNE
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::BNE;
            inst.category = InstrCategory::Branch;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x06: { // BLEZ
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::BLEZ;
            inst.category = InstrCategory::Branch;
            inst.rs = extractRs(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x07: { // BGTZ
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::BGTZ;
            inst.category = InstrCategory::Branch;
            inst.rs = extractRs(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }

        // I-type ALU
        case 0x08: { // ADDI
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::ADDI;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x09: { // ADDIU
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::ADDIU;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x0A: { // SLTI
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::SLTI;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x0B: { // SLTIU
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::SLTIU;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x0C: { // ANDI
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::ANDI;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x0D: { // ORI
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::ORI;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x0E: { // XORI
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::XORI;
            inst.category = InstrCategory::ALU;
            inst.rs = extractRs(raw);
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }
        case 0x0F: { // LUI
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::LUI;
            inst.category = InstrCategory::ALU;
            inst.rt = extractRt(raw);
            inst.imm16 = extractImm16(raw);
            return inst;
        }

        // COP0
        case 0x10: return decodeCOP0(raw);

        // COP2 / GTE
        case 0x12: return decodeCOP2(raw);

        // Loads
        case 0x20: { Instruction i; i.raw=raw; i.id=InstrId::LB;  i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x21: { Instruction i; i.raw=raw; i.id=InstrId::LH;  i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x22: { Instruction i; i.raw=raw; i.id=InstrId::LWL; i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x23: { Instruction i; i.raw=raw; i.id=InstrId::LW;  i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x24: { Instruction i; i.raw=raw; i.id=InstrId::LBU; i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x25: { Instruction i; i.raw=raw; i.id=InstrId::LHU; i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x26: { Instruction i; i.raw=raw; i.id=InstrId::LWR; i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }

        // Stores
        case 0x28: { Instruction i; i.raw=raw; i.id=InstrId::SB;  i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x29: { Instruction i; i.raw=raw; i.id=InstrId::SH;  i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x2A: { Instruction i; i.raw=raw; i.id=InstrId::SWL; i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x2B: { Instruction i; i.raw=raw; i.id=InstrId::SW;  i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x2E: { Instruction i; i.raw=raw; i.id=InstrId::SWR; i.category=InstrCategory::Memory; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }

        // COP2 Loads/Stores
        case 0x32: { Instruction i; i.raw=raw; i.id=InstrId::LWC2; i.category=InstrCategory::GTE; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }
        case 0x3A: { Instruction i; i.raw=raw; i.id=InstrId::SWC2; i.category=InstrCategory::GTE; i.rs=extractRs(raw); i.rt=extractRt(raw); i.imm16=extractImm16(raw); return i; }

        default: {
            Instruction inst;
            inst.raw = raw;
            inst.id = InstrId::INVALID;
            inst.category = InstrCategory::Special;
            return inst;
        }
    }
}

// Instruction::isStore / isLoad

bool Instruction::isStore() const {
    switch (id) {
        case InstrId::SB: case InstrId::SH: case InstrId::SW:
        case InstrId::SWL: case InstrId::SWR: case InstrId::SWC2:
            return true;
        default: return false;
    }
}

bool Instruction::isLoad() const {
    switch (id) {
        case InstrId::LB: case InstrId::LBU: case InstrId::LH: case InstrId::LHU:
        case InstrId::LW: case InstrId::LWL: case InstrId::LWR: case InstrId::LWC2:
            return true;
        default: return false;
    }
}

// Name Tables

std::string_view MipsDecoder::instrName(InstrId id) {
    switch (id) {
        case InstrId::INVALID: return "INVALID";
        case InstrId::NOP:     return "NOP";

        // ALU R-type
        case InstrId::ADD:  return "ADD";
        case InstrId::ADDU: return "ADDU";
        case InstrId::SUB:  return "SUB";
        case InstrId::SUBU: return "SUBU";
        case InstrId::AND:  return "AND";
        case InstrId::OR:   return "OR";
        case InstrId::XOR:  return "XOR";
        case InstrId::NOR:  return "NOR";
        case InstrId::SLT:  return "SLT";
        case InstrId::SLTU: return "SLTU";

        // Shifts
        case InstrId::SLL:  return "SLL";
        case InstrId::SRL:  return "SRL";
        case InstrId::SRA:  return "SRA";
        case InstrId::SLLV: return "SLLV";
        case InstrId::SRLV: return "SRLV";
        case InstrId::SRAV: return "SRAV";

        // Mul/Div
        case InstrId::MULT:  return "MULT";
        case InstrId::MULTU: return "MULTU";
        case InstrId::DIV:   return "DIV";
        case InstrId::DIVU:  return "DIVU";
        case InstrId::MFHI:  return "MFHI";
        case InstrId::MTHI:  return "MTHI";
        case InstrId::MFLO:  return "MFLO";
        case InstrId::MTLO:  return "MTLO";

        // I-type ALU
        case InstrId::ADDI:  return "ADDI";
        case InstrId::ADDIU: return "ADDIU";
        case InstrId::ANDI:  return "ANDI";
        case InstrId::ORI:   return "ORI";
        case InstrId::XORI:  return "XORI";
        case InstrId::SLTI:  return "SLTI";
        case InstrId::SLTIU: return "SLTIU";
        case InstrId::LUI:   return "LUI";

        // Loads
        case InstrId::LB:  return "LB";
        case InstrId::LBU: return "LBU";
        case InstrId::LH:  return "LH";
        case InstrId::LHU: return "LHU";
        case InstrId::LW:  return "LW";
        case InstrId::LWL: return "LWL";
        case InstrId::LWR: return "LWR";

        // Stores
        case InstrId::SB:  return "SB";
        case InstrId::SH:  return "SH";
        case InstrId::SW:  return "SW";
        case InstrId::SWL: return "SWL";
        case InstrId::SWR: return "SWR";

        // Branches
        case InstrId::BEQ:    return "BEQ";
        case InstrId::BNE:    return "BNE";
        case InstrId::BLEZ:   return "BLEZ";
        case InstrId::BGTZ:   return "BGTZ";
        case InstrId::BLTZ:   return "BLTZ";
        case InstrId::BGEZ:   return "BGEZ";
        case InstrId::BLTZAL: return "BLTZAL";
        case InstrId::BGEZAL: return "BGEZAL";

        // Jumps
        case InstrId::J:    return "J";
        case InstrId::JAL:  return "JAL";
        case InstrId::JR:   return "JR";
        case InstrId::JALR: return "JALR";

        // System
        case InstrId::SYSCALL: return "SYSCALL";
        case InstrId::BREAK:   return "BREAK";

        // COP0
        case InstrId::MFC0: return "MFC0";
        case InstrId::MTC0: return "MTC0";
        case InstrId::RFE:  return "RFE";

        // COP2 register moves
        case InstrId::MFC2: return "MFC2";
        case InstrId::MTC2: return "MTC2";
        case InstrId::CFC2: return "CFC2";
        case InstrId::CTC2: return "CTC2";
        case InstrId::LWC2: return "LWC2";
        case InstrId::SWC2: return "SWC2";

        // GTE commands
        case InstrId::GTE_RTPS:  return "RTPS";
        case InstrId::GTE_NCLIP: return "NCLIP";
        case InstrId::GTE_OP:    return "OP";
        case InstrId::GTE_DPCS:  return "DPCS";
        case InstrId::GTE_INTPL: return "INTPL";
        case InstrId::GTE_MVMVA: return "MVMVA";
        case InstrId::GTE_NCDS:  return "NCDS";
        case InstrId::GTE_CDP:   return "CDP";
        case InstrId::GTE_NCDT:  return "NCDT";
        case InstrId::GTE_NCCS:  return "NCCS";
        case InstrId::GTE_CC:    return "CC";
        case InstrId::GTE_NCS:   return "NCS";
        case InstrId::GTE_NCT:   return "NCT";
        case InstrId::GTE_SQR:   return "SQR";
        case InstrId::GTE_DCPL:  return "DCPL";
        case InstrId::GTE_DPCT:  return "DPCT";
        case InstrId::GTE_AVSZ3: return "AVSZ3";
        case InstrId::GTE_AVSZ4: return "AVSZ4";
        case InstrId::GTE_RTPT:  return "RTPT";
        case InstrId::GTE_GPF:   return "GPF";
        case InstrId::GTE_GPL:   return "GPL";
        case InstrId::GTE_NCCT:  return "NCCT";

        default: return "UNKNOWN";
    }
}

std::string_view MipsDecoder::categoryName(InstrCategory cat) {
    switch (cat) {
        case InstrCategory::ALU:     return "ALU";
        case InstrCategory::Memory:  return "Memory";
        case InstrCategory::Branch:  return "Branch";
        case InstrCategory::Jump:    return "Jump";
        case InstrCategory::MulDiv:  return "MulDiv";
        case InstrCategory::System:  return "System";
        case InstrCategory::COP0:    return "COP0";
        case InstrCategory::GTE:     return "GTE";
        case InstrCategory::Special: return "Special";
        default: return "Unknown";
    }
}

// Register Names

static constexpr const char* s_gprNames[32] = {
    "$zero", "$at", "$v0", "$v1",
    "$a0",   "$a1", "$a2", "$a3",
    "$t0",   "$t1", "$t2", "$t3",
    "$t4",   "$t5", "$t6", "$t7",
    "$s0",   "$s1", "$s2", "$s3",
    "$s4",   "$s5", "$s6", "$s7",
    "$t8",   "$t9", "$k0", "$k1",
    "$gp",   "$sp", "$fp", "$ra"
};

std::string_view gprName(uint8_t reg) {
    if (reg >= 32) return "$??";
    return s_gprNames[reg];
}

static constexpr const char* s_cop0Names[32] = {
    "Index",    "Random", "EntryLo",  "BPC",
    "Context",  "BDA",    "TAR",      "DCIC",
    "BadVAddr", "BDAM",   "EntryHi",  "BPCM",
    "SR",       "Cause",  "EPC",      "PRId",
    "cop0_16",  "cop0_17","cop0_18",  "cop0_19",
    "cop0_20",  "cop0_21","cop0_22",  "cop0_23",
    "cop0_24",  "cop0_25","cop0_26",  "cop0_27",
    "cop0_28",  "cop0_29","cop0_30",  "cop0_31"
};

std::string_view cop0Name(uint8_t reg) {
    if (reg >= 32) return "cop0_??";
    return s_cop0Names[reg];
}

} // namespace ps1recomp
