#pragma once

#include <cstdint>

struct recomp_context;

namespace ps1::mips {

// Identity of the outermost compatibility-interpreter boundary currently
// executing on this thread. Static executable text is identified by its
// exact entry and immutable owner range; dynamic heap code is deliberately
// labelled separately because its address alone is not a stable identity.
// This is diagnostic/admission metadata only and never affects guest state.
struct InterpreterSliceIdentity {
  uint32_t entry = 0;
  uint32_t ownerStart = 0;
  uint32_t ownerEnd = 0;
  bool active = false;
  bool dynamic = false;
};

InterpreterSliceIdentity currentInterpreterSliceIdentity() noexcept;

// Crash 2/3 GOOL entries can contain short native R3000A blocks. They are
// copied into the game heap, so their addresses cannot be known by the static
// recompiler. Keep the fallback opt-in and range-limited: ordinary missing
// functions must remain hard failures during bring-up.
void configureDynamicInterpreter(bool enabled, uint32_t minAddress,
                                 uint32_t maxAddress) noexcept;
bool dynamicInterpreterEnabled() noexcept;

// Execute an unmapped guest call until it returns to the incoming $ra.
// Returns false when the address does not look like a dynamic native block or
// an unsupported instruction is encountered; the dispatcher can then retain
// its normal fatal diagnostic.
bool interpretDynamicCall(uint8_t *rdram, recomp_context *ctx,
                          uint32_t address);

// Execute a statically loaded compatibility island. Unlike the dynamic GOOL
// entry path, this does not scan for a nearby return signature: the generator
// has already proven that the entry belongs to the configured text region.
// Direct and indirect transfers that remain inside [minAddress, maxAddress)
// stay in the interpreter, including private labels that are not standalone
// functions. Calls outside the island use recomp_dispatch and then resume.
bool interpretStaticCall(uint8_t *rdram, recomp_context *ctx,
                         uint32_t address, uint32_t minAddress,
                         uint32_t maxAddress);

// Execute an indirect entry that lands inside a statically known function
// body but has no standalone recompiled symbol. The owner range is generated
// from the analyzer configuration, so arbitrary RAM/data addresses remain a
// hard dispatcher failure.
bool interpretStaticPrivateCall(uint8_t *rdram, recomp_context *ctx,
                                uint32_t address, uint32_t ownerStart,
                                uint32_t ownerEnd);

// An interpreter-owned J/JR which leaves its current compatibility island is
// an architectural tail transfer, not a host C++ call. Publish it back to the
// generated dispatcher so that dispatcher can iterate without growing the
// native stack once per private label. The value is thread-local because all
// guest execution for one machine is serialized on its game thread.
bool takeDeferredTailDispatch(uint32_t &address) noexcept;

} // namespace ps1::mips
