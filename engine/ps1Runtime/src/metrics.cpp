#include "runtime/metrics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace ps1::metrics {
namespace detail {

// Normal gameplay asks about the surgical GTE trace on every command. Keep a
// process-lifetime rejection gate visible to gte.cpp so an untraced run does
// not cross this translation-unit boundary millions of times. Tests restore
// the unresolved state before changing trace environment variables.
std::atomic<int> g_gteTraceFastGate{-1};

} // namespace detail
namespace {

std::mutex g_mutex;
// `std::less<>` makes these heterogeneously comparable, so lookups take a
// string_view without allocating.
std::map<std::string, uint64_t, std::less<>> g_counters;
std::map<std::string, int64_t, std::less<>> g_states;

// -1 = unresolved, 0 = off, 1 = on.  Racing threads all compute the same
// value from the same environment, so the race is benign.
std::atomic<int> g_enabled{-1};

std::mutex g_traceMutex;
std::ofstream g_traceOut;
std::atomic<int> g_traceEnabled{-1};
std::atomic<int> g_traceArmAfterLoadMode{-1};
std::atomic<int> g_guestTraceLoaded{0};
// -1 = unresolved, 0 = allow every event, 1 = exact-name allowlist. The
// string becomes immutable before mode is published with release semantics.
// resetTraceForTesting mutates it only while no producer is running.
std::atomic<int> g_traceEventFilterMode{-1};
std::string g_traceEventFilter;
std::atomic<int> g_gteTraceEnabled{-1};
std::atomic<int> g_gteTraceBoundsResolved{0};
std::atomic<int> g_gteTraceTriggerMode{-1};
std::atomic<uint64_t> g_gteTraceTriggeredFrame{
    std::numeric_limits<uint64_t>::max()};
std::atomic<uint64_t> g_gteTraceTriggerFrames{1};
std::atomic<int> g_gteTraceTriggerFrameFromResolved{0};
std::atomic<uint64_t> g_gteTraceTriggerFrameFrom{0};
std::atomic<uint64_t> g_gteTraceTriggerHitTarget{0};
std::atomic<uint64_t> g_gteTraceTriggerHits{0};
std::atomic<uint64_t> g_gteTraceFrameFrom{0};
std::atomic<uint64_t> g_gteTraceFrameTo{std::numeric_limits<uint64_t>::max()};
std::atomic<int> g_gpuWordTraceEnabled{-1};
std::atomic<int> g_gpuWordTraceBoundsResolved{0};
std::atomic<int> g_gpuWordTraceTriggerMode{-1};
std::atomic<uint64_t> g_gpuWordTraceTriggeredFrame{
    std::numeric_limits<uint64_t>::max()};
std::atomic<uint64_t> g_gpuWordTraceTriggerFrames{2};
std::atomic<int> g_gpuWordTraceTriggerFrameFromResolved{0};
std::atomic<uint64_t> g_gpuWordTraceTriggerFrameFrom{0};
std::atomic<uint64_t> g_gpuWordTraceTriggerHitTarget{0};
std::atomic<uint64_t> g_gpuWordTraceTriggerHits{0};
std::atomic<uint64_t> g_gpuWordTraceFrameFrom{0};
std::atomic<uint64_t> g_gpuWordTraceFrameTo{
    std::numeric_limits<uint64_t>::max()};
std::atomic<uint64_t> g_projectionTraceFirstId{
    std::numeric_limits<uint64_t>::max()};
std::atomic<uint64_t> g_projectionTraceLastId{0};
std::atomic<uint64_t> g_projectionTraceMatchedSubmission{0};
std::atomic<int> g_projectionTraceComplete{0};
std::atomic<int> g_pgxpVertexTraceEnabled{-1};
std::atomic<int> g_pgxpVertexTraceBoundsResolved{0};
std::atomic<uint64_t> g_pgxpVertexTraceFrameFrom{0};
std::atomic<uint64_t> g_pgxpVertexTraceFrameTo{
    std::numeric_limits<uint64_t>::max()};
std::atomic<int> g_modernRejectTraceEnabled{-1};
std::atomic<int> g_modernRejectTraceBoundsResolved{0};
std::atomic<uint64_t> g_modernRejectTraceFrameFrom{0};
std::atomic<uint64_t> g_modernRejectTraceFrameTo{
    std::numeric_limits<uint64_t>::max()};
std::atomic<uint64_t> g_traceFrame{0};
uint64_t g_traceSequence = 0;
std::chrono::steady_clock::time_point g_traceEpoch;

void writeJsonString(std::ostream &out, std::string_view value) {
  out << '"';
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        static constexpr char hex[] = "0123456789abcdef";
        out << "\\u00" << hex[c >> 4] << hex[c & 0xf];
      } else {
        out << static_cast<char>(c);
      }
    }
  }
  out << '"';
}

uint64_t parseFrameBound(const char *name, uint64_t fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    return fallback;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  return end != value && *end == '\0' ? static_cast<uint64_t>(parsed)
                                      : fallback;
}

uint64_t resolvePositiveTriggerHit(const char *name,
                                   std::atomic<uint64_t> &cached) {
  uint64_t target = cached.load(std::memory_order_acquire);
  if (target != 0)
    return target;
  target = std::max<uint64_t>(1, parseFrameBound(name, 1));
  cached.store(target, std::memory_order_release);
  return target;
}

bool traceEventEnabled(std::string_view event) {
  int mode = g_traceEventFilterMode.load(std::memory_order_acquire);
  if (mode < 0) {
    std::lock_guard<std::mutex> lock(g_traceMutex);
    mode = g_traceEventFilterMode.load(std::memory_order_relaxed);
    if (mode < 0) {
      const char *value = std::getenv("PS1_TRACE_EVENTS");
      if (value == nullptr || *value == '\0') {
        g_traceEventFilter.clear();
        mode = 0;
      } else {
        g_traceEventFilter = value;
        mode = 1;
      }
      g_traceEventFilterMode.store(mode, std::memory_order_release);
    }
  }
  if (mode == 0)
    return true;

  // Exact tokens only: a broad prefix such as "gpu" must not accidentally
  // turn a surgical provenance capture into millions of raster-route events.
  std::string_view filter(g_traceEventFilter);
  while (!filter.empty()) {
    const std::size_t delimiter = filter.find_first_of(",;");
    std::string_view token = filter.substr(0, delimiter);
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
      token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
      token.remove_suffix(1);
    if (token == event)
      return true;
    if (delimiter == std::string_view::npos)
      break;
    filter.remove_prefix(delimiter + 1);
  }
  return false;
}

} // namespace

bool enabled() {
  int e = g_enabled.load(std::memory_order_relaxed);
  if (e < 0) {
    e = std::getenv("PS1_METRICS") != nullptr ? 1 : 0;
    g_enabled.store(e, std::memory_order_relaxed);
  }
  return e == 1;
}

void setEnabledForTesting(bool on) {
  g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool traceEnabled() {
  int enabled = g_traceEnabled.load(std::memory_order_acquire);
  if (enabled >= 0)
    return enabled == 1;

  const char *path = std::getenv("PS1_TRACE_JSONL");
  enabled = path != nullptr && *path != '\0' ? 1 : 0;
  g_traceEnabled.store(enabled, std::memory_order_release);
  return enabled == 1;
}

bool gteTraceEnabled() {
  if (!traceEnabled()) {
    detail::g_gteTraceFastGate.store(0, std::memory_order_relaxed);
    return false;
  }
  detail::g_gteTraceFastGate.store(1, std::memory_order_relaxed);
  if (!guestTraceArmed())
    return false;
  int enabled = g_gteTraceEnabled.load(std::memory_order_acquire);
  if (enabled < 0) {
    const char *value = std::getenv("PS1_TRACE_GTE");
    enabled = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_gteTraceEnabled.store(enabled, std::memory_order_release);
  }
  detail::g_gteTraceFastGate.store(enabled == 1 ? 1 : 0,
                                   std::memory_order_relaxed);
  if (enabled != 1)
    return false;

  int triggerMode = g_gteTraceTriggerMode.load(std::memory_order_acquire);
  if (triggerMode < 0) {
    const char *value = std::getenv("PS1_TRACE_GTE_TRIGGER_PC");
    triggerMode = value != nullptr && *value != '\0' ? 1 : 0;
    if (triggerMode == 1) {
      g_gteTraceTriggerFrames.store(
          std::max<uint64_t>(
              1, parseFrameBound("PS1_TRACE_GTE_TRIGGER_FRAMES", 1)),
          std::memory_order_relaxed);
    }
    g_gteTraceTriggerMode.store(triggerMode, std::memory_order_release);
  }
  if (triggerMode == 1) {
    if (g_projectionTraceComplete.load(std::memory_order_acquire) != 0)
      return false;
    const uint64_t triggered =
        g_gteTraceTriggeredFrame.load(std::memory_order_acquire);
    const uint64_t frame = traceFrame();
    return triggered != std::numeric_limits<uint64_t>::max() &&
           frame >= triggered &&
           frame - triggered <
               g_gteTraceTriggerFrames.load(std::memory_order_relaxed);
  }

  if (g_gteTraceBoundsResolved.load(std::memory_order_acquire) == 0) {
    g_gteTraceFrameFrom.store(parseFrameBound("PS1_TRACE_GTE_FRAME_FROM", 0),
                              std::memory_order_relaxed);
    g_gteTraceFrameTo.store(
        parseFrameBound("PS1_TRACE_GTE_FRAME_TO",
                        std::numeric_limits<uint64_t>::max()),
        std::memory_order_relaxed);
    g_gteTraceBoundsResolved.store(1, std::memory_order_release);
  }

  const uint64_t frame = traceFrame();
  return frame >= g_gteTraceFrameFrom.load(std::memory_order_relaxed) &&
         frame <= g_gteTraceFrameTo.load(std::memory_order_relaxed);
}

void triggerGteTraceForCurrentFrame() {
  if (!guestTraceArmed())
    return;
  if (g_gteTraceTriggerFrameFromResolved.load(std::memory_order_acquire) ==
      0) {
    g_gteTraceTriggerFrameFrom.store(
        parseFrameBound("PS1_TRACE_GTE_TRIGGER_FRAME_FROM", 0),
        std::memory_order_relaxed);
    g_gteTraceTriggerFrameFromResolved.store(1, std::memory_order_release);
  }
  if (traceFrame() <
      g_gteTraceTriggerFrameFrom.load(std::memory_order_relaxed))
    return;
  const uint64_t hit =
      g_gteTraceTriggerHits.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (hit != resolvePositiveTriggerHit("PS1_TRACE_GTE_TRIGGER_HIT",
                                       g_gteTraceTriggerHitTarget))
    return;
  uint64_t unarmed = std::numeric_limits<uint64_t>::max();
  if (g_gteTraceTriggeredFrame.compare_exchange_strong(
          unarmed, traceFrame(), std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    setState("trace.gte_trigger.hit", static_cast<int64_t>(hit));
    setState("trace.gte_trigger.frame", static_cast<int64_t>(traceFrame()));
  }
}

bool gpuWordTraceEnabled() {
  if (!traceEnabled() || !guestTraceArmed())
    return false;
  int enabled = g_gpuWordTraceEnabled.load(std::memory_order_acquire);
  if (enabled < 0) {
    const char *value = std::getenv("PS1_TRACE_GPU_WORDS");
    enabled = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_gpuWordTraceEnabled.store(enabled, std::memory_order_release);
  }
  if (enabled != 1)
    return false;

  int triggerMode = g_gpuWordTraceTriggerMode.load(std::memory_order_acquire);
  if (triggerMode < 0) {
    const char *value = std::getenv("PS1_TRACE_GPU_TRIGGER_PC");
    triggerMode = value != nullptr && *value != '\0' ? 1 : 0;
    if (triggerMode == 1) {
      g_gpuWordTraceTriggerFrames.store(
          std::max<uint64_t>(
              1, parseFrameBound("PS1_TRACE_GPU_TRIGGER_FRAMES", 2)),
          std::memory_order_relaxed);
    }
    g_gpuWordTraceTriggerMode.store(triggerMode, std::memory_order_release);
  }
  if (triggerMode == 1) {
    if (g_projectionTraceComplete.load(std::memory_order_acquire) != 0)
      return false;
    const uint64_t triggered =
        g_gpuWordTraceTriggeredFrame.load(std::memory_order_acquire);
    const uint64_t frame = traceFrame();
    return triggered != std::numeric_limits<uint64_t>::max() &&
           frame >= triggered &&
           frame - triggered <
               g_gpuWordTraceTriggerFrames.load(std::memory_order_relaxed);
  }

  if (g_gpuWordTraceBoundsResolved.load(std::memory_order_acquire) == 0) {
    g_gpuWordTraceFrameFrom.store(
        parseFrameBound("PS1_TRACE_GPU_FRAME_FROM", 0),
        std::memory_order_relaxed);
    g_gpuWordTraceFrameTo.store(
        parseFrameBound("PS1_TRACE_GPU_FRAME_TO",
                        std::numeric_limits<uint64_t>::max()),
        std::memory_order_relaxed);
    g_gpuWordTraceBoundsResolved.store(1, std::memory_order_release);
  }

  const uint64_t frame = traceFrame();
  return frame >= g_gpuWordTraceFrameFrom.load(std::memory_order_relaxed) &&
         frame <= g_gpuWordTraceFrameTo.load(std::memory_order_relaxed);
}

void triggerGpuWordTraceForCurrentFrame() {
  if (!guestTraceArmed())
    return;
  if (g_gpuWordTraceTriggerFrameFromResolved.load(
          std::memory_order_acquire) == 0) {
    g_gpuWordTraceTriggerFrameFrom.store(
        parseFrameBound("PS1_TRACE_GPU_TRIGGER_FRAME_FROM", 0),
        std::memory_order_relaxed);
    g_gpuWordTraceTriggerFrameFromResolved.store(1,
                                                  std::memory_order_release);
  }
  if (traceFrame() <
      g_gpuWordTraceTriggerFrameFrom.load(std::memory_order_relaxed))
    return;
  const uint64_t hit =
      g_gpuWordTraceTriggerHits.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (hit != resolvePositiveTriggerHit("PS1_TRACE_GPU_TRIGGER_HIT",
                                       g_gpuWordTraceTriggerHitTarget))
    return;
  uint64_t unarmed = std::numeric_limits<uint64_t>::max();
  if (g_gpuWordTraceTriggeredFrame.compare_exchange_strong(
          unarmed, traceFrame(), std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    setState("trace.gpu_trigger.hit", static_cast<int64_t>(hit));
    setState("trace.gpu_trigger.frame", static_cast<int64_t>(traceFrame()));
  }
}

void noteGteProjectionForTrace(uint64_t projectionId) {
  if (projectionId == 0)
    return;
  uint64_t first = g_projectionTraceFirstId.load(std::memory_order_relaxed);
  while (projectionId < first &&
         !g_projectionTraceFirstId.compare_exchange_weak(
             first, projectionId, std::memory_order_release,
             std::memory_order_relaxed)) {
  }
  uint64_t last = g_projectionTraceLastId.load(std::memory_order_relaxed);
  while (projectionId > last &&
         !g_projectionTraceLastId.compare_exchange_weak(
             last, projectionId, std::memory_order_release,
             std::memory_order_relaxed)) {
  }
}

void noteGpuProjectionForTrace(uint64_t projectionId, uint64_t submissionId) {
  if (projectionId == 0 || submissionId == 0 ||
      g_projectionTraceComplete.load(std::memory_order_acquire) != 0)
    return;
  const uint64_t first =
      g_projectionTraceFirstId.load(std::memory_order_acquire);
  const uint64_t last =
      g_projectionTraceLastId.load(std::memory_order_acquire);
  if (first == std::numeric_limits<uint64_t>::max() || projectionId < first ||
      projectionId > last)
    return;
  uint64_t unmatched = 0;
  g_projectionTraceMatchedSubmission.compare_exchange_strong(
      unmatched, submissionId, std::memory_order_acq_rel,
      std::memory_order_relaxed);
}

void finishGpuTraceSubmission(uint64_t submissionId) {
  if (submissionId != 0 &&
      g_projectionTraceMatchedSubmission.load(std::memory_order_acquire) ==
          submissionId)
    g_projectionTraceComplete.store(1, std::memory_order_release);
}

bool projectionTraceComplete() {
  return g_projectionTraceComplete.load(std::memory_order_acquire) != 0;
}

bool pgxpVertexTraceEnabled() {
  if (!traceEnabled())
    return false;
  int enabled = g_pgxpVertexTraceEnabled.load(std::memory_order_acquire);
  if (enabled < 0) {
    const char *value = std::getenv("PS1_TRACE_PGXP_VERTICES");
    enabled = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_pgxpVertexTraceEnabled.store(enabled, std::memory_order_release);
  }
  if (enabled != 1)
    return false;

  if (g_pgxpVertexTraceBoundsResolved.load(std::memory_order_acquire) == 0) {
    g_pgxpVertexTraceFrameFrom.store(
        parseFrameBound("PS1_TRACE_PGXP_FRAME_FROM", 0),
        std::memory_order_relaxed);
    g_pgxpVertexTraceFrameTo.store(
        parseFrameBound("PS1_TRACE_PGXP_FRAME_TO",
                        std::numeric_limits<uint64_t>::max()),
        std::memory_order_relaxed);
    g_pgxpVertexTraceBoundsResolved.store(1, std::memory_order_release);
  }

  const uint64_t frame = traceFrame();
  return frame >= g_pgxpVertexTraceFrameFrom.load(std::memory_order_relaxed) &&
         frame <= g_pgxpVertexTraceFrameTo.load(std::memory_order_relaxed);
}

bool modernRejectTraceEnabledAtFrame(uint64_t frame) {
  if (!traceEnabled())
    return false;
  int enabled = g_modernRejectTraceEnabled.load(std::memory_order_acquire);
  if (enabled < 0) {
    const char *value = std::getenv("PS1_TRACE_MODERN_REJECTS");
    enabled = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_modernRejectTraceEnabled.store(enabled, std::memory_order_release);
  }
  if (enabled != 1)
    return false;

  if (g_modernRejectTraceBoundsResolved.load(std::memory_order_acquire) == 0) {
    g_modernRejectTraceFrameFrom.store(
        parseFrameBound("PS1_TRACE_MODERN_REJECT_FRAME_FROM", 0),
        std::memory_order_relaxed);
    g_modernRejectTraceFrameTo.store(
        parseFrameBound("PS1_TRACE_MODERN_REJECT_FRAME_TO",
                        std::numeric_limits<uint64_t>::max()),
        std::memory_order_relaxed);
    g_modernRejectTraceBoundsResolved.store(1, std::memory_order_release);
  }

  return frame >=
             g_modernRejectTraceFrameFrom.load(std::memory_order_relaxed) &&
         frame <= g_modernRejectTraceFrameTo.load(std::memory_order_relaxed);
}

bool modernRejectTraceEnabled() {
  return modernRejectTraceEnabledAtFrame(traceFrame());
}

void setTraceFrame(uint64_t frame) {
  g_traceFrame.store(frame, std::memory_order_release);
}

uint64_t traceFrame() { return g_traceFrame.load(std::memory_order_acquire); }

bool guestTraceArmed() {
  int mode = g_traceArmAfterLoadMode.load(std::memory_order_acquire);
  if (mode < 0) {
    const char *value = std::getenv("PS1_TRACE_ARM_AFTER_LOAD");
    mode = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_traceArmAfterLoadMode.store(mode, std::memory_order_release);
  }
  return mode == 0 || g_guestTraceLoaded.load(std::memory_order_acquire) != 0;
}

void armGuestTraceAfterLoad() {
  g_guestTraceLoaded.store(1, std::memory_order_release);
}

void trace(std::string_view event, std::initializer_list<TraceField> fields) {
  traceAtFrame(event, traceFrame(), fields);
}

void traceAtFrame(std::string_view event, uint64_t frame,
                  std::initializer_list<TraceField> fields) {
  if (!traceEnabled() || !traceEventEnabled(event))
    return;

  std::lock_guard<std::mutex> lock(g_traceMutex);
  if (!g_traceOut.is_open()) {
    const char *path = std::getenv("PS1_TRACE_JSONL");
    if (path == nullptr || *path == '\0')
      return;
    g_traceOut.open(path, std::ios::out | std::ios::trunc);
    if (!g_traceOut) {
      g_traceEnabled.store(0, std::memory_order_release);
      return;
    }
    g_traceEpoch = std::chrono::steady_clock::now();
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - g_traceEpoch);
  const char *source = std::getenv("PS1_TRACE_SOURCE");
  if (source == nullptr || *source == '\0')
    source = "ps1-recomp";

  g_traceOut << "{\"schema\":\"ps1trace/v1\",\"source\":";
  writeJsonString(g_traceOut, source);
  g_traceOut << ",\"seq\":" << ++g_traceSequence << ",\"frame\":" << frame
             << ",\"host_us\":" << elapsed.count() << ",\"event\":";
  writeJsonString(g_traceOut, event);
  for (const auto &field : fields) {
    g_traceOut << ',';
    writeJsonString(g_traceOut, field.name);
    g_traceOut << ':' << field.value;
  }
  g_traceOut << "}\n";
  // main_host may take the forced _Exit path, which bypasses ofstream
  // destruction.  A diagnostic trace must remain complete in that case.
  g_traceOut.flush();
}

void resetTraceForTesting() {
  std::lock_guard<std::mutex> lock(g_traceMutex);
  if (g_traceOut.is_open())
    g_traceOut.close();
  g_traceEnabled.store(-1, std::memory_order_relaxed);
  g_traceArmAfterLoadMode.store(-1, std::memory_order_relaxed);
  g_guestTraceLoaded.store(0, std::memory_order_relaxed);
  g_traceEventFilter.clear();
  g_traceEventFilterMode.store(-1, std::memory_order_relaxed);
  g_gteTraceEnabled.store(-1, std::memory_order_relaxed);
  detail::g_gteTraceFastGate.store(-1, std::memory_order_relaxed);
  g_gteTraceBoundsResolved.store(0, std::memory_order_relaxed);
  g_gteTraceTriggerMode.store(-1, std::memory_order_relaxed);
  g_gteTraceTriggeredFrame.store(std::numeric_limits<uint64_t>::max(),
                                 std::memory_order_relaxed);
  g_gteTraceTriggerFrames.store(1, std::memory_order_relaxed);
  g_gteTraceTriggerFrameFromResolved.store(0, std::memory_order_relaxed);
  g_gteTraceTriggerFrameFrom.store(0, std::memory_order_relaxed);
  g_gteTraceTriggerHitTarget.store(0, std::memory_order_relaxed);
  g_gteTraceTriggerHits.store(0, std::memory_order_relaxed);
  g_gteTraceFrameFrom.store(0, std::memory_order_relaxed);
  g_gteTraceFrameTo.store(std::numeric_limits<uint64_t>::max(),
                          std::memory_order_relaxed);
  g_gpuWordTraceEnabled.store(-1, std::memory_order_relaxed);
  g_gpuWordTraceBoundsResolved.store(0, std::memory_order_relaxed);
  g_gpuWordTraceTriggerMode.store(-1, std::memory_order_relaxed);
  g_gpuWordTraceTriggeredFrame.store(std::numeric_limits<uint64_t>::max(),
                                     std::memory_order_relaxed);
  g_gpuWordTraceTriggerFrames.store(2, std::memory_order_relaxed);
  g_gpuWordTraceTriggerFrameFromResolved.store(0,
                                                std::memory_order_relaxed);
  g_gpuWordTraceTriggerFrameFrom.store(0, std::memory_order_relaxed);
  g_gpuWordTraceTriggerHitTarget.store(0, std::memory_order_relaxed);
  g_gpuWordTraceTriggerHits.store(0, std::memory_order_relaxed);
  g_gpuWordTraceFrameFrom.store(0, std::memory_order_relaxed);
  g_gpuWordTraceFrameTo.store(std::numeric_limits<uint64_t>::max(),
                              std::memory_order_relaxed);
  g_projectionTraceFirstId.store(std::numeric_limits<uint64_t>::max(),
                                 std::memory_order_relaxed);
  g_projectionTraceLastId.store(0, std::memory_order_relaxed);
  g_projectionTraceMatchedSubmission.store(0, std::memory_order_relaxed);
  g_projectionTraceComplete.store(0, std::memory_order_relaxed);
  g_pgxpVertexTraceEnabled.store(-1, std::memory_order_relaxed);
  g_pgxpVertexTraceBoundsResolved.store(0, std::memory_order_relaxed);
  g_pgxpVertexTraceFrameFrom.store(0, std::memory_order_relaxed);
  g_pgxpVertexTraceFrameTo.store(std::numeric_limits<uint64_t>::max(),
                                 std::memory_order_relaxed);
  g_modernRejectTraceEnabled.store(-1, std::memory_order_relaxed);
  g_modernRejectTraceBoundsResolved.store(0, std::memory_order_relaxed);
  g_modernRejectTraceFrameFrom.store(0, std::memory_order_relaxed);
  g_modernRejectTraceFrameTo.store(std::numeric_limits<uint64_t>::max(),
                                   std::memory_order_relaxed);
  g_traceFrame.store(0, std::memory_order_relaxed);
  g_traceSequence = 0;
}

void count(std::string_view name, uint64_t n) {
  if (!enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_counters.find(name);
  if (it == g_counters.end())
    g_counters.emplace(std::string(name), n);
  else
    it->second += n;
}

void setState(std::string_view name, int64_t value) {
  if (!enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_states.find(name);
  if (it == g_states.end())
    g_states.emplace(std::string(name), value);
  else
    it->second = value;
}

uint64_t get(std::string_view name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_counters.find(name);
  return it == g_counters.end() ? 0u : it->second;
}

int64_t getState(std::string_view name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_states.find(name);
  return it == g_states.end() ? 0 : it->second;
}

bool dumpJsonTo(const char *path) {
  if (!path)
    return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::ofstream out(path);
  if (!out)
    return false;
  // Metric names are internal literals, never user input, so they need no
  // JSON escaping.  Keep it that way.
  out << "{\n  \"counters\": {";
  bool first = true;
  for (const auto &kv : g_counters) {
    out << (first ? "\n" : ",\n") << "    \"" << kv.first
        << "\": " << kv.second;
    first = false;
  }
  out << (first ? "" : "\n") << "  },\n  \"state\": {";
  first = true;
  for (const auto &kv : g_states) {
    out << (first ? "\n" : ",\n") << "    \"" << kv.first
        << "\": " << kv.second;
    first = false;
  }
  out << (first ? "" : "\n") << "  }\n}\n";
  return out.good();
}

bool dumpJson() {
  if (!enabled())
    return false;
  return dumpJsonTo(std::getenv("PS1_METRICS"));
}

void reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counters.clear();
  g_states.clear();
  g_traceFrame.store(0, std::memory_order_relaxed);
}

} // namespace ps1::metrics
