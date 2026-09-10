#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct recomp_context;

namespace ps1::bios {

struct Event {
  uint32_t classId;
  uint32_t specId;
  uint32_t mode;
  uint32_t handler;
  bool enabled;
  bool triggered;
  bool pendingTrigger; // trigger fired while disabled; applied on EnableEvent
};

struct PendingCallback {
  uint32_t handlerPc;
  uint32_t a0;     // value for ctx->r4 before call
  uint32_t a1;     // value for ctx->r5 before call
  bool hasArg;
  bool hasA1;
};

class EventSystem {
public:
  struct Snapshot {
    std::vector<Event> events;
    uint32_t triggeredBits = 0;
    std::vector<PendingCallback> pendingCallbacks;
  };

  EventSystem(recomp_context &ctx);
  ~EventSystem() = default;

  // BIOS Event Functions
  uint32_t openEvent(uint32_t classId, uint32_t specId, uint32_t mode,
                     uint32_t handler);
  uint32_t closeEvent(uint32_t eventId);
  uint32_t waitEvent(uint32_t eventId);
  uint32_t testEvent(uint32_t eventId);
  uint32_t enableEvent(uint32_t eventId);
  uint32_t disableEvent(uint32_t eventId);

  // Internal Emulator Functions (e.g. called by simulated hardware)
  void triggerEvent(uint32_t classId, uint32_t specId);
  void tick(); // Evaluate events

  // Process callbacks queued by triggerEvent / queueCallback.
  // MUST be called from the game thread (uses ctx_ safely).
  void drainPendingCallbacks();

  // Host-side BIOS state is part of the emulated timeline too. In
  // particular, pending callbacks must move backwards with a savestate; they
  // cannot be left over from the timeline being replaced.
  Snapshot snapshot() const;
  bool restore(const Snapshot &state);
  void discardPendingTimeline();

  // Queue a callback with no specific register setup.
  // Thread-safe: may be called from the main loop thread.
  void queueCallback(uint32_t handlerPc) {
    std::lock_guard<std::mutex> lk(cbMtx_);
    pendingCallbacks_.push_back({handlerPc, 0, 0, false, false});
  }

  // Queue a callback with a0 ($r4) set to a specific value.
  // Thread-safe: may be called from the main loop thread.
  void queueCallbackWithArg(uint32_t handlerPc, uint32_t a0Val) {
    std::lock_guard<std::mutex> lk(cbMtx_);
    pendingCallbacks_.push_back({handlerPc, a0Val, 0, true, false});
  }

  // Queue a callback with both a0 ($r4) and a1 ($r5) set.
  // Thread-safe: may be called from the main loop thread.
  void queueCallbackWithArgs(uint32_t handlerPc, uint32_t a0Val, uint32_t a1Val) {
    std::lock_guard<std::mutex> lk(cbMtx_);
    pendingCallbacks_.push_back({handlerPc, a0Val, a1Val, true, true});
  }

private:
  recomp_context &ctx_;
  std::vector<Event> events_;

  // Mutex protecting events_ vector. ALL reads and writes to events_
  // (openEvent, testEvent, enableEvent, triggerEvent, etc.) must hold this.
  // Main thread writes triggered state, game thread reads it.
  mutable std::mutex eventsMtx_;

  // Atomic bitmask for thread-safe event triggering.
  // Main thread sets bits via triggerEvent(), game thread reads/clears via testEvent().
  // Each bit corresponds to an event ID (0-31).
  std::atomic<uint32_t> triggeredBits_{0};

  mutable std::mutex cbMtx_;  // protects pendingCallbacks_
  std::vector<PendingCallback> pendingCallbacks_;

  static constexpr uint32_t MAX_EVENTS = 32;
  static constexpr uint32_t INVALID_EVENT_ID = 0xFFFFFFFF;
};

} // namespace ps1::bios
