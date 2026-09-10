#include "bios_internal.h"
#include "runtime/bios/bios.h"

namespace ps1::bios {

// PS1 BIOS Table C (0xC0) -- kernel-mode interrupt chain, exception handler
// installation, root-counter clear flags. Dispatched from Bios::executeC0()
// in bios.cpp.

void Bios::handleC0(uint32_t index) {
  switch (index) {
  case 0x00: // InstallExceptionHandler -- stub
    BIOS_LOG("[BIOS] InstallExceptionHandler() [STUB]\n");
    break;
  case 0x01: // SysEnqIntRP
    // Registers a handler in the interrupt priority chain.
    // On a real PS1 this would add to the exception dispatch chain at
    // the given priority level.  In our HLE, we deliver events directly
    // from the main thread, so the handler chain isn't needed.
    // We still acknowledge the call properly (return 0 = success).
    BIOS_LOG("[BIOS] SysEnqIntRP(priority: {}, handler: 0x{:08X})\n",
               ctx_.r[A0], ctx_.r[A1]);
    ctx_.r[V0] = 0;
    break;
  case 0x02: // SysDeqIntRP
    // Removes a handler from the interrupt priority chain.
    BIOS_LOG("[BIOS] SysDeqIntRP(priority: {}, handler: 0x{:08X})\n",
               ctx_.r[A0], ctx_.r[A1]);
    ctx_.r[V0] = 0;
    break;
  case 0x03: // SysInitMemory
    BIOS_LOG("[BIOS] SysInitMemory(addr: 0x{:08X}, size: {}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x07: // InstallExceptionHandlers
    BIOS_LOG("[BIOS] InstallExceptionHandlers() [STUB]\n");
    break;
  case 0x08: // SysInitKMem
    BIOS_LOG("[BIOS] SysInitKMem() [STUB]\n");
    break;
  case 0x0A: // ChangeClearRCnt
    // Controls whether root counter interrupts auto-clear after firing.
    // t (A0) = root counter index (0-3), flag (A1) = 0 or 1
    // Returns the old flag value.  We always return 0 (was auto-clear).
    // PsyQ VSync depends on this being called during init to set up RCnt3.
    BIOS_LOG("[BIOS] ChangeClearRCnt(t: {}, flag: {})\n", ctx_.r[A0],
               ctx_.r[A1]);
    ctx_.r[V0] = 0; // return old value
    break;
  case 0x0C: // InitDefInt
    BIOS_LOG("[BIOS] InitDefInt(priority: {}) [STUB]\n", ctx_.r[A0]);
    break;
  default:
    BIOS_LOG("[BIOS] Unimplemented C0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

} // namespace ps1::bios
