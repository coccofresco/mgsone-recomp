// Stubs for symbols defined in recompiled_out.cpp that the runtime library
// references but are not available during unit testing.

#include <cstdint>
#include <runtime/cpu_context.h>

// Opt-in hook letting a test stand in for the dispatched target, so HLEs that
// call through recomp_dispatch (e.g. libgpu__addque2 invoking its device
// routine) can be asserted on what the callee actually observes and returns.
// Null by default: recomp_dispatch stays a no-op for every other test.
void (*g_testRecompDispatchHook)(recomp_context *ctx, uint32_t addr) = nullptr;

// Stub recomp_dispatch -- called by EventSystem and Bios drainPendingCallbacks.
void recomp_dispatch(uint8_t * /*rdram*/, recomp_context *ctx, uint32_t addr) {
  if (g_testRecompDispatchHook)
    g_testRecompDispatchHook(ctx, addr);
}
