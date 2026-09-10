#pragma once

// ps1Recomp -- HLE Stub Emitter
//
// When the analyzer config marks a function with `hle = true` (the
// `[[hle_functions]]` table emitted by ps1Analyzer Sessao 0.4), the
// recompiler skips MIPS translation for that function and instead emits
// a stub that delegates by name to the runtime PsyQ registry:
//
//     psyq_dispatch("<hleName>", ctx);
//
// `<hleName>` is the canonical `<library>_<basename>` identifier from
// `psyq_signatures.toml` (e.g., `libetc_VSync`, `libgpu_PutDispEnv`).
// Resolution happens at runtime, so missing implementations fail loudly
// (with the function name + return address) instead of breaking the build.

#include <cstdint>
#include <string>

namespace ps1recomp {

struct HleStub {
  uint32_t address = 0;     ///< PS1 virtual address of the function being stubbed
  std::string funcName;     ///< Recompiled C++ function name (e.g., "func_801ABCDE")
  std::string hleName;      ///< HLE backend identifier (e.g., "libetc_VSync")
};

/// Emit the single preamble forward-decl for `psyq_dispatch`.
/// Stub argument is unused (kept for API stability); the result is the
/// same string regardless of stub, so the recompiler should emit it once.
std::string emitHleForwardDecl(const HleStub &stub);

/// Emit the stub body that delegates to `psyq_dispatch("<hleName>", ctx)`.
std::string emitHleStub(const HleStub &stub);

} // namespace ps1recomp
