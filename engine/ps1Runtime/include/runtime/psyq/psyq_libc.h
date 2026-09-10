#pragma once
/**
 * @file psyq_libc.h
 * @brief HLE for the PsyQ libc subset (Group 1.F).
 *
 * The PsyQ SDK ships its own copies of `memcpy`, `memset`, `strcpy`,
 * `printf`, etc. statically linked into LIBAPI/LIBGPU/LIBCD/LIBETC etc., so
 * the same symbol may surface under several library prefixes depending on
 * which `.LIB` the linker pulled it from.  Each handler here is registered
 * under every plausible `lib<X>_<name>` alias plus the canonical `libc_<name>`
 * to keep the registry future-proof when new games detect new prefixes.
 *
 * **Consolidation strategy.** `bios.cpp` already implements byte-for-byte
 * `memcpy`/`memset`/`strcpy`/`strlen`/etc. as A0 syscalls (A0:0x10..0x2D).
 * The HLEs in this module forward to those tables via the same `dispatchA`
 * pattern used by `psyq_libapi.cpp` -- no logic duplication, single source of
 * truth in `bios.cpp`.  Functions without a matching A0 stub (`atoi`,
 * `printf`, `sprintf`) are implemented directly here.
 *
 * **`printf` semantics.** Real PsyQ `printf` writes to the SIO debug port
 * unconditionally; for our headless runtime we route to host stdout but
 * gate on `PS1_BIOS_DEBUG=1` to match the convention `bios.cpp`'s
 * `BIOS_LOG` macro already established.  This keeps production runs quiet
 * while leaving instrumented `printf` output available for debugging.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

// Memory routines (A0:0x2A..0x2D)
void hle_libc_memcpy(recomp_context *ctx);   ///< A0:0x2A -- memcpy(dst, src, n)
void hle_libc_memset(recomp_context *ctx);   ///< A0:0x2B -- memset(dst, c, n)
void hle_libc_memmove(recomp_context *ctx);  ///< A0:0x2C -- memmove(dst, src, n)
void hle_libc_memcmp(recomp_context *ctx);   ///< A0:0x2D -- memcmp(s1, s2, n)

// String routines (A0:0x15..0x1B, psx-spx/openbios layout)
void hle_libc_strcat(recomp_context *ctx);   ///< A0:0x15 -- strcat(dst, src)
void hle_libc_strncat(recomp_context *ctx);  ///< A0:0x16 -- strncat(dst, src, n)
void hle_libc_strcmp(recomp_context *ctx);   ///< A0:0x17 -- strcmp(s1, s2)
void hle_libc_strncmp(recomp_context *ctx);  ///< A0:0x18 -- strncmp(s1, s2, n)
void hle_libc_strcpy(recomp_context *ctx);   ///< A0:0x19 -- strcpy(dst, src)
void hle_libc_strncpy(recomp_context *ctx);  ///< A0:0x1A -- strncpy(dst, src, n)
void hle_libc_strlen(recomp_context *ctx);   ///< A0:0x1B -- strlen(s)

// Math / RNG
void hle_libc_abs(recomp_context *ctx);      ///< A0:0x10 -- abs(x)
void hle_libc_labs(recomp_context *ctx);     ///< A0:0x11 -- labs(x)
void hle_libc_rand(recomp_context *ctx);     ///< A0:0x2F -- rand()
void hle_libc_srand(recomp_context *ctx);    ///< A0:0x30 -- srand(seed)

// Standalone (no matching BIOS syscall)

/// `atoi(s)` -- parse leading optional sign + decimal digits; ignores
/// leading whitespace.  Returns 0 on parse failure (matches libc).
void hle_libc_atoi(recomp_context *ctx);

/// `printf(fmt, ...)` -- formatted host-stdout debug output.  No-op unless
/// `PS1_BIOS_DEBUG=1` is set in the environment.  Returns the number of
/// characters that *would have been* written (matches C-libc semantics)
/// regardless of the gate.
void hle_libc_printf(recomp_context *ctx);

/// `sprintf(buf, fmt, ...)` -- formatted write to PS1 RAM at `buf`.  Always
/// runs (the gate only suppresses host stdout).  Returns the number of
/// characters written, excluding the trailing NUL.
void hle_libc_sprintf(recomp_context *ctx);

/// Reset RNG state.  Test-only hook so `rand`/`srand` tests are
/// deterministic across invocations.
void psyq_libc_reset_for_tests();

/// Register every libc HLE in this header into the runtime registry, under
/// the canonical `libc_<name>` plus every observed `lib<X>_<name>` alias.
void psyq_register_libc();

} // namespace ps1::psyq
