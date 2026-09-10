#pragma once
/**
 * @file dispatch_emitter.h
 * @brief Emits the body of the generated `recomp_dispatch` function.
 *
 * `recomp_dispatch` is the runtime resolver for every indirect call and jump
 * the recompiler could not bind statically. Its body is fixed text, so it
 * lives here rather than inline in the CLI: the policy it encodes -- above all
 * what happens when an address was never emitted -- is behaviour worth pinning
 * with tests.
 */

#include <string>

namespace ps1recomp {

/**
 * @brief Generate the C++ source of `recomp_dispatch`.
 *
 * Resolution order: NULL guard, direct lookup, KSEG-normalised retry, BIOS
 * entry points, BIOS table sentinels, JR-RA trampolines in RAM, and finally
 * the unmapped-target path.
 *
 * **Unmapped targets abort by default.** An address that reaches the end is
 * code the recompiler never emitted; continuing past it turns the miss into a
 * silent no-op, so the game runs on with corrupted state and the symptom
 * appears far from the cause. Aborting makes the failure point the cause.
 * `PS1_DISPATCH_PERMISSIVE=1` restores rate-limited logging for when you
 * deliberately want to see how far a knowingly-incomplete build gets.
 */
std::string emitDispatchBody();

} // namespace ps1recomp
