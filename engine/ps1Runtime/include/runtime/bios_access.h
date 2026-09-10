#pragma once
#include <cstdint>

namespace ps1::bios {
class Bios;
}

namespace ps1 {

// Narrow generated-code ABI.  The definitions live beside the BIOS and keep
// its Memory/device implementation out of the generated translation unit.
void runtimeBiosDrainPendingCallbacks(bios::Bios *bios, uint32_t resumePc = 0);
void runtimeBiosExecuteA0(bios::Bios *bios);
void runtimeBiosExecuteB0(bios::Bios *bios);
void runtimeBiosExecuteC0(bios::Bios *bios);

} // namespace ps1
