#pragma once

// ps1Recomp -- GTE Emitter
// Generates C++ code for COP2/GTE (Geometry Transformation Engine) operations.
// GTE has 64 registers (32 data + 32 control) and 22 commands.
// The emitter generates calls to runtime GTE functions.

#include "mips_decoder.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace ps1recomp {

// GTE Register Names

/// Data register names (cop2d, 0-31)
std::string_view gteDataRegName(uint8_t reg);

/// Control register names (cop2c, 0-31)
std::string_view gteControlRegName(uint8_t reg);

// GTE Command Info

struct GteCommandInfo {
    InstrId     id;
    uint8_t     cmd;        // COP2 funct bits (0-5)
    const char* name;       // Mnemonic (RTPS, NCLIP, etc.)
    const char* description;
    int         cycles;     // Hardware cycle count
};

/// Get info about a GTE command by InstrId
const GteCommandInfo* getGteCommandInfo(InstrId id);

// GTE Emitter

class GteEmitter {
public:
    /// Emit C++ code for a GTE instruction (register move or command)
    /// @param inst  Decoded instruction (must be GTE category)
    /// @param pc    Address of this instruction
    /// @return C++ code string
    std::string emit(const Instruction& inst, uint32_t pc) const;

    /// Emit a GTE register move (MFC2/MTC2/CFC2/CTC2/LWC2/SWC2)
    std::string emitRegisterMove(const Instruction& inst) const;

    /// Emit a GTE command (RTPS, NCLIP, MVMVA, etc.)
    std::string emitCommand(const Instruction& inst) const;

    /// Emit MVMVA with decoded matrix/vector/translation fields
    std::string emitMVMVA(uint32_t raw) const;
};

} // namespace ps1recomp
