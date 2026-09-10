#pragma once
/**
 * @file psyq_registry.h
 * @brief Name-keyed registry for PsyQ HLE implementations.
 *
 * The recompiler emits, for every `[[hle_functions]]` entry produced by
 * ps1Analyzer (Sessao 0.4+), a stub of the form:
 *
 *   void func_801ABCDE(uint8_t* rdram, recomp_context* ctx) {
 *       psyq_dispatch("libetc_VSync", ctx);
 *   }
 *
 * Resolution happens at runtime via this registry instead of via linker
 * symbols, so the build does not break when the analyzer detects PsyQ
 * functions for which no C++ implementation exists yet -- those calls fail
 * loudly at runtime with the function name and return address.
 *
 * Names follow the canonical `<library>_<basename>` convention from
 * `ps1Analyzer/data/psyq_signatures.toml` (e.g., `libetc_VSync`,
 * `libgpu_PutDispEnv`). They are matched by C-string equality.
 */

#include "runtime/cpu_context.h"

using PsyqHleFn = void (*)(recomp_context *);

/// Register an HLE implementation under the canonical PsyQ name.
/// Re-registering an existing name overwrites the previous binding.
void psyq_register(const char *name, PsyqHleFn fn);

/// Dispatch to a registered HLE by name.
/// If no implementation is registered, prints a fatal diagnostic that
/// includes the call's return address (`ctx->r[31]`) and aborts.
void psyq_dispatch(const char *name, recomp_context *ctx);

/// Register the built-in HLE implementations that ship with ps1Runtime.
/// Idempotent -- repeated calls are no-ops after the first.
/// Call once from `main_host.cpp` after `ps1::psyq::configure(...)`.
void psyq_registry_init_defaults();

/// Register the boot-path libapi/libcd/libgte/libgpu HLEs that the hash
/// matcher detects in Rayman.  Adds ~30 entries on top of the 10 builtins
/// from `psyq_registry_init_defaults()`.  Idempotent: later `psyq_register`
/// calls overwrite earlier slots, so re-registering is safe.
void psyq_register_rayman_boot();
