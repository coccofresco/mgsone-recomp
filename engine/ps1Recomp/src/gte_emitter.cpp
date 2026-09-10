// ps1Recomp -- GTE Emitter Implementation
// Generates C++ for COP2/GTE instructions.
// Each GTE command maps to a runtime function call: gte_RTPS(ctx), etc.
// Register moves access ctx->cop2d[] and ctx->cop2c[] arrays.

#include "ps1recomp/gte_emitter.h"
#include <fmt/format.h>
#include <array>

namespace ps1recomp {

// GTE Data Register Names (cop2d[0..31])

static constexpr const char* s_gteDataRegs[32] = {
    "VXY0",  "VZ0",   "VXY1",  "VZ1",     // 0-3:  Vectors V0, V1
    "VXY2",  "VZ2",   "RGBC",  "OTZ",     // 4-7:  Vector V2, Color, OrderTableZ
    "IR0",   "IR1",   "IR2",   "IR3",     // 8-11: Intermediate results
    "SXY0",  "SXY1",  "SXY2",  "SXYP",   // 12-15: Screen XY FIFO + push
    "SZ0",   "SZ1",   "SZ2",   "SZ3",     // 16-19: Screen Z FIFO
    "RGB0",  "RGB1",  "RGB2",  "RES1",    // 20-23: Color FIFO + reserved
    "MAC0",  "MAC1",  "MAC2",  "MAC3",    // 24-27: Multiply-accumulate results
    "IRGB",  "ORGB",  "LZCS",  "LZCR"    // 28-31: Color conversion + CLZ
};

std::string_view gteDataRegName(uint8_t reg) {
    if (reg >= 32) return "gte_d??";
    return s_gteDataRegs[reg];
}

// GTE Control Register Names (cop2c[0..31])

static constexpr const char* s_gteControlRegs[32] = {
    "RT11RT12", "RT13RT21", "RT22RT23", "RT31RT32",   // 0-3:   Rotation matrix
    "RT33",     "TRX",      "TRY",      "TRZ",        // 4-7:   Rotation + Translation
    "L11L12",   "L13L21",   "L22L23",   "L31L32",     // 8-11:  Light source matrix
    "L33",      "RBK",      "GBK",      "BBK",        // 12-15: Light + Background color
    "LR1LR2",   "LR3LG1",   "LG2LG3",   "LB1LB2",   // 16-19: Light color matrix
    "LB3",      "RFC",      "GFC",      "BFC",        // 20-23: Light color + Far color
    "OFX",      "OFY",      "H",        "DQA",        // 24-27: Screen offset, projection, depth
    "DQB",      "ZSF3",     "ZSF4",     "FLAG"        // 28-31: Depth q, Z scale, flags
};

std::string_view gteControlRegName(uint8_t reg) {
    if (reg >= 32) return "gte_c??";
    return s_gteControlRegs[reg];
}

// GTE Command Table

static const std::array<GteCommandInfo, 22> s_gteCommands = {{
    { InstrId::GTE_RTPS,   0x01, "RTPS",  "Perspective transform (single)",          15 },
    { InstrId::GTE_NCLIP,  0x06, "NCLIP", "Normal clipping",                          8 },
    { InstrId::GTE_OP,     0x0C, "OP",    "Outer product",                             6 },
    { InstrId::GTE_DPCS,   0x10, "DPCS",  "Depth cue (single)",                        8 },
    { InstrId::GTE_INTPL,  0x11, "INTPL", "Interpolation",                              8 },
    { InstrId::GTE_MVMVA,  0x12, "MVMVA", "Matrix-vector multiply + add",             8 },
    { InstrId::GTE_NCDS,   0x13, "NCDS",  "Normal color depth (single)",              19 },
    { InstrId::GTE_CDP,    0x14, "CDP",   "Color depth cue",                           13 },
    { InstrId::GTE_NCDT,   0x16, "NCDT",  "Normal color depth (triple)",              44 },
    { InstrId::GTE_NCCS,   0x1B, "NCCS",  "Normal color color (single)",              17 },
    { InstrId::GTE_CC,     0x1C, "CC",    "Color color",                               11 },
    { InstrId::GTE_NCS,    0x1E, "NCS",   "Normal color (single)",                    14 },
    { InstrId::GTE_NCT,    0x20, "NCT",   "Normal color (triple)",                    30 },
    { InstrId::GTE_SQR,    0x28, "SQR",   "Square of vector",                          5 },
    { InstrId::GTE_DCPL,   0x29, "DCPL",  "Depth cue color light",                     8 },
    { InstrId::GTE_DPCT,   0x2A, "DPCT",  "Depth cue (triple)",                       17 },
    { InstrId::GTE_AVSZ3,  0x2D, "AVSZ3", "Average Z (3 values)",                     5 },
    { InstrId::GTE_AVSZ4,  0x2E, "AVSZ4", "Average Z (4 values)",                     6 },
    { InstrId::GTE_RTPT,   0x30, "RTPT",  "Perspective transform (triple)",           23 },
    { InstrId::GTE_GPF,    0x3D, "GPF",   "General purpose interpolation",             5 },
    { InstrId::GTE_GPL,    0x3E, "GPL",   "General purpose interpolation + base",     5 },
    { InstrId::GTE_NCCT,   0x3F, "NCCT",  "Normal color color (triple)",              39 },
}};

const GteCommandInfo* getGteCommandInfo(InstrId id) {
    for (const auto& cmd : s_gteCommands) {
        if (cmd.id == id) return &cmd;
    }
    return nullptr;
}

// Helpers

static std::string gteReg(uint8_t r) {
    return fmt::format("ctx->r{}", r);
}

// GTE Register Moves

std::string GteEmitter::emitRegisterMove(const Instruction& inst) const {
    switch (inst.id) {
        case InstrId::MFC2:
            return fmt::format("pgxp_invalidate_gpr(ctx, {}); {} = pgxp_mfc2(ctx, {}, {}); // {}",
                               inst.rt, gteReg(inst.rt), inst.rt, inst.rd,
                               gteDataRegName(inst.rd));
        case InstrId::MTC2:
            return fmt::format("pgxp_mtc2(ctx, {}, {}, {}); // {}", inst.rd,
                               inst.rt, gteReg(inst.rt), gteDataRegName(inst.rd));
        case InstrId::CFC2:
            return fmt::format("pgxp_invalidate_gpr(ctx, {}); {} = gte_read_control(ctx, {}); // {}",
                               inst.rt, gteReg(inst.rt), inst.rd,
                               gteControlRegName(inst.rd));
        case InstrId::CTC2:
            return fmt::format("gte_write_control(ctx, {}, {}); // {}",
                               inst.rd, gteReg(inst.rt), gteControlRegName(inst.rd));
        case InstrId::LWC2:
            return fmt::format("pgxp_lwc2(ctx, {}, (int32_t)({}) + {}); // {}",
                               inst.rt, gteReg(inst.rs), inst.imm16,
                               gteDataRegName(inst.rt));
        case InstrId::SWC2:
            return fmt::format("pgxp_swc2(ctx, {}, (int32_t)({}) + {}); // {}",
                               inst.rt, gteReg(inst.rs), inst.imm16,
                               gteDataRegName(inst.rt));
        default:
            return fmt::format("// UNKNOWN GTE MOVE: {}", MipsDecoder::instrName(inst.id));
    }
}

// GTE MVMVA special handling
// MVMVA has flags encoded in the instruction word:
//   bits 17-18: multiply matrix (0=Rotation, 1=Light, 2=Color, 3=Reserved)
//   bits 15-16: multiply vector (0=V0, 1=V1, 2=V2, 3=IR)
//   bit  13:    translation vector (0=TR, 1=BK, 2=FC/Bugged, 3=None)

std::string GteEmitter::emitMVMVA(uint32_t raw) const {
    uint8_t mx = (raw >> 17) & 0x03;
    uint8_t mv = (raw >> 15) & 0x03;
    uint8_t tv = (raw >> 13) & 0x03;
    bool sf   = (raw >> 19) & 0x01;
    bool lm   = (raw >> 10) & 0x01;

    static const char* mxNames[] = { "Rotation", "Light", "Color", "Reserved" };
    static const char* mvNames[] = { "V0", "V1", "V2", "IR" };
    static const char* tvNames[] = { "TR", "BK", "FC", "None" };

    return fmt::format("gte_MVMVA(ctx, {}, {}, {}, {}, {}); // mx={}, mv={}, tv={}",
                       mx, mv, tv, sf ? 1 : 0, lm ? 1 : 0,
                       mxNames[mx], mvNames[mv], tvNames[tv]);
}

// GTE Commands

std::string GteEmitter::emitCommand(const Instruction& inst) const {
    // MVMVA gets special treatment due to instruction-encoded parameters
    if (inst.id == InstrId::GTE_MVMVA) {
        return emitMVMVA(inst.raw);
    }

    auto* info = getGteCommandInfo(inst.id);
    if (!info) {
        return fmt::format("// UNKNOWN GTE CMD: 0x{:02X}", inst.raw & 0x3F);
    }

    // Extract sf (shift 12 vs no shift) and lm (saturate to 0) flags
    bool sf = (inst.raw >> 19) & 0x01;
    bool lm = (inst.raw >> 10) & 0x01;

    return fmt::format("gte_{}(ctx, {}, {}); // {} ({} cycles)",
                       info->name, sf ? 1 : 0, lm ? 1 : 0,
                       info->description, info->cycles);
}

// Main Dispatch

std::string GteEmitter::emit(const Instruction& inst, uint32_t /*pc*/) const {
    if (inst.category != InstrCategory::GTE) {
        return fmt::format("// NOT GTE: {}", MipsDecoder::instrName(inst.id));
    }

    // Distinguish register moves from commands
    if (inst.id >= InstrId::GTE_RTPS && inst.id <= InstrId::GTE_NCCT) {
        return emitCommand(inst);
    }

    return emitRegisterMove(inst);
}

} // namespace ps1recomp
