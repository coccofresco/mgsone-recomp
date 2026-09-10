#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

// Runtime metric collector.
//
// Exists because every legacy GPU log in this project is capped (FillRect at
// 10, CPU->VRAM at 20) or one-shot (the GP0 histogram fires at fixed word
// counts).  Two consecutive debugging sessions reached opposite conclusions
// from those logs.  Counters here are uncapped and are the source of truth for
// any decision.
//
// Disabled unless PS1_METRICS names an output path, so the hot path pays
// nothing in a normal run.
namespace ps1::metrics {

/// True when PS1_METRICS is set.  Resolved once from the environment.
bool enabled();

/// Add `n` to the counter `name`.  No-op while disabled.
void count(std::string_view name, uint64_t n = 1);

/// Record the latest value of `name`.  No-op while disabled.
void setState(std::string_view name, int64_t value);

/// Read a counter back.  Returns 0 when absent.
uint64_t get(std::string_view name);

/// Read a state field back.  Returns 0 when absent.
int64_t getState(std::string_view name);

/// Write the JSON report to the path in PS1_METRICS.
bool dumpJson();

/// Write the JSON report to an explicit path, ignoring PS1_METRICS.
bool dumpJsonTo(const char *path);

/// Drop everything recorded so far.
void reset();

/// Force the enabled flag, bypassing the environment.  For tests.
void setEnabledForTesting(bool on);

// Ordered, donor-comparable event trace.
//
// Set PS1_TRACE_JSONL to a path to enable it. PS1_TRACE_EVENTS may contain a
// comma-separated exact-name allowlist, keeping surgical captures small even
// when high-volume diagnostics exist elsewhere. Every record carries the
// current guest VBlank, a process-local sequence number, and microseconds
// since the first record. The event names and numeric field names form the
// portable contract used by tools/trace_compare.py and by the donor probes
// under tools/donor_trace/.
struct TraceField {
  std::string_view name;
  int64_t value;
};

bool traceEnabled();
void setTraceFrame(uint64_t frame);
uint64_t traceFrame();

/// Optional architectural gate for checkpoint diagnostics. With
/// PS1_TRACE_ARM_AFTER_LOAD=1, high-volume guest producers stay dormant until
/// the host confirms a successful timeline load. Ordinary traces are armed.
bool guestTraceArmed();
void armGuestTraceAfterLoad();

void trace(std::string_view event,
           std::initializer_list<TraceField> fields = {});
// Emit an event for an explicitly latched guest frame.  Use this when work is
// consumed asynchronously (for example, the renderer presenting a VBlank
// snapshot after the emulation thread has already advanced).
void traceAtFrame(std::string_view event, uint64_t frame,
                  std::initializer_list<TraceField> fields = {});

/// GTE traces are intentionally a separate opt-in because a gameplay run can
/// execute millions of commands.  Enable with PS1_TRACE_GTE=1 in addition to
/// PS1_TRACE_JSONL. Optional inclusive PS1_TRACE_GTE_FRAME_FROM/TO bounds
/// restrict capture to the scene being diagnosed. When
/// PS1_TRACE_GTE_TRIGGER_PC is set, capture stays off until the matching MIPS
/// checkpoint. It captures one guest frame by default; an explicit
/// PS1_TRACE_GTE_TRIGGER_FRAMES widens that bounded transport window so GTE
/// production can be paired with a delayed OT submission.
/// PS1_TRACE_GTE_TRIGGER_HIT=N selects the exact Nth checkpoint execution
/// after the load/frame gate (default 1), independent of host VBlank grouping.
bool gteTraceEnabled();

/// Arm GTE tracing for the current guest frame. The first trigger wins, so a
/// loop-back through the same checkpoint cannot grow a surgical trace into a
/// whole gameplay run.
void triggerGteTraceForCurrentFrame();

/// Raw GP0 input words form the narrowest useful boundary between the game's
/// ordering-table renderer and the GPU implementation. Enable with
/// PS1_TRACE_GPU_WORDS=1; optional inclusive
/// PS1_TRACE_GPU_FRAME_FROM/TO bounds keep texture uploads out of a focused
/// scene capture. PS1_TRACE_GPU_TRIGGER_PC replaces those bounds with a
/// semantic PC anchor and captures its frame plus the following frame, which
/// covers games that submit an ordering table one VBlank after building it.
/// PS1_TRACE_GPU_TRIGGER_HIT=N has the same exact checkpoint-occurrence
/// contract as the GTE producer; paired captures must use the same value.
bool gpuWordTraceEnabled();

/// Arm raw GP0 tracing at a semantic renderer checkpoint. First trigger wins.
void triggerGpuWordTraceForCurrentFrame();

/// Couple semantic GTE production to the first later GP0 batch that consumes
/// one of its exact projection IDs. These calls only stop a diagnostic capture
/// at an intrinsic join; elapsed frames never classify a depth family.
void noteGteProjectionForTrace(uint64_t projectionId);
void noteGpuProjectionForTrace(uint64_t projectionId, uint64_t submissionId);
void finishGpuTraceSubmission(uint64_t submissionId);

/// True after the intrinsic projection-ID/submission join has closed the
/// paired diagnostic capture. High-volume producers use this to stop their
/// private bookkeeping as well as their JSON output.
bool projectionTraceComplete();

/// Emit one diagnostic record for each polygon vertex rejected from the
/// high-precision path. Enable with PS1_TRACE_PGXP_VERTICES=1; optional
/// inclusive PS1_TRACE_PGXP_FRAME_FROM/TO bounds keep the trace surgical.
/// This is independent from PS1_METRICS so provenance can be diagnosed
/// without enabling the full counter collector.
bool pgxpVertexTraceEnabled();

/// Renderer-modern reject diagnostics are a separate opt-in so normal modern
/// rendering traces remain compact. Enable with PS1_TRACE_MODERN_REJECTS=1 in
/// addition to PS1_TRACE_JSONL; optional inclusive
/// PS1_TRACE_MODERN_REJECT_FRAME_FROM/TO bounds restrict capture to the guest
/// frames under investigation. This filter is generic and does not select a
/// particular reject reason or rendering backend.
bool modernRejectTraceEnabled();
// Asynchronous consumers must filter the frame carried by their immutable
// latch, not the emulation thread's current global trace frame.
bool modernRejectTraceEnabledAtFrame(uint64_t frame);

/// Close and forget trace state. Tests only; production uses one trace/run.
void resetTraceForTesting();

} // namespace ps1::metrics
