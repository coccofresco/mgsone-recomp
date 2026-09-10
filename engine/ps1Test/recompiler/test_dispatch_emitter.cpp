// Tests for the generated `recomp_dispatch` body.
//
// The behaviour under test is a policy decision, not a formatting detail: an
// address that reaches the end of the resolver is code the recompiler never
// emitted. The original generator logged it five times and kept running, which
// made every miss a silent no-op -- execution continued with corrupted state
// and the symptom surfaced far from its cause. Measured consequence on Crash
// Bandicoot: months spent chasing render symptoms whose real cause was a
// function split in two by the analyzer, whose intra-function branch became a
// dispatch to an address that was never emitted, and was then swallowed.

#include "ps1recomp/dispatch_emitter.h"
#include <gtest/gtest.h>
#include <string>

using ps1recomp::emitDispatchBody;

namespace {

// Position of `needle` in the emitted body, or std::string::npos.
size_t at(const std::string &body, const char *needle) {
  return body.find(needle);
}

// Position of `needle` at or after a structural marker.  The generated body
// can contain independent fatal guards (for example the interpreter-tail hop
// limit), so tests for the final unmapped-target path must not accidentally
// bind to an earlier abort.
size_t atAfter(const std::string &body, const char *marker,
               const char *needle) {
  const size_t markerAt = at(body, marker);
  return markerAt == std::string::npos ? markerAt : body.find(needle, markerAt);
}

TEST(DispatchEmitter, AbortsOnUnmappedTarget) {
  const std::string body = emitDispatchBody();
  EXPECT_NE(at(body, "std::abort();"), std::string::npos)
      << "an unmapped target must stop the run, not be logged and skipped";
}

TEST(DispatchEmitter, FatalMessageNamesAddressCallerAndCause) {
  const std::string body = emitDispatchBody();
  // The whole point of failing here is that the message is actionable: the
  // address to emit, and the caller that reached it.
  EXPECT_NE(at(body, "unmapped call to 0x{:08X}"), std::string::npos);
  EXPECT_NE(at(body, "RA=0x{:08X}"), std::string::npos);
  EXPECT_NE(at(body, "never emitted by the recompiler"), std::string::npos);
}

TEST(DispatchEmitter, AbortIsReachedBeforeTheRateLimitedLogging) {
  const std::string body = emitDispatchBody();
  const size_t abortAt =
      atAfter(body, "8. Unmapped target", "std::abort();");
  const size_t logAt = at(body, "s_unknownHits");
  ASSERT_NE(abortAt, std::string::npos);
  ASSERT_NE(logAt, std::string::npos);
  EXPECT_LT(abortAt, logAt)
      << "the permissive log must sit after the abort, so it only runs when "
         "the escape hatch was taken";
}

TEST(DispatchEmitter, PermissiveEscapeHatchIsEnvGated) {
  const std::string body = emitDispatchBody();
  EXPECT_NE(at(body, "PS1_DISPATCH_PERMISSIVE"), std::string::npos)
      << "there must be a documented way to run a knowingly-incomplete build";
  // Guard against the hatch being open by default: the abort is skipped only
  // when the variable is set to something other than empty or "0".
  EXPECT_NE(at(body, "std::getenv(\"PS1_DISPATCH_PERMISSIVE\")"),
            std::string::npos);
  EXPECT_NE(at(body, "if (!s_permissive)"), std::string::npos);
}

// The resolver has seven legitimate paths before the failure. Losing any of them
// would turn a resolvable call into a spurious abort, which is worse than the
// silent no-op it replaced -- so pin the order.
TEST(DispatchEmitter, ResolutionPathsPrecedeTheFailure) {
  const std::string body = emitDispatchBody();
  const size_t nullGuard = at(body, "1. NULL pointer guard");
  const size_t direct = at(body, "2. Direct lookup");
  const size_t normalize = at(body, "3. Address normalization");
  const size_t biosEntry = at(body, "4. BIOS entry points");
  const size_t sentinel = at(body, "5. BIOS table sentinel");
  const size_t trampoline = at(body, "6. JR RA trampoline");
  const size_t dynamicMips = at(body, "7. Heap-loaded native code");
  const size_t fatal = at(body, "8. Unmapped target");

  for (size_t pos : {nullGuard, direct, normalize, biosEntry, sentinel,
                     trampoline, dynamicMips, fatal})
    ASSERT_NE(pos, std::string::npos) << "a resolution path went missing";

  EXPECT_LT(nullGuard, direct);
  EXPECT_LT(direct, normalize);
  EXPECT_LT(normalize, biosEntry);
  EXPECT_LT(biosEntry, sentinel);
  EXPECT_LT(sentinel, trampoline);
  EXPECT_LT(trampoline, dynamicMips);
  EXPECT_LT(dynamicMips, fatal);
}

TEST(DispatchEmitter, NullDispatchStillReturnsQuietly) {
  // Null targets are a known benign startup transient (uninitialised callback
  // pointers), so they must keep returning rather than abort.
  const std::string body = emitDispatchBody();
  const size_t guard = at(body, "if (addr == 0)");
  const size_t fatal =
      atAfter(body, "8. Unmapped target", "std::abort();");
  ASSERT_NE(guard, std::string::npos);
  ASSERT_NE(fatal, std::string::npos);
  EXPECT_LT(guard, fatal);
}

TEST(DispatchEmitter, ReturnTrampolineFastPathRequiresNopDelaySlot) {
  const std::string body = emitDispatchBody();
  const size_t trampoline = at(body, "instr == 0x03E00008u");
  const size_t delayGuard = at(body, "delaySlot == 0x00000000u");
  const size_t dynamicMips = at(body, "7. Heap-loaded native code");
  ASSERT_NE(trampoline, std::string::npos);
  ASSERT_NE(delayGuard, std::string::npos)
      << "JR RA with a non-NOP delay slot must be interpreted";
  ASSERT_NE(dynamicMips, std::string::npos);
  EXPECT_LT(trampoline, delayGuard);
  EXPECT_LT(delayGuard, dynamicMips)
      << "a non-empty return delay slot must fall through to dynamic MIPS";
}

TEST(DispatchEmitter, NativeEntryAliasesPrecedePrivateInterpreterFallback) {
  const std::string body = emitDispatchBody();
  const size_t aliasLookup = at(body, "recomp_lookup_entry_alias(normalized)");
  const size_t selectPc = at(body, "ctx->nativeEntryPc = normalized");
  const size_t privateFallback = at(body, "recomp_try_static_interpreter");
  ASSERT_NE(aliasLookup, std::string::npos);
  ASSERT_NE(selectPc, std::string::npos);
  ASSERT_NE(privateFallback, std::string::npos);
  EXPECT_LT(aliasLookup, privateFallback);
  EXPECT_LT(selectPc, privateFallback);
}

TEST(DispatchEmitter, InterpreterTailTransfersUseBoundedDispatcherLoop) {
  const std::string body = emitDispatchBody();
  const size_t loop = at(body, "for (;;) {");
  const size_t consume = at(body, "takeDeferredTailDispatch(target)");
  const size_t continueTail = at(body, "if (continueDeferredTail()) continue;");
  const size_t hopLimit = at(body, "deferredTailHops > 1'000'000u");
  ASSERT_NE(loop, std::string::npos);
  ASSERT_NE(consume, std::string::npos);
  ASSERT_NE(continueTail, std::string::npos);
  ASSERT_NE(hopLimit, std::string::npos);
  EXPECT_LT(consume, loop);
  EXPECT_LT(loop, continueTail);
}

} // namespace
