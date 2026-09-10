#pragma once

#include <cstdint>

namespace ps1 {

struct CPUContext;

// Stable construction identities for host implementations of GTE setters.
// These odd tags cannot alias an aligned MIPS instruction address. They share
// the existing gteControlWriterPc metadata carrier, not the architectural PC.
// Trace consumers must treat these values as host operations, not guest PCs.
enum class HostGteControlWriter : uint32_t {
  PsyqSetRotationMatrix = 1u,
  PsyqSetTranslationMatrix = 5u,
};

// Same register semantics/source tracking as GTE::writeControl, but no guest
// instruction is being executed. In particular the stale ctx->pc must neither
// name this write nor trigger an instruction-specific game hook. No guest PC,
// register, timer or input is changed beyond the requested GTE control write.
void writeGteControlFromHost(CPUContext *ctx, uint8_t reg, uint32_t value,
                             HostGteControlWriter writer,
                             uint32_t sourceAddress = UINT32_MAX);

} // namespace ps1
