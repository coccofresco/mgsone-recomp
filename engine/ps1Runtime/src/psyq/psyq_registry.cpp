#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_font.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libapi.h"
#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_libc.h"
#include "runtime/psyq/psyq_libetc.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_libgte.h"
#include "runtime/psyq/psyq_pad.h"
#include "runtime/metrics.h"

#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

std::unordered_map<std::string, PsyqHleFn> &registry() {
  static std::unordered_map<std::string, PsyqHleFn> instance;
  return instance;
}

bool g_defaults_initialized = false;

} // namespace

void psyq_register(const char *name, PsyqHleFn fn) {
  registry()[name] = fn;
}

// Permissive mode: when env var PS1_HLE_PERMISSIVE=1 is set, missing HLE
// implementations log once-per-name and return a NOP instead of aborting.
// Useful for incremental group-by-group integration (e.g. measuring 1.A
// progress without the full 1.B/1.C/1.D coverage in place yet).
namespace {
bool isPermissive() {
  static const bool v = []() {
    const char *e = std::getenv("PS1_HLE_PERMISSIVE");
    return e && std::strcmp(e, "0") != 0 && e[0] != '\0';
  }();
  return v;
}
} // namespace

void psyq_dispatch(const char *name, recomp_context *ctx) {
  // Optional per-call trace gated by `PS1_HLE_TRACE=1`.  Single getenv
  // (cached) so the fast path is just a load+branch.
  static const bool s_trace = []() {
    const char *e = std::getenv("PS1_HLE_TRACE");
    return e && std::strcmp(e, "0") != 0 && e[0] != '\0';
  }();
  if (s_trace) {
    fmt::print(stderr, "[PSYQ] {} (a0={:08X} a1={:08X} a2={:08X} RA={:08X})\n",
               name, ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[31]);
  }

  auto &reg = registry();
  auto it = reg.find(name);
  if (it == reg.end()) {
    if (isPermissive()) {
      static std::unordered_set<std::string> warned;
      if (warned.insert(name).second)
        fmt::print(stderr,
                   "[PSYQ] WARN: function '{}' missing HLE -- NOP "
                   "(PS1_HLE_PERMISSIVE) RA=0x{:08X}\n",
                   name, ctx->r[31]);
      return;
    }
    fmt::print(stderr,
               "[PSYQ] FATAL: function '{}' marked HLE but no "
               "implementation registered (RA=0x{:08X})\n",
               name, ctx->r[31]);
    std::abort();
  }
  if (ps1::metrics::enabled()) {
    std::string key = "hle.";
    key += name;
    ps1::metrics::count(key);
  }
  it->second(ctx);
}

// Generic no-op stub for HLE entries that have no side-effect on early boot
// (e.g. root-counter start/stop, streamed-CD interrupt handlers).  Returns 0
// in v0 so callers that check status see "success".  Use sparingly -- if a
// game actually depends on the function's behaviour, a real implementation
// is needed instead.
static void hle_noop_stub(recomp_context *ctx) {
  ctx->r[ps1::V0] = 0;
}

// The runtime executes SPU DMA writes inline when the channel is started.
// Consequently there is no in-flight transfer by the time recompiled code
// can call SpuIsTransferCompleted(), even though the native PsyQ routine
// would still be waiting for its DMA-completion event callback.
static void hle_spu_is_transfer_completed(recomp_context *ctx) {
  ctx->r[ps1::V0] = 1;
}

void psyq_registry_init_defaults() {
  if (g_defaults_initialized)
    return;
  g_defaults_initialized = true;

  psyq_register("libetc_VSync",       &ps1::psyq::hle_VSync);
  psyq_register("libgpu_DrawSync",    &ps1::psyq::hle_DrawSync);
  psyq_register("libgpu_ResetGraph",  &ps1::psyq::hle_ResetGraph);
  psyq_register("libgpu_ClearOTag",   &ps1::psyq::hle_ClearOTag);
  psyq_register("libgpu_ClearOTagR",  &ps1::psyq::hle_ClearOTagR);
  psyq_register("libgpu_DrawOTag",    &ps1::psyq::hle_DrawOTag);
  psyq_register("libgpu_SetDefDispEnv", &ps1::psyq::hle_SetDefDispEnv);
  psyq_register("libgpu_PutDispEnv",  &ps1::psyq::hle_PutDispEnv);
  psyq_register("libgpu_SetDefDrawEnv", &ps1::psyq::hle_SetDefDrawEnv);
  psyq_register("libgpu_PutDrawEnv",  &ps1::psyq::hle_PutDrawEnv);

  // Minimal no-op stubs for early-boot HLEs Crash Bandicoot references
  // These have no side-effect during init.  Promote to real impl if a game
  // is observed waiting on their state (e.g. RCnt IRQs, OT linkage via
  // TermPrim, immediate-mode draws via DrawPrim).
  psyq_register("libapi_StopRCnt",       &ps1::psyq::hle_StopRCnt);
  psyq_register("libapi_StartRCnt",      &ps1::psyq::hle_StartRCnt);
  // Root-counter HLE entries are registered below once their MMIO-backed
  // implementations are selected. Do not replace them with per-game time
  // increments: the timer target and clock source are already guest inputs.
  psyq_register("libapi_SetRCnt",        &ps1::psyq::hle_SetRCnt);
  psyq_register("libcd_StCdInterrupt2",  &hle_noop_stub);
  psyq_register("libgpu__reset",         &hle_noop_stub);
  psyq_register("libgpu_TermPrim",       &hle_noop_stub);
  psyq_register("libgpu_DrawPrim",       &hle_noop_stub);
  psyq_register("libspu_SpuIsTransferCompleted",
                &hle_spu_is_transfer_completed);
  // VSyncCallbacks (plural) is the same shape as VSyncCallback in PsyQ --
  // game-thread-side it just registers a function pointer.  Aliasing to the
  // existing libgpu_VSyncCallback handler covers Crash's usage.
  // (Wired in psyq_libgpu.cpp::psyq_register_libgpu_extras.)
}

void psyq_register_rayman_boot() {
  ps1::psyq::psyq_register_libapi();
  ps1::psyq::psyq_register_libcd();
  ps1::psyq::psyq_register_libgte();
  ps1::psyq::psyq_register_libgpu_extras();
  ps1::psyq::psyq_register_libgpu_font();
  ps1::psyq::psyq_register_libetc_pad();
  ps1::psyq::psyq_register_libetc_intr();
  ps1::psyq::psyq_register_libc();
}
