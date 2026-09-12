#pragma once
#include <cstdint>
struct recomp_context;
namespace ps1 {
class Memory;
}
namespace ps1::games::mgs {
inline constexpr uint32_t UsaTextureTable = 0x800B46C8;
void configureDecompiled(bool enabled) noexcept;
// Shared source implementation for the game and presentation. A read-only
// query returns a guest address or zero; it never invokes an AOT routine.
uint32_t findSourceTexture(const Memory &memory, uint16_t id);
// Declining is atomic: no guest data or CPU state has been modified. Restored
// interior continuations and unported USA exceptional behavior stay in AOT.
bool tryDecompiledCall(recomp_context *ctx, uint32_t entry);
} // namespace ps1::games::mgs
bool mgsTryDecompiled(recomp_context *ctx, uint32_t entry);
