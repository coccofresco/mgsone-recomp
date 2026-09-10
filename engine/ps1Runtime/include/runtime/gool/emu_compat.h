#pragma once
/**
 * @file emu_compat.h
 * @brief Compatibility shim that lets the hand-decompiled GOOL engine
 *        (ported from the `c1c` reference, which targets a threaded
 *        PS1 interpreter) run on top of this project's *static*
 *        recompilation runtime.
 *
 * The `c1c` GOOL sources express every PS1-memory access and every
 * callout to a native PS1 routine through a small primitive vocabulary
 * (`EMU_ReadU32`, `EMU_Write32`, `EMUADDR`, `EMU_Invoke`, ...). Those
 * primitives are defined here against the facilities this runtime
 * already has:
 *
 *   - reads/writes  -> `recomp_context::mem->read32 / write32` (full
 *                      path: RAM, scratchpad, MMIO)
 *   - host<->PS1    -> `ps1::emuptr_translate` and its inverse
 *   - native call   -> `recomp_dispatch` (the static-recomp dispatcher)
 *
 * **Key difference from the c1c reference.** `c1c::EMU_Invoke` drives a
 * threaded program-counter loop with a `0xDEADBEEF` return sentinel
 * because c1c interprets. Here every recompiled MIPS function is a real
 * C++ function that returns at its `JR RA`, so `EMU_Invoke` simply sets
 * up the argument registers, calls `recomp_dispatch`, and reads `V0`.
 *
 * **Binding.** The `EMU_*` functions are free functions but operate on
 * the active `recomp_context`. The GOOL interpreter override binds it on
 * entry via `emu_bind()` and clears it on return via `emu_unbind()`.
 * Binding is thread-local: only the game thread runs GOOL.
 */

#include <cstdarg>
#include <cstdint>

struct recomp_context;

namespace ps1::gool {

/// Bind the active CPU context + RAM base for the duration of one GOOL
/// interpretation. Call on override entry; pair with `emu_unbind()`.
void emu_bind(uint8_t *rdram, recomp_context *ctx) noexcept;
void emu_unbind() noexcept;

/// Access to the bound context (asserts in debug if unbound). Exposed so
/// the ported GOOL translation unit can define the bare-name MIPS
/// register macros (A0, V0, SP, RA, LO, HI, ...) without pulling in the
/// `ps1::Register` enum, which would collide with those macro names.
recomp_context *emu_ctx() noexcept;
uint32_t &emu_reg(int index) noexcept; // index: 0..31 GPR, 32=LO, 33=HI

// --- Memory primitives (route through Memory: RAM/scratchpad/MMIO) ----

uint8_t EMU_ReadU8(uint32_t address) noexcept;
uint16_t EMU_ReadU16(uint32_t address) noexcept;
uint32_t EMU_ReadU32(uint32_t address) noexcept;
int8_t EMU_ReadS8(uint32_t address) noexcept;
int16_t EMU_ReadS16(uint32_t address) noexcept;

void EMU_Write8(uint32_t address, uint8_t value) noexcept;
void EMU_Write16(uint32_t address, uint16_t value) noexcept;
void EMU_Write32(uint32_t address, uint32_t value) noexcept;

// --- Host pointer <-> PS1 address -------------------------------------

/// Inverse of `emuptr_translate`: maps a host pointer that lives inside
/// the bound RAM buffer back to its PS1 (KSEG0) virtual address. 64-bit
/// safe, unlike the c1c macro which truncates the pointer to uint32_t.
uint32_t EMU_Address(const void *host_ptr) noexcept;

// --- Native callout ---------------------------------------------------

/// Call the recompiled PS1 function at `address` with `argc` arguments.
/// Args 0..3 go in A0..A3; args 4+ are written to the reserved outgoing
/// stack slots (o32 ABI), matching the c1c reference. Returns V0.
uint32_t EMU_Invoke(uint32_t address, int argc, ...) noexcept;

// --- HI/LO mul-div helpers (mirror c1c::EMU_*Multiply / *Divide) ------

void EMU_SMultiply(int32_t a, int32_t b) noexcept;
void EMU_UMultiply(uint32_t a, uint32_t b) noexcept;
void EMU_SDivide(int32_t a, int32_t b) noexcept;
void EMU_UDivide(uint32_t a, uint32_t b) noexcept;

/// Debug breakpoint hook in the original engine. No-op here (logged once
/// under PS1_GOOL_TRACE).
void EMU_Break(uint32_t id) noexcept;

} // namespace ps1::gool

/// 64-bit-safe replacement for the c1c `EMUADDR(x)` macro. The original
/// is `EMU_Address((uint32_t)(x))`, which truncates a host pointer on a
/// 64-bit build; this passes the full pointer.
#define EMUADDR(x) ::ps1::gool::EMU_Address((const void *)(x))
