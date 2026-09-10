#include "runtime/bios_access.h"

#include "runtime/bios/bios.h"

namespace ps1 {

void runtimeBiosDrainPendingCallbacks(bios::Bios *bios) {
  bios->drainPendingCallbacks();
}

void runtimeBiosExecuteA0(bios::Bios *bios) { bios->executeA0(); }

void runtimeBiosExecuteB0(bios::Bios *bios) { bios->executeB0(); }

void runtimeBiosExecuteC0(bios::Bios *bios) { bios->executeC0(); }

} // namespace ps1
