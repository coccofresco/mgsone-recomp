#include "bios_internal.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/games/crash3_profile.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/psyq/psyq_pad.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fmt/format.h>

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global
// namespace)
extern void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace ps1::bios {

using detail::readString;

// Constructor / Destructor

Bios::Bios(recomp_context &ctx, cdrom::VirtualFs &fs, Memory &mem)
    : ctx_(ctx), heap_(mem), eventSystem_(ctx), fileIo_(fs, mem) {
  fileIo_.setCardCompletion([this](unsigned, unsigned bytes, bool success) {
    // MGS decrements its pending byte count on every 128-byte HwCARD event.
    for (unsigned frame = 0; frame < (success ? (bytes + 127) / 128 : 1); ++frame)
      cardEvents_.push({0xF0000011u, success ? 4u : 0x8000u});
    cardEvents_.push({0xF4000001u, success ? 4u : 0x8000u});
  });
  // Reset syscall and handlers
  // In a real PS1, vectors are at 0x80000080

  // Set up A0, B0, and C0 sentinel tables at the end of RAM (2MB limit).
  // When a game reads GetXTable()[i] and does `jalr $v0`, $v0 will contain
  // the sentinel 0x0000X0xx, which recomp_dispatch() maps to the BIOS handler.
  // This covers both direct BIOS calls and indirect calls through stored table
  // pointers (used by some games like Tomba that cache table pointers in BSS).
  //
  // Layout (growing downward from end of RAM):
  //   A0 table: 0xC0 entries * 4 = 0x300 bytes  -> starts at 0x801FFD00
  //   B0 table: 0x60 entries * 4 = 0x180 bytes  -> starts at 0x801FFE00 (was -0x200)
  //   C0 table: 0x20 entries * 4 = 0x080 bytes  -> starts at 0x801FFF80
  uint32_t a0Addr = 0x80000000 + (2 * 1024 * 1024) - 0x500; // 0x801FFB00
  uint32_t b0Addr = 0x80000000 + (2 * 1024 * 1024) - 0x200; // 0x801FFE00
  uint32_t c0Addr = b0Addr + 0x180;                         // 0x801FFF80

  for (int i = 0; i < 0xC0; i++) {
    mem.write32(a0Addr + i * 4, 0x0000A000 + i); // Sentinel for A0:i
  }
  for (int i = 0; i < 0x60; i++) {
    mem.write32(b0Addr + i * 4, 0x0000B000 + i); // Sentinel for B0:i
  }
  for (int i = 0; i < 0x20; i++) {
    mem.write32(c0Addr + i * 4, 0x0000C000 + i); // Sentinel for C0:i
  }

  // Store the table pointers where BIOS calls can return them.
  // K0/K1 are kernel-reserved registers; the game must not rely on them.
  ctx_.r[K0] = b0Addr;
  ctx_.r[K1] = c0Addr;
}

Bios::~Bios() = default;

Bios::Snapshot Bios::snapshot() const {
  Snapshot state;
  state.eventSystem = eventSystem_.snapshot();
  state.padBuf1Addr = padBuf1Addr_;
  state.padBuf1Size = padBuf1Size_;
  state.padBuf2Addr = padBuf2Addr_;
  state.padBuf2Size = padBuf2Size_;
  state.padActive = padActive_;
  state.cdIntPending = cdIntPending_.load(std::memory_order_acquire);
  {
    std::lock_guard lock(cdEventQueueMtx_);
    auto pending = cdEventQueue_;
    while (!pending.empty()) {
      state.cdEventQueue.push_back(pending.front());
      pending.pop();
    }
  }
  state.cdExceptionPending =
      cdExceptionPending_.load(std::memory_order_acquire);
  state.lastIntrTickFrame = lastIntrTickFrame_;
  state.lastDmaCompletion = lastDmaCompletion_;
  state.customExceptionBuffer = customExceptionBuffer_;
  return state;
}

bool Bios::restore(const Snapshot &state) {
  const auto validRamAddress = [](uint32_t address, uint32_t bytes) {
    if (address == 0)
      return true;
    const uint32_t physical = address & 0x1FFFFFFFu;
    return bytes <= Memory::RAM_SIZE && physical <= Memory::RAM_SIZE - bytes;
  };
  if (state.cdIntPending > 5 || state.cdExceptionPending > 5 ||
      !validRamAddress(state.padBuf1Addr, state.padBuf1Size) ||
      !validRamAddress(state.padBuf2Addr, state.padBuf2Size) ||
      !validRamAddress(state.customExceptionBuffer,
                       state.customExceptionBuffer == 0 ? 0u : 48u) ||
      !std::all_of(
          state.cdEventQueue.begin(), state.cdEventQueue.end(),
          [](uint8_t interrupt) { return interrupt >= 1 && interrupt <= 5; }) ||
      !eventSystem_.restore(state.eventSystem))
    return false;

  padBuf1Addr_ = state.padBuf1Addr;
  padBuf1Size_ = state.padBuf1Size;
  padBuf2Addr_ = state.padBuf2Addr;
  padBuf2Size_ = state.padBuf2Size;
  padActive_ = state.padActive;
  cdIntPending_.store(state.cdIntPending, std::memory_order_release);
  {
    std::lock_guard lock(cdEventQueueMtx_);
    std::queue<uint8_t> empty;
    cdEventQueue_.swap(empty);
    for (uint8_t interrupt : state.cdEventQueue)
      cdEventQueue_.push(interrupt);
  }
  cdExceptionPending_.store(state.cdExceptionPending,
                            std::memory_order_release);
  lastIntrTickFrame_ = state.lastIntrTickFrame;
  lastDmaCompletion_ = state.lastDmaCompletion;
  customExceptionBuffer_ = state.customExceptionBuffer;
  if (customExceptionBuffer_ != 0) {
    const uint32_t buffer = customExceptionBuffer_;
    customExceptionCallback_ = [this, buffer]() {
      hle_longjmp_emulator(ctx_, *ctx_.mem, buffer);
    };
    customExceptionRegistered_.store(true, std::memory_order_release);
  } else {
    customExceptionCallback_ = nullptr;
    customExceptionRegistered_.store(false, std::memory_order_release);
  }
  return true;
}

void Bios::rebaseAfterTimelineLoad() {
  eventSystem_.discardPendingTimeline();
  cdIntPending_.store(0, std::memory_order_release);
  cdExceptionPending_.store(0, std::memory_order_release);
  {
    std::lock_guard lock(cdEventQueueMtx_);
    std::queue<uint8_t> empty;
    cdEventQueue_.swap(empty);
  }
  lastIntrTickFrame_ =
      ps1::psyq::psyq_state().vsyncCounter.load(std::memory_order_acquire);
  if (dma_) {
    for (std::size_t channel = 0; channel < lastDmaCompletion_.size();
         ++channel)
      lastDmaCompletion_[channel] = dma_->completionGeneration(channel);
  } else {
    lastDmaCompletion_.fill(0);
  }
}

// Entry Points

void Bios::executeA0() {
  uint32_t index =
      ctx_.r[T1]; // In PS1 BIOS standard, t1(r9) holds the function index
  BIOS_LOG("[BIOS] A0:{:02X} (a0={:08X} a1={:08X} RA={:08X})\n", index,
           ctx_.r[A0], ctx_.r[A1], ctx_.r[RA]);
  handleA0(index);
}

void Bios::executeB0() {
  uint32_t index = ctx_.r[T1];
  BIOS_LOG("[BIOS] B0:{:02X} (a0={:08X} a1={:08X} a2={:08X} RA={:08X})\n",
           index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], ctx_.r[RA]);
  handleB0(index);
}

void Bios::executeC0() {
  uint32_t index = ctx_.r[T1];
  BIOS_LOG("[BIOS] C0:{:02X} (a0={:08X} a1={:08X} RA={:08X})\n", index,
           ctx_.r[A0], ctx_.r[A1], ctx_.r[RA]);
  handleC0(index);
}

// Printf Implementation

// CDROM Event Triggering
//
// The PS1 BIOS CDROM interrupt handler translates CDROM INT types
// into event system notifications.  In a real PS1 the IRQ handler
// at 0x80000080 would do this, but we HLE it here.
//
// Mapping (empirically derived from Rayman openEvent calls):
//   Rayman opens class 0xF4000001 with 4 specs: {0x0004, 0x8000, 0x0100, 0x2000}
//   Matching against CDROM INT types:
//   INT1 (DataReady)   -> event spec 0x0100  <- Rayman ID2 (sector read complete)
//   INT2 (Complete)    -> event spec 0x8000  <- Rayman ID1 (init/cmd done)
//   INT3 (Acknowledge) -> event spec 0x0004  <- Rayman ID0 (cmd accepted)
//   INT4 (DataEnd)     -> event spec 0x0100  <- same as DataReady
//   INT5 (DiskError)   -> event spec 0x2000  <- Rayman ID3 (error)

static constexpr uint32_t CDROM_EVENT_CLASS_1 = 0xF4000001;
static constexpr uint32_t CDROM_EVENT_CLASS_2 = 0xF0000011;

static uint32_t cdIntToEventSpec(uint8_t intType) {
  switch (intType) {
  case 1:
    return 0x0100; // INT1 DataReady
  case 2:
    return 0x8000; // INT2 Complete
  case 3:
    return 0x0004; // INT3 Acknowledge
  case 4:
    return 0x0100; // INT4 DataEnd (same class as DataReady)
  case 5:
    return 0x2000; // INT5 DiskError
  default:
    return 0;
  }
}

void Bios::queueCdromEvent(uint8_t cdIntType) {
  // Push under a short mutex.  This is the only cross-thread CDROM IRQ
  // entry-point: `cdromCtrl.setInterruptCallback` (wired in main_host.cpp)
  // routes here from both the SDL-render-thread `tick()` path and the
  // game-thread synchronous `writeRegister` path.  The actual side-effects
  // (psyq_state writes, eventSystem_.triggerEvent, cdIntPending_ set)
  // happen later in `drainCdromEventQueue` on the game thread.
  {
    std::lock_guard<std::mutex> lk(cdEventQueueMtx_);
    cdEventQueue_.push(cdIntType);
  }

  // Inline-drain when the push originates from the game thread itself
  // (recompiled MIPS port writes -> cdromCtrl.writeRegister -> this
  // callback fires synchronously on the game thread).  Recompiled PsyQ
  // kernel polls BSS mirrors right after the port write -- without the
  // inline drain, triggerCdromEvent would only run later (next yield
  // point) and the polling loop would observe stale mirror state.
  // Cross-thread pushes (SDL render thread via tick) skip the drain;
  // they're handled by the next drainPendingCallbacks / hle_libcd_CdSync.
  if (std::this_thread::get_id() == gameThreadId_) {
    drainCdromEventQueue();
  }
}

std::size_t Bios::drainCdromEventQueue() {
  // Pop everything under the mutex into a local queue, then drop the lock
  // before dispatching -- `triggerCdromEvent` may re-enter via cdrom_->tick
  // during the pump-loop pass and we don't want to recursively lock.
  std::queue<uint8_t> local;
  {
    std::lock_guard<std::mutex> lk(cdEventQueueMtx_);
    std::swap(local, cdEventQueue_);
  }
  std::size_t count = local.size();
  while (!local.empty()) {
    triggerCdromEvent(local.front());
    local.pop();
  }
  return count;
}

void Bios::triggerCdromEvent(uint8_t cdIntType) {
  uint32_t spec = cdIntToEventSpec(cdIntType);
  if (spec == 0)
    return;
  BIOS_LOG("[BIOS] triggerCdromEvent: INT{} -> spec 0x{:04X}\n", cdIntType,
           spec);

  // A registered low-level PsyQ driver owns the response FIFO, DMA and ACK.
  // Queue it at a cooperative boundary rather than consuming its response
  // through the high-level CdControl/CdRead replacement as well.
  if (const uint32_t handler = ps1::psyq::psyq_state().intrCallback[2]) {
    eventSystem_.queueCallback(handler);
    return;
  }

  // HLE PsyQ CDROM interrupt handler variables
  //
  // On a real PS1 the BIOS exception handler at 0x80000080 reads the CDROM
  // interrupt flag + response FIFO and stores the results in PsyQ-internal
  // RAM variables.  The PsyQ CdSync / CdControl polling functions then check
  // these variables instead of reading CDROM hardware registers directly.
  //
  // In our recompiled environment the exception handler never runs, so we
  // HLE the relevant writes here.  Phase 2.3 moved cdSyncByte/cdReadyByte
  // out of per-game BSS into `psyq_state()` (atomic uint8_t), so VSync-style
  // cooperative polling works without configuring a memory address.  The
  // legacy cdStatusHw write was a one-way mirror with no reader after HLE
  // coverage landed -- dropped entirely.
  //

  // HLE sector copy for INT1 (DataReady)
  //
  // IMPORTANT: Do the sector copy BEFORE writing the ready byte to avoid a
  // race with the game thread's polling loop.  The game thread checks the
  // ready byte and immediately reads the destination buffer, so the data
  // must already be there when the signal is visible.
  //
  // On a real PS1, INT1 fires asynchronously and the interrupt handler chain
  // programs DMA Ch3 to copy the sector, then sets the ready byte.  We HLE
  // the entire sector copy right here on the main thread:
  //   1. Read remaining/destPtr/wordCount from PsyQ BSS
  //   2. memcpy from CDROM sector buffer to game RAM
  //   3. Update destPtr and decrement remaining
  //   4. If remaining <= 0: issue CdlPause so the CDROM stops reading
  //   5. ACK + clear waitingForAck_ so the next sector can arrive
  //
  // APPROACH: Sector copies are handled by the game's own PsyQ callback
  // (dispatched from drainPendingCallbacks on the game thread).
  //
  // We set cdIntPending_ so drainPendingCallbacks on the game thread
  // dispatches the game's dataCb. When no callback is registered, the HLE
  // copy below is the complete interrupt handler: it also ACKs inline after
  // consuming the sector. This keeps the host clock as the only source of
  // new sectors without leaving the controller permanently waiting for an
  // ACK that no callback will ever provide.
  //
  // On a real PS1 the entire interrupt -> callback -> ACK chain runs
  // atomically within the interrupt handler. Here the same chain is owned by
  // the game thread; callback-based reads may span cooperative yield points,
  // while no-callback HLE reads complete it inline.
  //
  if (cdIntType == 1 && cdrom_ && !cdrom_->hasSectorReady() &&
      ps1::psyq::psyq_state().cdRemaining > 0) {
    // An INT1 without a published sector cannot be consumed safely. This was
    // the signature of the old cross-thread double-publication race: the
    // first queued event consumed the single buffer and the duplicate event
    // advanced libcd state without data. Ignore it and release a stale gate;
    // normal register-DMA and HLE reads both have sectorReady set here.
    ps1::metrics::trace("cdrom.spurious_data_ready",
                        {{"remaining", ps1::psyq::psyq_state().cdRemaining},
                         {"data_callback", ps1::psyq::psyq_state().cdDataCb}});
    cdrom_->ackInterrupt(0x1F);
    cdrom_->clearWaitingForAck();
    return;
  }

  if (cdIntType == 1 && cdrom_) {
    cdIntPending_.store(cdIntType, std::memory_order_release);
    // Callback reads remain pending until drainPendingCallbacks dispatches
    // the callback and acknowledges them.

    auto &state = ps1::psyq::psyq_state();
    const uint32_t remainingBefore = state.cdRemaining;
    const uint32_t destinationBefore = state.cdDestPtr;
    bool hleCopiedSector = false;
    bool acknowledgedInline = false;

    // HLE register-direct sector copy
    // When the game programs DMA Ch3 directly (no libcd HLE in flight:
    // cdRemaining == 0 and cdDataCb == 0), the channel was previously
    // armed before the sector was ready -- `DMA::executeChannel` deferred
    // it and left the start bit set.  Now that INT1 has fired, retry
    // any armed channels so the deferred CDROM transfer runs.  Safe to
    // call unconditionally: other channels won't be triggered unless
    // their own start/trigger bits are set.
    if (dma_ && state.cdRemaining == 0 && state.cdDataCb == 0) {
      dma_->checkAndRunTransfers();
    }

    // HLE libcd sector copy (no callback registered)
    // When the game uses libcd HLE `CdRead` (state.cdRemaining > 0,
    // cdDestPtr/cdWordCount stashed) but never registered
    // `CdReadCallback` (cdDataCb == 0), the drainPendingCallbacks pump
    // would dispatch nothing and ACK -- discarding the sector.  Copy it
    // ourselves to the destination CdRead stashed in psyq_state.
    if (state.cdRemaining > 0 && state.cdDataCb == 0 &&
        cdrom_->hasSectorReady() && state.cdDestPtr != 0) {
      const uint8_t *sector = cdrom_->getSectorBuffer();
      uint32_t sectorSz = cdrom_->getSectorSize();
      // Raw sector (2352 bytes): 12-sync + 4-header + 8-subheader + data.
      // sectorSize=2048 -> user data at +24; sectorSize=2340 -> +12.
      uint32_t dataOff = (sectorSz == 2048) ? 24u : 12u;
      uint32_t words = state.cdWordCount;
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t off = dataOff + i * 4;
        if (off + 3 >= 2352)
          break;
        uint32_t word = static_cast<uint32_t>(sector[off]) |
                        (static_cast<uint32_t>(sector[off + 1]) << 8) |
                        (static_cast<uint32_t>(sector[off + 2]) << 16) |
                        (static_cast<uint32_t>(sector[off + 3]) << 24);
        ctx_.mem->write32(state.cdDestPtr + i * 4, word);
      }
      state.cdDestPtr += words * 4;
      state.cdRemaining -= 1;
      hleCopiedSector = true;
      cdrom_->clearSectorReady();
      if (state.cdRemaining == 0) {
        cdrom_->stopReading();
        state.cdClockDemand.store(false, std::memory_order_release);
      }

      // No guest callback owns this INT1. The inline HLE copy above is the
      // entire handler, so release the controller now. A following sector may
      // be published only by the owned, bounded clock-credit pump.
      cdrom_->ackInterrupt(0x1F);
      cdrom_->clearWaitingForAck();
      cdIntPending_.store(0, std::memory_order_release);
      acknowledgedInline = true;
    }

    ps1::metrics::trace(
        "cdrom.data_ready_hle",
        {{"data_callback", state.cdDataCb},
         {"remaining_before", remainingBefore},
         {"remaining_after", state.cdRemaining},
         {"destination_before", destinationBefore},
         {"destination_after", state.cdDestPtr},
         {"sector_ready_after", cdrom_->hasSectorReady() ? 1 : 0},
         {"copied", hleCopiedSector ? 1 : 0},
         {"ack_inline", acknowledgedInline ? 1 : 0}});
  }

  // Update PsyQ CD state in psyq_state() singleton
  //
  // PsyQ interrupt handler mapping (as implemented by the game's own IRQ
  // chain):
  //
  //   INT1 (DataReady)  -> readyByte = 1          (syncByte untouched)
  //   INT2 (Complete)   -> syncByte  = 2          (readyByte untouched)
  //   INT3 (Acknowledge)-> syncByte  = 2 (mapped!) (readyByte untouched)
  //   INT4 (DataEnd)    -> readyByte = 4          (syncByte untouched)
  //   INT5 (DiskError)  -> syncByte  = 5, readyByte = 5
  //
  // CRITICAL: INT3 maps to syncByte=2 (Complete), NOT 3!  The game's
  // CdCommand function gates on syncByte==2 before issuing new commands.
  // If we wrote 3, subsequent commands would hang waiting for 2.
  //
  auto &psyqSync  = ps1::psyq::psyq_state().cdSyncByte;
  auto &psyqReady = ps1::psyq::psyq_state().cdReadyByte;
  switch (cdIntType) {
  case 1: psyqReady.store(1, std::memory_order_release); break;
  case 2: psyqSync.store(2, std::memory_order_release); break;
  case 3: psyqSync.store(2, std::memory_order_release); break; // mapped to Complete
  case 4: psyqReady.store(4, std::memory_order_release); break;
  case 5:
    psyqSync.store(5, std::memory_order_release);
    psyqReady.store(5, std::memory_order_release);
    break;
  default: break;
  }

  // BSS write-through (Phase 2.3 fallout): when the per-game TOML
  // declares `[bss_mirrors]` cd_sync_byte / cd_ready_byte, mirror the
  // atomic value into PS1 RAM so recompiled native MIPS that polls the
  // legacy BSS slot directly (Rayman PsyQ CdReset at 0x801CF1D8 / 0x801CF1DC)
  // observes the new state.  Single byte per slot -- same width as the
  // pre-2.3 BIOS handler used to write.
  if (cdSyncMirror_ != 0)
    ctx_.mem->write8(cdSyncMirror_,
                     psyqSync.load(std::memory_order_acquire));
  if (cdReadyMirror_ != 0)
    ctx_.mem->write8(cdReadyMirror_,
                     psyqReady.load(std::memory_order_acquire));
  BIOS_LOG("[CDROM-HLE] INT{} -> psyq_state.sync={} ready={}\n", cdIntType,
           psyqSync.load(std::memory_order_relaxed),
           psyqReady.load(std::memory_order_relaxed));

  // Dispatch CustomExceptionExit if registered
  //
  // CD interrupts fire SYNCHRONOUSLY from the game thread during CDROM
  // register writes.  If we longjmp here, we abort the game's call stack
  // before it enters its polling loop.
  //
  // On a real PS1, CD interrupts are ASYNCHRONOUS: the game sends a command,
  // enters a polling loop, and the interrupt arrives later.  The BIOS handler
  // processes it and then longjmps (if SetCustomExitFromException was used).
  //
  // We emulate this by DEFERRING the customExceptionExit dispatch: queue the
  // CD interrupt type, and fire the longjmp from drainPendingCallbacks() --
  // which the recompiler inserts into polling loops.  This gives the game time
  // to set up its polling state before the longjmp fires.
  //
  // Do NOT defer customExceptionExit for CD interrupts.
  //
  // CD commands in our implementation execute synchronously on the game
  // thread (writeRegister -> executeCommand -> pushResponse -> callback).
  // Secondary responses (INT2) also fire synchronously via fireSecondaryNow()
  // when the game acks the primary interrupt.  The game's PsyQ polling code
  // reads the hardware interrupt flag register directly and processes the
  // response through register I/O -- it does not need (and is disrupted by)
  // a deferred longjmp.
  //
  // The BSS state (cd_sync_byte, cd_ready_byte, cd_status_hw) is already
  // updated above, which is sufficient for the polling code to detect
  // command completion.

  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_1, spec);
  eventSystem_.triggerEvent(CDROM_EVENT_CLASS_2, spec);

}

void Bios::triggerVBlankEvent() {
  // The Phase-2 PsyQ VBlank counter lives in `psyq_state().vsyncCounter`
  // and is incremented by the host VBlank thread in `main_host.cpp`.
  // This entry-point delivers the HLE replacement for the BIOS/PsyQ VBlank
  // handler chain.  Do not also invoke the buffer installed by HookEntryInt
  // (B0:19): that buffer is the exit route used after the low-level chain,
  // not an additional VBlank callback.  The event and libetc callback work
  // below already replaces that chain.  Restoring the HookEntryInt jmp_buf
  // here would perform the same interrupt twice and attempt a non-local MIPS
  // jump from inside an ordinary recompiled C++ call.

  // VBlank IRQ event
  eventSystem_.triggerEvent(0xF0000001, 0x0001);

  // NOTE: Memory Card / CARD events (class 0xF4000001) are NOT triggered
  // here anymore.  On a real PS1, class 0xF4000001 is shared between the
  // memory-card SIO handler and the CDROM interrupt handler.  Firing them
  // every VBlank was causing false-positive TestEvent returns that confused
  // the PsyQ CdInit polling loop.  Card events should only be triggered
  // by actual SIO/CDROM hardware activity (i.e. triggerCdromEvent).

  // HLE PsyQ display swap callback (SysEnqIntRP replacement)
  //
  // On a real PS1, the SysEnqIntRP priority chain includes a PsyQ VBlank
  // handler that manages display double-buffering.  This handler calls
  // the game's swap callback with r4=4 to signal "swap done", which sets
  // a flag that the game's display-ready poll loop checks.
  //
  // Since SysEnqIntRP is stubbed (we don't build the handler chain), we queue
  // this callback for the game thread. `main_host` invokes this entry from
  // hle_VSync and drains the queue before that blocking VSync returns. Direct
  // dispatch from the host clock thread would instead race the shared
  // recomp_context.
  //
  uint32_t swapCb = ps1::psyq::psyq_state().gpuSwapCb;
  if (swapCb != 0 && !kernelThreads_.active()) {
    ps1::metrics::trace("callback.vsync_queue", {{"callback", swapCb}});
    eventSystem_.queueCallbackWithArg(swapCb,
                                      4); // a0 = 4 -> "swap done"
  }

  // HLE PsyQ DrawSync status (GPU ordering table completion)
  //
  // Since the runtime GPU is fully synchronous (all GP0 commands process
  // inline), every OT slot is "complete" by the time the next VBlank fires.
  // Stamp psyq_state().drawSync.status[] with 2 (= CdlComplete sentinel
  // used by PsyQ DrawSync polling) so any future consumer reading the
  // singleton sees consistent state.  Native PsyQ DrawSync polling against
  // PS1 RAM is short-circuited by the libgpu HLE (`hle_DrawSync` returns 0
  // unconditionally), so no BSS write is needed anymore -- the matcher
  // catches DrawSync in both Rayman and Crash.
  {
    auto &draw = ps1::psyq::psyq_state().drawSync;
    const uint32_t cnt = std::min<uint32_t>(
        draw.count, ps1::psyq::GpuDrawSync::kMaxSlots);
    for (uint32_t i = 0; i < cnt; ++i)
      draw.status[i] = 2;
  }
}

void Bios::deliverVBlankBoundary() {
  triggerVBlankEvent();
  drainPendingCallbacks();
}

void Bios::triggerRootCounterEvents(uint32_t timerIndex, uint32_t count) {
  if (timerIndex >= 3 || count == 0)
    return;

  // RCntCNT0..2 are 0xF2000000..2 and EvSpINT is 0x0002 in PsyQ. The
  // callback registered by Crash 3 at RCntCNT2 increments its high-resolution
  // clock once per timer target, about 20/21 times per PAL VBlank. Delivering
  // it once per rendered host frame was the source of the global speed error.
  const uint32_t eventClass = 0xF2000000u | timerIndex;
  const std::size_t intrSlot = static_cast<std::size_t>(timerIndex + 4u);
  const uint32_t directCallback =
      ps1::psyq::psyq_state().intrCallback[intrSlot];

  for (uint32_t i = 0; i < count; ++i) {
    eventSystem_.triggerEvent(eventClass, 0x0002);
    if (directCallback != 0)
      eventSystem_.queueCallback(directCallback);
  }
  ps1::metrics::count(fmt::format("timer.irq{}", timerIndex), count);
}

void Bios::drainPendingCallbacks() {
  // Some assembly renderers temporarily use $sp as a list/data cursor. The
  // recompiler marks that whole native call subtree with a depth guard. Keep
  // queued work pending until execution returns to a genuine guest stack;
  // dispatching a conventional callback here would push its frame over the
  // renderer's data.
  if (ctx_.callbackDrainBlockDepth != 0)
    return;

  // Diagnostic — env-gated via `PS1_DRAIN_TRACE=N` (period). Prints
  // every Nth entry + every Nth exit so we can tell whether the
  // function is being re-entered, returns cleanly, or hangs inside.
  // Used to debug the Crash Bandicoot iter-3+ stall (ISSUES.md #1).
  static const long period = []() {
    const char *e = std::getenv("PS1_DRAIN_TRACE");
    return (e && *e) ? std::strtol(e, nullptr, 0) : 0;
  }();
  static std::atomic<long> enter{0};
  static std::atomic<long> exit{0};
  if (period > 0) {
    long n = enter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % period == 0) {
      // Host return address identifies WHICH emitted busy-loop is spinning
      // (resolve with: addr2line -e ps1Runtime <addr>).
      fmt::print(stderr, "[DRAIN] enter#{} (exits={}) caller={}\n", n,
                 exit.load(std::memory_order_relaxed),
                 fmt::ptr(__builtin_return_address(0)));
    }
  }

  // This is the controller ownership boundary. The host thread only adds
  // clock credit; consuming it here means all CD state, VFS reads, buffer
  // publication and IRQ delivery stay on the game thread. serviceClock may
  // drain bounded matured credit, but only as sequential publish/copy/ACK
  // transactions; a callback/register-DMA consumer holds the INT1 latch and
  // stops the pump after one sector.
  if (cdrom_)
    cdrom_->serviceClock();

  // Drain controller IRQs before consuming the remaining callback surfaces.
  drainCdromEventQueue();

  // Convert synchronous hardware completions into the asynchronous callback
  // surface exposed by PsyQ DMACallback().  A generation counter preserves
  // multiple transfers completed between yield points, while queueing through
  // EventSystem avoids re-entering MIPS code from DMA::writeRegister().
  if (dma_) {
    auto &st = ps1::psyq::psyq_state();
    for (std::size_t ch = 0; ch < lastDmaCompletion_.size(); ++ch) {
      const uint32_t completed = dma_->completionGeneration(ch);
      const uint32_t pending = completed - lastDmaCompletion_[ch];
      // A normal DMA channel cannot complete thousands of transfers between
      // two cooperative yield points. A huge unsigned delta means an older
      // hardware generation was restored without its host delivery epoch.
      // Rebase instead of wrapping through 2^32 and filling the callback
      // queue until std::bad_alloc.
      constexpr uint32_t kMaxDmaCallbacksPerDrain = 4096;
      if (pending > kMaxDmaCallbacksPerDrain) {
        ps1::metrics::count("savestate.dma_epoch_rebased");
        lastDmaCompletion_[ch] = completed;
        continue;
      }
      for (uint32_t delivery = 0; delivery < pending; ++delivery) {
        ++lastDmaCompletion_[ch];
        if (uint32_t cb = st.dmaCallback[ch]; cb != 0)
          eventSystem_.queueCallback(cb);
      }
    }
  }

  // Tick the VBlank InterruptCallback once for every elapsed guest VBlank.
  // Root-counter slots 4-6 are delivered from triggerRootCounterEvents() at
  // their real hardware multiplicity instead of being forged here.
  {
    auto &st = ps1::psyq::psyq_state();
    uint32_t frame = st.vsyncCounter.load(std::memory_order_relaxed);
    const uint32_t pending = frame - lastIntrTickFrame_;
    if (pending && kernelThreads_.active()) {
      updatePadBuffers();
      psyq::psyq_pad_refresh_direct_buffers(&ctx_);
    }
    constexpr uint32_t kMaxVblankCallbacksPerDrain = 4096;
    if (pending > kMaxVblankCallbacksPerDrain) {
      ps1::metrics::count("savestate.vblank_epoch_rebased");
      lastIntrTickFrame_ = frame;
    } else {
      for (uint32_t delivery = 0; delivery < pending; ++delivery) {
        ++lastIntrTickFrame_;
        if (uint32_t cb = st.intrCallback[0]; cb != 0)
          eventSystem_.queueCallback(cb);
        // Kernel-threaded titles wait in their idle task instead of calling
        // blocking VSync. Deliver their registered VSync callback here too.
        if (kernelThreads_.active() && st.gpuSwapCb != 0)
          eventSystem_.queueCallbackWithArg(st.gpuSwapCb, 4);
      }
    }
  }

  if (!cardEvents_.empty()) {
    const auto event = cardEvents_.front(); cardEvents_.pop();
    eventSystem_.triggerEvent(event.first, event.second);
  }
  kernelThreads_.captureCurrent();
  eventSystem_.drainPendingCallbacks();
  kernelThreads_.scheduleSelected();

  // Deferred CD customExceptionExit dispatch
  //
  // When a CD interrupt fires synchronously (during a CDROM register write),
  // triggerCdromEvent queues it here instead of longjmping immediately.
  // Now that the game is at a safe yield point (polling loop), we fire the
  // longjmp so the game's exception handler can process the CD result.
  //
  // Check for a deferred CD exception -- fire at most one per
  // drainPendingCallbacks call.
  uint8_t cdExc = cdExceptionPending_.exchange(0, std::memory_order_acquire);
  if (cdExc != 0) {
    if (customExceptionCallback_) {
      BIOS_LOG("[CDROM-HLE] Firing deferred customExceptionExit for INT{}\n",
               cdExc);
      triggerCustomException();
      if (period > 0) {
        long n = exit.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % period == 0) {
          fmt::print(stderr, "[DRAIN] exit#{} via customException\n", n);
        }
      }
      return; // longjmp fired -- game handles it
    }
  }

  // Simulate SysEnqIntRP CDROM interrupt handler chain
  //
  // On a real PS1, when a CDROM interrupt fires the BIOS exception handler
  // at 0x80000080 dispatches through the SysEnqIntRP chain.  The PsyQ CD
  // interrupt handler reads HW registers, processes the response, and the
  // chain caller then dispatches dataCb/notifyCb.
  //
  // We dispatch the game's own dataCb/notifyCb here on the GAME thread.
  // The host master clock advances the CDROM. This drain only consumes IRQs;
  // feeding a sector's worth of cycles from the game thread collapses all
  // disc latency and makes async audio cues overlap.
  //
  // Maximum sectors to process per call -- prevents infinite loops if
  // something goes wrong, and limits time spent in one drain call.
  constexpr int MAX_SECTORS_PER_DRAIN = 32;

  auto &state = ps1::psyq::psyq_state();

  for (int pump = 0; pump < MAX_SECTORS_PER_DRAIN; ++pump) {
    // Drain the cross-thread queue at the top of each pump iteration: the
    // previous iteration's `cdrom_->tick(...)` may have fired the IRQ
    // callback inline, which now enqueues onto `cdEventQueue_` instead of
    // calling `triggerCdromEvent` directly.  Draining here populates
    // `cdIntPending_` so the loop body picks up streaming-read INT1s.
    drainCdromEventQueue();

    uint8_t intType = cdIntPending_.exchange(0, std::memory_order_acquire);
    if (intType == 0)
      break;

    // Save registers -- callbacks clobber temps.
    auto saved = static_cast<ps1::CPUContext>(ctx_);

    if (intType == 1 || intType == 4) {
      // INT1 (DataReady) or INT4 (DataEnd) -> call dataCb
      uint32_t dataCb = state.cdDataCb;
      int32_t remaining = static_cast<int32_t>(state.cdRemaining);
      static int hleDispatch = 0;
      hleDispatch++;
      if (detail::biosVerbose() && hleDispatch <= 20) {
        fmt::print(stderr, "[CD-CB] #{} INT{} dataCb=0x{:08X} remaining={}\n",
                   hleDispatch, intType, dataCb, remaining);
      }
      if (dataCb != 0) {
        ctx_.r4 = intType; // a0 = interrupt type
        ctx_.r5 = 0;       // a1 (unused by dataCb in practice)
        recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, dataCb);
      }
    } else if (intType == 2) {
      // INT2 (Complete) -> call notifyCb
      uint32_t notifyCb = state.cdNotifyCb;
      if (notifyCb != 0) {
        ctx_.r4 = intType;
        ctx_.r5 = 0;
        recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, notifyCb);
      }
    }
    // INT3 (Acknowledge) -- handled by CdControlF's own polling loop, skip.
    // INT5 (DiskError) -- not expected in normal operation.

    // ACK the interrupt so the CDROM can deliver the next sector.
    if (cdrom_ && (intType == 1 || intType == 4)) {
      cdrom_->ackInterrupt(0x1F);
      cdrom_->clearWaitingForAck();
    }

    // Restore registers
    static_cast<ps1::CPUContext &>(ctx_) = saved;

    // End the stream when the callback consumed the final sector. If another
    // sector already matured from host-clock cycles while this INT1 waited
    // for its ACK, the bounded stream pump may release one without adding
    // emulated time.
    if (cdrom_ && (intType == 1) && state.cdRemaining != 0) {
      int32_t remainAfterCb = static_cast<int32_t>(state.cdRemaining);
      if (remainAfterCb > 0) {
        cdrom_->serviceMaturedSector();
      } else {
        // Read complete -- stop the CDROM so it doesn't generate more INT1s.
        cdrom_->stopReading();
        cdIntPending_.store(0, std::memory_order_release);
        state.cdClockDemand.store(false, std::memory_order_release);
      }
    }
  }
  if (period > 0) {
    long n = exit.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % period == 0) {
      fmt::print(stderr, "[DRAIN] exit#{}\n", n);
    }
  }
}

void Bios::stub_printf() {
  // Read format string from emulated RAM
  uint32_t fmtAddr = ctx_.r[A0];
  // Forward the immutable call boundary before formatting. Game profiles are
  // responsible for exact executable/address validation and remain inert for
  // every other title. Crash 3 uses the BIOS printf syscall (rather than the
  // similarly named PsyQ HLE routine) for its loader identity marker.
  ps1::games::crash3::observeDebugPrintf(ctx_, fmtAddr);
  std::string fmtStr = readString(*ctx_.mem, fmtAddr);

  // Process basic format specifiers using arguments from $a1, $a2, $a3, stack
  std::string output;
  uint32_t argRegs[] = {ctx_.r[A1], ctx_.r[A2], ctx_.r[A3]};
  int argIdx = 0;
  uint32_t stackArgBase =
      ctx_.r[SP] + 16; // First stack arg after 4 register shadow area

  auto getArg = [&]() -> uint32_t {
    if (argIdx < 3) {
      return argRegs[argIdx++];
    }
    uint32_t val = ctx_.mem->read32(stackArgBase + (argIdx - 3) * 4);
    argIdx++;
    return val;
  };

  for (size_t i = 0; i < fmtStr.size(); i++) {
    if (fmtStr[i] != '%') {
      output += fmtStr[i];
      continue;
    }
    i++;
    if (i >= fmtStr.size())
      break;

    // Skip flags/width/precision modifiers
    while (i < fmtStr.size() &&
           (fmtStr[i] == '-' || fmtStr[i] == '+' || fmtStr[i] == ' ' ||
            fmtStr[i] == '0' || fmtStr[i] == '#'))
      i++;
    while (i < fmtStr.size() && fmtStr[i] >= '0' && fmtStr[i] <= '9')
      i++;
    if (i < fmtStr.size() && fmtStr[i] == '.')
      i++;
    while (i < fmtStr.size() && fmtStr[i] >= '0' && fmtStr[i] <= '9')
      i++;
    // Skip length modifiers (l, h)
    while (i < fmtStr.size() && (fmtStr[i] == 'l' || fmtStr[i] == 'h'))
      i++;

    if (i >= fmtStr.size())
      break;

    switch (fmtStr[i]) {
    case 'd':
    case 'i':
      output += fmt::format("{}", static_cast<int32_t>(getArg()));
      break;
    case 'u':
      output += fmt::format("{}", getArg());
      break;
    case 'x':
      output += fmt::format("{:x}", getArg());
      break;
    case 'X':
      output += fmt::format("{:X}", getArg());
      break;
    case 'p':
      output += fmt::format("0x{:08x}", getArg());
      break;
    case 'c':
      output += static_cast<char>(getArg() & 0xFF);
      break;
    case 's': {
      uint32_t strAddr = getArg();
      if (strAddr != 0) {
        output += readString(*ctx_.mem, strAddr);
      } else {
        output += "(null)";
      }
      break;
    }
    case '%':
      output += '%';
      break;
    default:
      output += '%';
      output += fmtStr[i];
      break;
    }
  }

  fmt::print("{}", output);
}

// Pad Buffer Update (called each VBlank)

void Bios::updatePadBuffers() {
  if (!padActive_ || !input_)
    return;

  // PS1 BIOS pad buffer format (34 bytes per port):
  //   byte 0: status (0x00 = OK, 0xFF = no controller)
  //   byte 1: pad type high nibble + half-word count (e.g., 0x41 = digital)
  //   byte 2: button state low byte  (active-low)
  //   byte 3: button state high byte (active-low)
  //   bytes 4-7: analog data (if analog controller)

  auto writePadBuffer = [&](uint32_t bufAddr, uint32_t bufSize, int port) {
    if (bufAddr == 0 || bufSize < 4)
      return;

    input::PadType padType = input_->getPadType(port);
    const auto inputState = input_->snapshot();
    const auto &pad = inputState.ports[port];
    uint16_t buttons = input_->latchedButtonState(port);

    if (padType == input::PadType::None) {
      // No controller connected
      ctx_.mem->write8(bufAddr + 0, 0xFF);
      ctx_.mem->write8(bufAddr + 1, 0xFF);
      return;
    }

    ctx_.mem->write8(bufAddr + 0, 0x00);                          // Status OK
    ctx_.mem->write8(bufAddr + 1, static_cast<uint8_t>(padType)); // Type ID
    // PsyQ exposes the serial response as a native little-endian button word:
    // Select/Start/D-pad first, then shoulders/face buttons.
    ctx_.mem->write8(bufAddr + 2, buttons & 0xFF);
    ctx_.mem->write8(bufAddr + 3, (buttons >> 8) & 0xFF);
    if (bufSize >= 8 &&
        (padType == input::PadType::Analog ||
         padType == input::PadType::DualShock)) {
      ctx_.mem->write8(bufAddr + 4, pad.analogRX);
      ctx_.mem->write8(bufAddr + 5, pad.analogRY);
      ctx_.mem->write8(bufAddr + 6, pad.analogLX);
      ctx_.mem->write8(bufAddr + 7, pad.analogLY);
    }
  };

  writePadBuffer(padBuf1Addr_, padBuf1Size_, 0);
  writePadBuffer(padBuf2Addr_, padBuf2Size_, 1);
}

void Bios::triggerCustomException() {
  if (customExceptionCallback_)
    customExceptionCallback_();
}

void hle_longjmp_emulator(recomp_context &ctx, Memory &mem, uint32_t buf) {
  // PsyQ jmp_buf layout (matches BIOS exception exit):
  //   +0   RA   +4   SP   +8   FP
  //   +12  S0   ... +40  S7
  //   +44  GP
  uint32_t jmpRA = mem.read32(buf + 0);
  ctx.r[RA] = jmpRA;
  ctx.r[SP] = mem.read32(buf + 4);
  ctx.r[FP] = mem.read32(buf + 8);
  for (int i = 0; i < 8; i++) {
    ctx.r[S0 + i] = mem.read32(buf + 12 + (i * 4));
  }
  ctx.r[GP] = mem.read32(buf + 44);

  // Cause = ext-interrupt (matches what the BIOS exception path leaves behind)
  ctx.cop0[COP0_CAUSE] = 0x400;
  ctx.pc = jmpRA;
  ctx.r[V0] = 1; // longjmp return convention

  // Pop SR exception stack: bits 5:2 -> 3:0
  uint32_t sr = ctx.cop0[COP0_SR];
  ctx.cop0[COP0_SR] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}

} // namespace ps1::bios
