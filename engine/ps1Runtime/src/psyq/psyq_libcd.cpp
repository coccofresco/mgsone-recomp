#include "runtime/psyq/psyq_libcd.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

namespace ps1::psyq {

namespace {

// PsyQ libcd command bytes (subset relevant for boot path)
// Mirrors `enum CdlCommand` from PsyQ <libcd.h>.
constexpr uint8_t CDL_NOP       = 0x01;
constexpr uint8_t CDL_SETLOC    = 0x02;
constexpr uint8_t CDL_PLAY      = 0x03;
constexpr uint8_t CDL_READN     = 0x06;
constexpr uint8_t CDL_PAUSE     = 0x09;
constexpr uint8_t CDL_INIT      = 0x0A;
constexpr uint8_t CDL_SETFILTER = 0x0D;
constexpr uint8_t CDL_SETMODE   = 0x0E;
constexpr uint8_t CDL_GETTD     = 0x14;
constexpr uint8_t CDL_TEST      = 0x19;

// PsyQ libcd response codes
// PsyQ <libcd.h> enum CdlIntr: NoIntr=0, DataReady=1, Complete=2,
// Acknowledge=3, DataEnd=4, DiskError=5.
constexpr uint8_t CDL_NO_INTR    = 0;
constexpr uint8_t CDL_DATA_READY = 1;
constexpr uint8_t CDL_COMPLETE   = 2;
constexpr uint8_t CDL_DISK_ERROR = 5;

// PsyQ CDROM I/O ports (KSEG1 unmasked addresses)
constexpr uint32_t CDR_PORT0 = 0x1F801800; // status / index
constexpr uint32_t CDR_PORT1 = 0x1F801801; // command / response
constexpr uint32_t CDR_PORT2 = 0x1F801802; // parameter / data
constexpr uint32_t CDR_PORT3 = 0x1F801803; // IE / IF

// Number of parameter bytes the controller consumes for each command.  Anything
// missing here defaults to 0, which is correct for stat-only commands.  The
// hardware FIFO is cleared after each command, so over-pushing a few extra
// bytes on commands we haven't tabulated would be benign -- but pushing fewer
// than required would leave the controller waiting for parameters it never
// sees (e.g. CdlSetloc without M:S:F).
constexpr int paramCountForCmd(uint8_t com) {
  switch (com) {
  case CDL_SETLOC:    return 3; // M / S / F
  case CDL_SETFILTER: return 2; // file / channel
  case CDL_SETMODE:   return 1; // mode bits
  case CDL_GETTD:     return 1; // track number
  case CDL_TEST:      return 1; // sub-function
  default:            return 0;
  }
}

cdrom::CdromController *cdromOf(recomp_context *ctx) {
  return ctx->bios ? ctx->bios->cdromController() : nullptr;
}

// Push parameter bytes from PS1 RAM into the controller's parameter FIFO.
// Skips silently when paramPtr is NULL (game passed nullptr for a command
// that takes no parameters).
void pushParams(recomp_context *ctx, cdrom::CdromController *cdrom,
                uint32_t paramPtr, int count) {
  if (!cdrom || count <= 0 || paramPtr == 0)
    return;
  for (int i = 0; i < count; ++i)
    cdrom->writeRegister(CDR_PORT2, ctx->mem->read8(paramPtr + i));
}

// Drain up to 4 bytes of response into `*result`.  Real libcd CdControl
// fills 0..8 bytes depending on the command; the boot-path callers we care
// about only inspect the status byte, so 4 is a safe upper bound that
// avoids over-reading into the next response.
void drainResponse(recomp_context *ctx, cdrom::CdromController *cdrom,
                   uint32_t resultPtr) {
  if (!cdrom || resultPtr == 0)
    return;
  for (int i = 0; i < 4; ++i)
    ctx->mem->write8(resultPtr + i, cdrom->readRegister(CDR_PORT1));
}

// Issue a CD command synchronously through the controller register interface.
// The controller's interruptCallback (wired to bios.triggerCdromEvent in
// main_host.cpp) fires INT3/INT2 inline, so by the time this returns the
// game-thread BSS state already reflects the response.
void issueCommand(cdrom::CdromController *cdrom, uint8_t com) {
  if (!cdrom) return;
  cdrom->writeRegister(CDR_PORT0, 0); // index = 0
  cdrom->writeRegister(CDR_PORT1, com);
  // This HLE driver consumes command completion via the queued BIOS event,
  // rather than a guest register-level IRQ handler. Release its hardware ACK
  // too, so a ReadN sector cannot be held behind an already-consumed INT3.
  // Keep the response bytes available to CdControl's drainResponse below.
  if (psyq_state().intrCallback[2] == 0)
    cdrom->ackInterrupt(0x1f);
}

} // namespace

// CdInit
//
// HLE replacement for the native PsyQ CdInit.  We deliberately bypass the
// retry loop / B0:42 verification that "Init failed" comes from -- the
// hardware controller is trivially reset and INT3+INT2 are delivered
// synchronously, so the state machine ends in cdSyncByte=2 and CdInit
// always succeeds.
//
void hle_libcd_CdInit(recomp_context *ctx) {
  auto *bios = ctx->bios;
  if (!bios) {
    ctx->r[V0] = 0;
    return;
  }

  if (auto *cdrom = bios->cdromController()) {
    // Send CdlInit through the register interface so cdrom_controller
    // transitions Idle / motorOn / mode=0 like a real reset.
    cdrom->writeRegister(CDR_PORT0, 0);
    cdrom->writeRegister(CDR_PORT1, CDL_INIT);
    // The controller queues INT3+INT2 itself; we deliver them ourselves
    // (next two lines), so cancel the queued copy to avoid a double event.
    cdrom->cancelPendingInterrupt();
  }

  // Drive the BIOS event chain that mirrors what the PsyQ IRQ handler
  // would do: INT3 = Acknowledge, INT2 = Complete.  Both end up in
  // psyq_state().cdSyncByte = 2 (see bios.cpp::triggerCdromEvent).
  bios->triggerCdromEvent(3);
  bios->triggerCdromEvent(2);

  // Reset the read-state slots that bios.cpp's HLE sector copy consults.
  auto &state = psyq_state();
  state.cdRemaining = 0;
  state.cdDestPtr   = 0;
  state.cdWordCount = 0;
  state.cdClockDemand.store(false, std::memory_order_release);

  ctx->r[V0] = 1; // success
}

// CdRead(sectors, *buf, mode)
//
// Updates the read-state slots in psyq_state() that bios.cpp consults from
// triggerCdromEvent(INT1)/drainPendingCallbacks, then issues CdlSetmode +
// CdlReadN.  CdlSetloc must already have been sent by the caller (PsyQ
// requires it before CdRead).
//
// CdRead is asynchronous on hardware. CdReadSync below owns the cooperative
// wait, while the host master clock advances the controller at VBlank cadence.
// Adding a sector's worth of cycles per loop here used to make disc reads
// deterministic, but collapsed emulated time: Crash 3 loaded all boot audio
// in microseconds and started cues roughly half a second before the donors.
// The stream pump may publish a sector after an ACK only when the host clock
// already accumulated its timing credit.
//
void hle_libcd_CdRead(recomp_context *ctx) {
  auto *bios = ctx->bios;
  if (!bios) { ctx->r[V0] = 0; return; }

  int32_t  sectors = static_cast<int32_t>(ctx->r[A0]);
  uint32_t bufPtr  = ctx->r[A1];
  uint8_t  mode    = static_cast<uint8_t>(ctx->r[A2]);

  // PsyQ uses 2048-byte sectors by default (Mode-2 form-1 with bit 5 of
  // mode_ unset), so the BIOS-side sector copy needs 512 32-bit words.
  // When mode bit 5 (0x20) is set the controller switches to 2340-byte
  // sectors (whole-sector mode); the BIOS copies 585 words in that case.
  const uint32_t wordCount = (mode & 0x20) ? 585u : 512u;

  auto &state = psyq_state();
  const uint32_t remainingBefore = state.cdRemaining;
  const uint32_t destBefore = state.cdDestPtr;
  const uint32_t wordsBefore = state.cdWordCount;
  // Preserve the runtime's legacy CdRead(sectors=0, ...) continuation
  // compatibility. The instrumented Crash 3 reference path does not use
  // this undocumented form; positive-sector calls own every observed read.
  // Zeroing `cdRemaining` here (or re-issuing CdlReadN, which
  // would reset `currentLba_` to the latest Setloc target) would discard
  // the in-flight sector when INT1 arrives.  Preserve the prior count
  // when sectors==0 and skip the controller commands entirely.
  if (sectors > 0)
    state.cdRemaining = static_cast<uint32_t>(sectors);
  if (state.cdRemaining > 0)
    state.cdClockDemand.store(true, std::memory_order_release);
  state.cdDestPtr   = bufPtr;
  state.cdWordCount = wordCount;

  if (sectors > 0) {
    if (auto *cdrom = bios->cdromController()) {
      // CdlSetmode with the requested mode byte.
      cdrom->writeRegister(CDR_PORT0, 0);
      cdrom->writeRegister(CDR_PORT2, mode);
      cdrom->writeRegister(CDR_PORT1, CDL_SETMODE);

      // CdlReadN -- current Setloc target, no parameters.
      issueCommand(cdrom, CDL_READN);

    }
  }

  // Low-volume, allowlist-gated evidence at the libcd ownership boundary.
  // This distinguishes a genuinely late read from a follow-up call that
  // redirects an already active transfer into a different guest buffer.
  ps1::metrics::trace(
      "psyq.cd_read",
      {{"sectors", sectors},
       {"buffer", bufPtr},
       {"mode", mode},
       {"return_address", ctx->r[RA]},
       {"remaining_before", remainingBefore},
       {"remaining_after", state.cdRemaining},
       {"destination_before", destBefore},
       {"destination_after", state.cdDestPtr},
       {"words_before", wordsBefore},
       {"words_after", state.cdWordCount}});

  ctx->r[V0] = 1;
}

// Cooperative wait on psyq_state() CD bytes (Phase 2.3)
//
// The CDROM IRQ path (Bios::triggerCdromEvent) writes to
// psyq_state().cdSyncByte / cdReadyByte atomically.  These helpers poll the
// atomic with a 100 us sleep between checks, draining BIOS callbacks per
// iteration so any pending mode-0x1000 handler runs on the game thread.  The
// 5 s deadline matches the previous Bios::waitForCdSync timeout.
//
namespace {

class ScopedAutonomousClockWait {
public:
  ScopedAutonomousClockWait() {
    psyq_state().autonomousClockWaiters.fetch_add(1,
                                                  std::memory_order_release);
  }
  ~ScopedAutonomousClockWait() {
    psyq_state().autonomousClockWaiters.fetch_sub(1,
                                                  std::memory_order_release);
  }

  ScopedAutonomousClockWait(const ScopedAutonomousClockWait &) = delete;
  ScopedAutonomousClockWait &
  operator=(const ScopedAutonomousClockWait &) = delete;
};

template <typename Atomic>
uint8_t waitOnCdAtomic(Atomic &slot) {
  // Reset to "pending" so we observe the NEXT CDROM event, not the previous
  // one -- same semantics the prior cv-based path enforced explicitly.
  slot.store(0, std::memory_order_release);
  ScopedAutonomousClockWait clockWait;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t v = slot.load(std::memory_order_acquire);
    if (v != 0)
      return v;
    if (getConfig().drainCallbacks)
      getConfig().drainCallbacks();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return 0; // timeout
}

} // namespace

// CdSync(mode, *result)
//
// In our model every CdControl-style call is synchronous (the controller
// fires INT3+INT2 inline), so by the time the game polls CdSync the state
// is usually already CdlComplete (2).  When mode=0 the caller wants to
// block until the next completion event -- we poll psyq_state().cdSyncByte
// cooperatively and surface the new value (2 or 5; 0 -> timeout maps to
// CdlNoIntr).  When mode!=0 (poll), we return Complete unconditionally
// because synchronous CD command dispatch guarantees the prior command
// already finished.
//
void hle_libcd_CdSync(recomp_context *ctx) {
  uint32_t mode      = ctx->r[A0];
  uint32_t resultPtr = ctx->r[A1];

  // Consume host clock credit on the game thread before observing command
  // status. This is the single-owner boundary for controller state.
  if (auto *bios = ctx->bios) {
    if (auto *cdrom = bios->cdromController())
      cdrom->serviceClock();
    bios->drainCdromEventQueue();
  }

  uint8_t code = CDL_COMPLETE;
  if (mode == 0) {
    uint8_t v = waitOnCdAtomic(psyq_state().cdSyncByte);
    code = (v == 0) ? CDL_NO_INTR : v;
  }

  if (resultPtr != 0) {
    // libcd places the 8-byte status response at *result.  We don't model
    // the full response layout, so write the status byte and zero the rest.
    ctx->mem->write8(resultPtr + 0, code);
    for (int i = 1; i < 8; ++i)
      ctx->mem->write8(resultPtr + i, 0);
  }

  ctx->r[V0] = code;
}

// CdReady(mode, *result)
//
// Same shape as CdSync but for data-ready (INT1/INT4) interrupts.  Phase 2.3
// migrated the ready byte into psyq_state().cdReadyByte; mode=0 (block) does
// the cooperative poll, mode!=0 (poll) reads the current value (or returns
// CdlDataReady if nothing has arrived -- keeps callers that poll
// non-blocking and then hit CdGetSector from deadlocking).
//
void hle_libcd_CdReady(recomp_context *ctx) {
  uint32_t mode      = ctx->r[A0];
  uint32_t resultPtr = ctx->r[A1];

  if (auto *bios = ctx->bios) {
    if (auto *cdrom = bios->cdromController())
      cdrom->serviceClock();
    bios->drainCdromEventQueue();
  }

  uint8_t code = CDL_NO_INTR;
  if (mode == 0) {
    uint8_t v = waitOnCdAtomic(psyq_state().cdReadyByte);
    code = (v == 0) ? CDL_NO_INTR : v;
  } else {
    uint8_t v = psyq_state().cdReadyByte.load(std::memory_order_acquire);
    code = (v == 0) ? CDL_DATA_READY : v;
  }

  if (resultPtr != 0) {
    ctx->mem->write8(resultPtr + 0, code);
    for (int i = 1; i < 8; ++i)
      ctx->mem->write8(resultPtr + i, 0);
  }

  ctx->r[V0] = code;
}

// CdReadSync(mode, *result)
//
// Companion to the CdRead HLE.  `cdRemaining` counts the sectors still
// pending in the active read; the INT1 sector copy that decrements it runs
// on this thread during callback drains (bios.cpp::triggerCdromEvent).
// mode==0 blocks until the transfer completes, mode!=0 polls.  Returns the
// number of sectors left (0 = done) or -1 on timeout, matching libcd.
//
// The recompiled MIPS CdReadSync carries a bounded retry budget calibrated
// for real drive latency. Host cadence is accumulated atomically, then
// consumed by the controller on this thread so the retry outcome cannot race
// sector-buffer publication.
//
void hle_libcd_CdReadSync(recomp_context *ctx) {
  uint32_t mode      = ctx->r[A0];
  uint32_t resultPtr = ctx->r[A1];
  auto &state = psyq_state();

  // Crash 3 uses mode=1 as its asynchronous streaming pump. First transfer
  // host clock credit into the game-thread-owned controller, consume the
  // resulting INT1, then permit already-matured credit to advance only after
  // the preceding sector was ACKed. This is iterative across polling yields,
  // never recursive in the IRQ handler.
  if (auto *bios = ctx->bios) {
    if (auto *cdrom = bios->cdromController()) {
      cdrom->serviceClock();
      bios->drainCdromEventQueue();
      cdrom->serviceMaturedSector();
    }
    bios->drainCdromEventQueue();
  }

  if (mode == 0) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state.cdRemaining > 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        if (resultPtr != 0)
          for (int i = 0; i < 8; ++i)
            ctx->mem->write8(resultPtr + i, 0);
        ctx->r[V0] = static_cast<uint32_t>(-1);
        return;
      }
      if (auto *bios = ctx->bios) {
        if (auto *cdrom = bios->cdromController()) {
          cdrom->serviceClock();
          bios->drainCdromEventQueue();
          cdrom->serviceMaturedSector();
        }
        bios->drainCdromEventQueue();
      }
      if (getConfig().drainCallbacks)
        getConfig().drainCallbacks();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

  if (resultPtr != 0) {
    ctx->mem->write8(resultPtr + 0, CDL_COMPLETE);
    for (int i = 1; i < 8; ++i)
      ctx->mem->write8(resultPtr + i, 0);
  }
  ctx->r[V0] = state.cdRemaining;
  if (state.cdRemaining == 0)
    state.cdClockDemand.store(false, std::memory_order_release);
}

// CdControl(com, *param, *result)
//
// Push the right number of parameter bytes, issue the command, drain the
// status response.  The controller fires the appropriate interrupt inline
// (via interruptCallback wired in main_host.cpp), so this is effectively
// synchronous as far as the game thread is concerned.
//
void hle_libcd_CdControl(recomp_context *ctx) {
  uint8_t  com       = static_cast<uint8_t>(ctx->r[A0]);
  uint32_t paramPtr  = ctx->r[A1];
  uint32_t resultPtr = ctx->r[A2];

  auto *cdrom = cdromOf(ctx);
  if (!cdrom) {
    ctx->r[V0] = 0;
    return;
  }

  pushParams(ctx, cdrom, paramPtr, paramCountForCmd(com));
  issueCommand(cdrom, com);
  drainResponse(ctx, cdrom, resultPtr);

  ctx->r[V0] = 1;
}

// CdControlF(com, *param)
//
// Fire-and-forget variant of CdControl: same dispatch, no result drain.
//
void hle_libcd_CdControlF(recomp_context *ctx) {
  uint8_t  com      = static_cast<uint8_t>(ctx->r[A0]);
  uint32_t paramPtr = ctx->r[A1];

  auto *cdrom = cdromOf(ctx);
  if (!cdrom) {
    ctx->r[V0] = 0;
    return;
  }

  pushParams(ctx, cdrom, paramPtr, paramCountForCmd(com));
  issueCommand(cdrom, com);

  ctx->r[V0] = 1;
}

// CdGetSector(*madr, size_words)
//
// Streaming reads land in game RAM via the BIOS HLE sector copy (driven
// from triggerCdromEvent INT1 + drainPendingCallbacks), so by the time the
// game asks CdGetSector for the buffer there's nothing buffered on our
// side to hand back.  Returning 0 reports "not ready" -- the typical caller
// (XA streamer) falls through; the data callback on INT1 is the real path.
//
void hle_libcd_CdGetSector(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

namespace {

// Shared body for the three callback setters: read prev, store new, return prev
// in v0.  All three currently update psyq_state().cdDataCb because bios.cpp
// dispatches a single data callback for INT1 / INT4 (the difference between
// Read and Ready is mostly which interrupts the game expects, and our INT
// routing folds them).
void setDataCallback(recomp_context *ctx) {
  uint32_t fn = ctx->r[A0];
  auto &slot = psyq_state().cdDataCb;
  uint32_t prev = slot;
  slot = fn;
  ctx->r[V0] = prev;
}

} // namespace

void hle_libcd_CdReadCallback(recomp_context *ctx)  { setDataCallback(ctx); }
void hle_libcd_CdReadyCallback(recomp_context *ctx) { setDataCallback(ctx); }
void hle_libcd_CdDataCallback(recomp_context *ctx)  { setDataCallback(ctx); }

// CdMix(*vol): same port sequence as USA retail CD_mix at 0x80090824.
// CdlATV bytes are LL, LR, RR, RL; applying the matrix does not ACK an IRQ.
void hle_libcd_CdMix(recomp_context *ctx) {
  if (auto *cd = cdromOf(ctx); cd && ctx->r[A0]) {
    const auto vol = ctx->r[A0];
    cd->writeRegister(CDR_PORT0, 2);
    cd->writeRegister(CDR_PORT2, ctx->mem->read8(vol));
    cd->writeRegister(CDR_PORT3, ctx->mem->read8(vol + 1));
    cd->writeRegister(CDR_PORT0, 3);
    cd->writeRegister(CDR_PORT1, ctx->mem->read8(vol + 2));
    cd->writeRegister(CDR_PORT2, ctx->mem->read8(vol + 3));
    cd->writeRegister(CDR_PORT3, 0x20);
  }
  ctx->r[V0] = 1;
}

// CdReadBreak()
//
// Cancel an in-progress CdRead.  Stops the controller's read state machine
// and zeros the read counters in psyq_state() so the next INT1 doesn't
// re-enter the sector-copy path.
//
void hle_libcd_CdReadBreak(recomp_context *ctx) {
  auto *bios = ctx->bios;
  if (!bios) { ctx->r[V0] = 0; return; }

  if (auto *cdrom = bios->cdromController())
    cdrom->stopReading();

  auto &state = psyq_state();
  state.cdRemaining = 0;
  state.cdDestPtr   = 0;
  state.cdWordCount = 0;
  state.cdClockDemand.store(false, std::memory_order_release);

  ctx->r[V0] = 1;
}

// StSetMask
// XA-stream sector filter.  Not modeled.
void hle_libcd_StSetMask(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

void psyq_register_libcd() {
  psyq_register("libcd_CdInit",          &hle_libcd_CdInit);
  psyq_register("libcd_CdRead",          &hle_libcd_CdRead);
  psyq_register("libcd_CdSync",          &hle_libcd_CdSync);
  psyq_register("libcd_CdReady",         &hle_libcd_CdReady);
  psyq_register("libcd_CdReadSync",      &hle_libcd_CdReadSync);
  psyq_register("libcd_CdControl",       &hle_libcd_CdControl);
  psyq_register("libcd_CdControlF",      &hle_libcd_CdControlF);
  psyq_register("libcd_CdGetSector",     &hle_libcd_CdGetSector);
  psyq_register("libcd_CdReadCallback",  &hle_libcd_CdReadCallback);
  psyq_register("libcd_CdReadyCallback", &hle_libcd_CdReadyCallback);
  psyq_register("libcd_CdDataCallback",  &hle_libcd_CdDataCallback);
  psyq_register("libcd_CdMix",           &hle_libcd_CdMix);
  psyq_register("libcd_CdReadBreak",     &hle_libcd_CdReadBreak);
  psyq_register("libcd_StSetMask",       &hle_libcd_StSetMask);
  // CdDataSync/CD_datasync: public wrapper and internal implementation used
  // by the Crash NS loader.  Mixing the native implementation with the HLE
  // CdRead path makes it poll PsyQ's private DMA state, which HLE transfers
  // intentionally bypass.  The sector copy is complete when CdReadSync
  // reaches this call, so report "ready, no error" from both entry points.
  auto cdDataSync = [](recomp_context *ctx) {
    ctx->r[V0] = 0;
  };
  psyq_register("libcd_CdDataSync", cdDataSync);
  psyq_register("libcd_CD_datasync", cdDataSync);
}

} // namespace ps1::psyq
