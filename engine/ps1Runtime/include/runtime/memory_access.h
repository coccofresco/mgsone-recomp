#pragma once

#include "runtime/memory.h"

namespace ps1 {

// Stable generated-code front end. Memory's RAM/scratchpad/BIOS paths remain
// inline here; its concrete device graph lives behind cold MMIO methods in the
// runtime library. Thus renderer header edits do not rebuild the generated
// game and ordinary guest loads/stores do not pay an extra ABI call.
inline uint8_t runtimeMemoryRead8(const Memory *memory, uint32_t address) {
  return memory->read8(address);
}

inline uint16_t runtimeMemoryRead16(const Memory *memory, uint32_t address) {
  return memory->read16(address);
}

inline uint32_t runtimeMemoryRead32(const Memory *memory, uint32_t address) {
  return memory->read32(address);
}

inline void runtimeMemoryWrite8(Memory *memory, uint32_t address,
                                uint8_t value) {
  memory->write8(address, value);
}

inline void runtimeMemoryWrite16(Memory *memory, uint32_t address,
                                 uint16_t value) {
  memory->write16(address, value);
}

inline void runtimeMemoryWrite32(Memory *memory, uint32_t address,
                                 uint32_t value) {
  memory->write32(address, value);
}

inline bool runtimeMemoryLookupPrecision(const Memory *memory,
                                         uint32_t address, uint32_t value,
                                         PGXPValue &precision) {
  return memory->lookupPrecision(address, value, precision);
}

[[gnu::always_inline]] inline bool
runtimeMemoryMightHavePrecision(const Memory *memory, uint32_t address) {
  return memory->mightHavePrecision(address);
}

inline void runtimeMemoryWrite32Tracked(Memory *memory, uint32_t address,
                                        uint32_t value,
                                        const PGXPValue *precision) {
  memory->write32Tracked(address, value, precision);
}

} // namespace ps1
