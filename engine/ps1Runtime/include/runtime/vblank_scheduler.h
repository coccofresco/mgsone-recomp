#pragma once

#include <algorithm>
#include <cstdint>

namespace ps1::timing {

// A bounded host-pacing slice. The wall clock is only permission to advance
// guest time; it is not guest time itself. If rendering or diagnostics fall
// behind, excess host pulses are coalesced instead of being replayed as a
// burst of guest VBlanks. Replaying them would let VBlank callbacks advance
// several times while the game thread is still executing one frame.
struct VblankSlice {
  uint64_t debt = 0;
  uint64_t count = 0;
  uint64_t end = 0;
  uint64_t coalesced = 0;
};

// The host cannot deliver a VBlank callback on the clock thread. Preserve the
// newest single clock credit until the game and main thread reach their next
// safe boundary. Discarding that first credit makes a frame which misses a
// 20 ms PAL boundary by a few microseconds wait another complete period;
// retaining more than one would instead expose host stalls as guest
// fast-forward. The returned value is suitable for rebasing the atomic host
// pulse counter, and `coalesced` accounts only for genuinely excess credits.
struct PacingCreditRebase {
  uint64_t retainedDue = 0;
  uint64_t coalesced = 0;
};

constexpr PacingCreditRebase planPacingCreditRebase(uint64_t processed,
                                                    uint64_t due) {
  if (due <= processed)
    return {processed, 0};
  const uint64_t retainedDue =
      processed == UINT64_MAX ? UINT64_MAX : processed + 1u;
  return {retainedDue, due - retainedDue};
}

constexpr VblankSlice planVblankSlice(uint64_t processed, uint64_t due,
                                      uint64_t requestedLimit) {
  if (due <= processed)
    return {0, 0, processed, 0};
  const uint64_t limit = std::max<uint64_t>(1, requestedLimit);
  const uint64_t debt = due - processed;
  const uint64_t count = std::min(debt, limit);
  return {debt, count, processed + count, debt - count};
}

// Before the first blocking VSync, retain free-running hardware time for boot
// code and titles which never use the PsyQ frame wait. Afterwards a VBlank is
// admitted only for an outstanding VSync target or an explicit asynchronous
// hardware wait. This is a scheduling relation, not a game/PC heuristic.
constexpr bool guestVblankRequested(bool blockingPacingActive,
                                    uint32_t blockingTarget,
                                    uint32_t currentVblank,
                                    uint32_t autonomousWaiters) {
  return !blockingPacingActive || autonomousWaiters != 0 ||
         (blockingTarget != 0 && blockingTarget > currentVblank);
}

} // namespace ps1::timing
