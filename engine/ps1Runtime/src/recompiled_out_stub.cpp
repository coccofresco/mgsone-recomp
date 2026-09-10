// recompiled_out_stub.cpp -- CI / test build placeholder
//
// Used by CMake when the real recompiled_out.cpp has not been generated.
// The real file is produced by `ps1Recomp <config.toml> <output.cpp>`, which
// translates the game's MIPS instructions to C++.
//
// This stub satisfies the linker for unit tests and CI pipelines that do not
// run a game binary.

#include <cstdint>
#include <runtime/cpu_context.h>

void recomp_init_dispatch_table() {
    // no-op: no game loaded
}

void recomp_dispatch(uint8_t* /*rdram*/, recomp_context* /*ctx*/, uint32_t /*addr*/) {
    // no-op: no game loaded
}
