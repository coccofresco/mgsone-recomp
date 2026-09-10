// Tests for ps1Runtime PsyQ HLE registry (Sessao 0.6)
//
// The registry maps canonical `<library>_<basename>` names to C++ HLE
// implementations. The recompiler emits `psyq_dispatch("name", ctx)` for
// every `[[hle_functions]]` entry; missing implementations must fail
// loudly at runtime with the function name and return address.

#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"

#include <atomic>
#include <gtest/gtest.h>

namespace {

// Process-global counter so fixtures can share a registered fn pointer
// (PsyqHleFn is a plain C function pointer, can't capture state directly).
std::atomic<int> g_test_dispatch_calls{0};

void test_hle_increment(recomp_context * /*ctx*/) {
  g_test_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
}

void test_hle_set_v0(recomp_context *ctx) {
  ctx->r[ps1::V0] = 0xDEADBEEFu;
}

class PsyqRegistryTest : public ::testing::Test {
protected:
  ps1::Memory mem;
  recomp_context ctx;

  void SetUp() override {
    mem.reset();
    ctx.reset();
    ctx.mem = &mem;
    g_test_dispatch_calls = 0;
  }
};

} // namespace

// Happy path: register + dispatch

TEST_F(PsyqRegistryTest, DispatchInvokesRegisteredFunction) {
  psyq_register("test_increment", &test_hle_increment);
  psyq_dispatch("test_increment", &ctx);
  EXPECT_EQ(g_test_dispatch_calls.load(), 1);

  psyq_dispatch("test_increment", &ctx);
  psyq_dispatch("test_increment", &ctx);
  EXPECT_EQ(g_test_dispatch_calls.load(), 3);
}

TEST_F(PsyqRegistryTest, DispatchPassesContextThrough) {
  psyq_register("test_set_v0", &test_hle_set_v0);
  ctx.r[ps1::V0] = 0;
  psyq_dispatch("test_set_v0", &ctx);
  EXPECT_EQ(ctx.r[ps1::V0], 0xDEADBEEFu);
}

TEST_F(PsyqRegistryTest, ReregisteringOverwritesBinding) {
  psyq_register("test_slot", &test_hle_increment);
  psyq_dispatch("test_slot", &ctx);
  EXPECT_EQ(g_test_dispatch_calls.load(), 1);

  psyq_register("test_slot", &test_hle_set_v0);
  ctx.r[ps1::V0] = 0;
  psyq_dispatch("test_slot", &ctx);
  EXPECT_EQ(ctx.r[ps1::V0], 0xDEADBEEFu);
  EXPECT_EQ(g_test_dispatch_calls.load(), 1); // increment not called again
}

// Unknown name aborts (FATAL)

TEST_F(PsyqRegistryTest, DispatchUnknownNameAborts) {
  ctx.r[ps1::RA] = 0x80012340u;
  // EXPECT_DEATH matches the FATAL diagnostic against a regex; the runtime
  // prints both the bad name and the return address.
  EXPECT_DEATH(
      { psyq_dispatch("definitely_not_registered_xyz", &ctx); },
      "FATAL.*definitely_not_registered_xyz.*0x80012340");
}

// init_defaults registers the 10 built-ins, idempotently

TEST_F(PsyqRegistryTest, InitDefaultsRegistersBuiltins) {
  psyq_registry_init_defaults();

  // Spot-check a few -- full list is in psyq_registry.cpp. Pick HLEs whose
  // bodies are NOPs or pointer-only (don't need GPU/BIOS configuration).
  // ResetGraph: NOP regardless of mode -- safe to dispatch with default ctx.
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_ResetGraph", &ctx));

  // ClearOTag with n==0: short-circuits to v0=base, no memory writes.
  ctx.r[ps1::A0] = 0x80100000u;
  ctx.r[ps1::A1] = 0u;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_ClearOTag", &ctx));
  EXPECT_EQ(ctx.r[ps1::V0], 0x80100000u);

  // SPU DMA transfers are synchronous in the runtime, so completion is
  // observable immediately even when the caller requests a non-blocking poll.
  ctx.r[ps1::A0] = 0u;
  ctx.r[ps1::V0] = 0u;
  EXPECT_NO_FATAL_FAILURE(
      psyq_dispatch("libspu_SpuIsTransferCompleted", &ctx));
  EXPECT_EQ(ctx.r[ps1::V0], 1u);
}

TEST_F(PsyqRegistryTest, InitDefaultsIsIdempotent) {
  // Register an override BEFORE init_defaults runs (or runs again). The
  // second init_defaults() call must NOT clobber user-registered slots.
  psyq_registry_init_defaults();

  psyq_register("libgpu_ResetGraph", &test_hle_increment);
  g_test_dispatch_calls = 0;
  psyq_dispatch("libgpu_ResetGraph", &ctx);
  EXPECT_EQ(g_test_dispatch_calls.load(), 1);

  // Calling init_defaults() again must be a no-op -- the override stays.
  psyq_registry_init_defaults();
  psyq_dispatch("libgpu_ResetGraph", &ctx);
  EXPECT_EQ(g_test_dispatch_calls.load(), 2);
}
