#pragma once

#include "runtime/gpu/modern_replay_preflight.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace ps1::gpu::replay {

// CPU-only policy used to decide whether one exact native display snapshot can
// be produced by M1 replay or must remain on M0.  It deliberately describes
// replay support, not Vulkan device features.
struct ModernReplaySubmissionConfig {
  uint32_t schemaVersion = CRASH_REPLAY_SCHEMA_VERSION;
  std::string schemaName{CRASH_REPLAY_SCHEMA_NAME};
  ReplayPolicyState expectedPolicy;
  ReplayCapabilityMask capabilities = 0;
  ReplayNearClipMode nearClipMode = ReplayNearClipMode::Saturate;
  bool requireCanonicalRawState = true;
  bool requireFullPrecisionCoverage = false;
  bool allowIncompleteNativeReanchor = false;

  bool operator==(const ModernReplaySubmissionConfig &) const = default;
};

enum class ModernReplaySubmissionRoute : uint8_t {
  M1Replay,
  M0Reanchor,
  M0PresentOnly,
};

class ModernReplaySubmissionScheduler;

// An immutable decision over the current FIFO prefix. Planning never consumes
// frames or advances replay state. A renderer may therefore abandon this
// object after acquire, command-recording, or vkQueueSubmit failure.
class ModernReplaySubmissionPlan final {
public:
  ModernReplaySubmissionPlan() = default;
  ModernReplaySubmissionPlan(const ModernReplaySubmissionPlan &) = delete;
  ModernReplaySubmissionPlan &
  operator=(const ModernReplaySubmissionPlan &) = delete;
  ModernReplaySubmissionPlan(ModernReplaySubmissionPlan &&other) noexcept;
  ModernReplaySubmissionPlan &
  operator=(ModernReplaySubmissionPlan &&other) noexcept;

  ModernReplaySubmissionRoute route() const noexcept { return route_; }
  ReplayPreflightReason reason() const noexcept { return reason_; }
  ReplayPreflightReason strictReason() const noexcept { return strictReason_; }
  ReplayCapabilityMask missingCapabilities() const noexcept {
    return missingCapabilities_;
  }
  const ReplayPreflightCounters &counters() const noexcept { return counters_; }
  std::span<const CrashReplayFramePtr> frames() const noexcept {
    return frames_;
  }
  const ReplayPreflightState &candidateState() const noexcept {
    return candidateState_;
  }
  bool advancesReplayState() const noexcept {
    return route_ != ModernReplaySubmissionRoute::M0PresentOnly;
  }

private:
  friend class ModernReplaySubmissionScheduler;

  ModernReplaySubmissionRoute route_ =
      ModernReplaySubmissionRoute::M0PresentOnly;
  ReplayPreflightReason reason_ = ReplayPreflightReason::None;
  ReplayPreflightReason strictReason_ = ReplayPreflightReason::None;
  ReplayCapabilityMask missingCapabilities_ = 0;
  ReplayPreflightCounters counters_;
  std::vector<CrashReplayFramePtr> frames_;
  ReplayPreflightState candidateState_;

  const ModernReplaySubmissionScheduler *owner_ = nullptr;
  uint64_t headGeneration_ = 0;
};

// Ownership record for one successfully queued submission. It keeps the
// consumed immutable frames and their resulting state alive until the owning
// FrameContext observes its queue-ordered fence.
class ModernReplaySubmissionCommit final {
public:
  ModernReplaySubmissionCommit() = default;
  ModernReplaySubmissionCommit(const ModernReplaySubmissionCommit &) = delete;
  ModernReplaySubmissionCommit &
  operator=(const ModernReplaySubmissionCommit &) = delete;
  ModernReplaySubmissionCommit(ModernReplaySubmissionCommit &&other) noexcept;
  // A FrameContext must wait for and confirm its fence, then destroy/reset its
  // optional commit before constructing a replacement. Assignment is deleted
  // so an in-flight commit cannot silently release its strong frame owners.
  ModernReplaySubmissionCommit &
  operator=(ModernReplaySubmissionCommit &&other) noexcept = delete;

  bool valid() const noexcept { return owner_ != nullptr; }
  uint64_t queueSerial() const noexcept { return queueSerial_; }
  ModernReplaySubmissionRoute route() const noexcept { return route_; }
  std::span<const CrashReplayFramePtr> frames() const noexcept {
    return frames_;
  }
  const ReplayPreflightState &candidateState() const noexcept {
    return candidateState_;
  }

private:
  friend class ModernReplaySubmissionScheduler;

  const ModernReplaySubmissionScheduler *owner_ = nullptr;
  uint64_t queueSerial_ = 0;
  ModernReplaySubmissionRoute route_ =
      ModernReplaySubmissionRoute::M0PresentOnly;
  std::vector<CrashReplayFramePtr> frames_;
  ReplayPreflightState candidateState_;
};

// Single-consumer scheduler between CrashReplayMailbox and Vulkan recording.
// The mailbox remains the producer synchronization boundary; this class owns
// a renderer-thread deque and never drops or coalesces mutations.
class ModernReplaySubmissionScheduler final {
public:
  explicit ModernReplaySubmissionScheduler(
      ModernReplaySubmissionConfig config,
      ReplayPreflightState initialState = {});

  ModernReplaySubmissionScheduler(const ModernReplaySubmissionScheduler &) =
      delete;
  ModernReplaySubmissionScheduler &
  operator=(const ModernReplaySubmissionScheduler &) = delete;
  ModernReplaySubmissionScheduler(ModernReplaySubmissionScheduler &&) = delete;
  ModernReplaySubmissionScheduler &
  operator=(ModernReplaySubmissionScheduler &&) = delete;

  void enqueue(CrashReplayFramePtr frame);
  void enqueue(std::span<const CrashReplayFramePtr> frames);

  std::size_t pendingFrameCount() const noexcept { return pending_.size(); }
  std::vector<CrashReplayFramePtr> pendingFrames() const;
  const ModernReplaySubmissionConfig &config() const noexcept {
    return config_;
  }
  const ReplayPreflightState &scheduledState() const noexcept {
    return scheduledState_;
  }
  const ReplayPreflightState &confirmedState() const noexcept {
    return confirmedState_;
  }
  uint64_t lastSubmittedQueueSerial() const noexcept {
    return lastSubmittedQueueSerial_;
  }
  uint64_t confirmedQueueSerial() const noexcept {
    return confirmedQueueSerial_;
  }

  // A save-state keeps the renderer-owned surface/depth baseline but replaces
  // the guest's scalar frame timeline. Call only after all submitted consumer
  // work has completed. Pending frames belong to the abandoned timeline and
  // are discarded; the next strong snapshot re-establishes frame ordering.
  void rebaseGuestTimeline() noexcept;

  // Selects only the prefix ending at snapshot.sealedFrame. A missing strong
  // token is never replaced by a scalar latch match.
  [[nodiscard]] ModernReplaySubmissionPlan
  plan(const ReplayNativeSnapshotIdentity &snapshot) const;

  // Atomically lowers an already accepted M1 prefix when Vulkan preparation
  // discovers an executor/resource miss. The exact frames, FIFO generation
  // and strong snapshot target must still match; planning remains read-only
  // and commitSubmitted is still the only consuming operation.
  [[nodiscard]] ModernReplaySubmissionPlan downgradeToNativeReanchor(
      ModernReplaySubmissionPlan &&acceptedM1,
      const ReplayNativeSnapshotIdentity &snapshot) const;

  // Check this immediately before vkQueueSubmit. With the documented
  // single-consumer discipline, only tail enqueue is allowed between this
  // check and commitSubmitted, and tail enqueue cannot invalidate the plan.
  [[nodiscard]] bool
  validatePlanForSubmit(const ModernReplaySubmissionPlan &plan) const noexcept;

  // Call exactly once, immediately after queue submission succeeds. This is
  // noexcept and is the sole operation which removes a FIFO prefix or advances
  // scheduled state. Presentation happens later and cannot roll this back.
  [[nodiscard]] ModernReplaySubmissionCommit
  commitSubmitted(ModernReplaySubmissionPlan &&plan) noexcept;

  // Vulkan queue ordering means a later signaled fence also confirms every
  // earlier serial. Polling an older FrameContext afterward never regresses
  // the confirmed replay state.
  void
  confirmQueueOrderedFence(const ModernReplaySubmissionCommit &commit) noexcept;

private:
  bool planStillMatchesPending(
      const ModernReplaySubmissionPlan &plan) const noexcept;

  ModernReplaySubmissionConfig config_;
  std::deque<CrashReplayFramePtr> pending_;
  ReplayPreflightState scheduledState_;
  ReplayPreflightState confirmedState_;
  uint64_t headGeneration_ = 0;
  uint64_t lastSubmittedQueueSerial_ = 0;
  uint64_t confirmedQueueSerial_ = 0;
};

} // namespace ps1::gpu::replay
