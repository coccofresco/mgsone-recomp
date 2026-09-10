#pragma once
/**
 * @file psyq_state.h
 * @brief Centralised C++ home for PsyQ runtime state (Phase 2, Session 2.1).
 *
 * Until Phase 2, every piece of "PsyQ-side" state -- VBlank counter, CD sync /
 * ready bytes, sector-read bookkeeping, async callbacks, GPU swap callback,
 * DrawSync OT tracking -- lived in PS1 RAM at game-specific BSS addresses
 * declared in `[psyq_addresses]` of each game TOML.  That model needs the
 * recompiled binary to actually initialise those addresses, which broke on
 * Crash Bandicoot (`cd_data_cb` is never written from MIPS, so `bios.cpp`
 * INT1 never finds a callback to invoke and `dma2_count` stays at 2).
 *
 * `PsyqState` is the C++ replacement: a process-wide singleton holding all
 * the values the HLE layer used to poke into PS1 RAM.  It is created lazily
 * by `psyq_state()` and lives until program exit.
 *
 * **Phase 2 status (post-2.4):** every former BSS slot now lives here --
 * VSync counter (2.2), CD sync/ready bytes (2.3), CD sector bookkeeping +
 * callbacks + GPU swap callback + DrawSync OT slots (2.4).  The
 * `[psyq_addresses]` block has been removed from `rayman.toml`/`crash.toml`
 * and the `Bios::PsyqAddresses` struct deleted.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ps1::psyq {

/**
 * @brief OT (ordering-table) status slots formerly tracked at
 *        `gpu_drawsync_base` / `gpu_drawsync_index_addr` in BSS.
 *
 * Each entry mirrors one double-buffered OT and holds the per-slot
 * "drawing complete" flag that PsyQ's DrawSync polls.  `count` equals the
 * number of slots in use (2 for typical double-buffering); `index` is the
 * current write slot.  Slots beyond `count` are unused.
 *
 * The struct is intentionally shallow -- Phase 2 only stores what the legacy
 * BSS layout stored.  `Bios::triggerVBlankEvent` stamps every active slot
 * with status == 2 (PsyQ "drawing complete") since the runtime GPU is
 * fully synchronous; future async-render work can refine to per-slot
 * atomics if needed.
 */
struct GpuDrawSync {
  /// Maximum number of OT slots we track.  Two is typical (front/back
  /// buffer); a few engines use 4.  Eight is generous head-room.
  static constexpr std::size_t kMaxSlots = 8;

  uint32_t status[kMaxSlots] = {}; ///< Per-slot completion words.
  uint32_t index = 0;              ///< Currently active slot.
  uint32_t count = 2;              ///< Number of valid slots in `status`.
};

/**
 * @brief Centralised PsyQ runtime state.
 *
 * Singleton -- instantiated lazily on first call to `psyq_state()` and
 * shared by HLE handlers across `psyq_*.cpp` and the VBlank thread in
 * `main_host.cpp`.  Atomic fields are touched from multiple threads
 * (game thread + VBlank thread + CDROM IRQ thread); plain fields are
 * written only from the game thread.
 */
class PsyqState {
public:
  // VBlank
  /// Tick counter incremented by `main_host.cpp`'s VBlank thread at 60 Hz.
  /// `hle_VSync` reads this to compute the wait target.  Replaces the
  /// per-game `vsync_counter` BSS address.
  std::atomic<uint32_t> vsyncCounter{0};

  /// Absolute VBlank count at which the last blocking VSync(0/n) completed.
  /// PsyQ uses this baseline for mode 1 (elapsed-time query) and n > 1 waits.
  /// Game-thread only; VSync(-n) reads the atomic absolute counter directly.
  uint32_t lastVSyncCounter = 0;

  /// Number of host VBlanks awaiting game-thread delivery. A counter is used
  /// instead of a boolean because VBlank callbacks (notably libsnd's sequence
  /// tick) are edge-driven: coalescing two missed VBlanks into one changes
  /// tempo and can leave voices overlapping.
  std::atomic<uint32_t> pendingVblanks{0};

  /// Once a blocking VSync has been observed, host wall-clock pulses may
  /// advance guest time only while this target is outstanding. Recompiled
  /// code has no cycle accounting yet, so allowing a host pulse between the
  /// return from one VSync and the next call makes renderer cost guest-visible.
  std::atomic<bool> blockingVSyncPacing{false};
  std::atomic<uint32_t> blockingVSyncTarget{0};

  /// Blocking hardware waits other than VSync (currently libcd data waits)
  /// temporarily request autonomous guest-clock progress. A count supports
  /// nested reusable HLE waits without a game-specific scheduler switch.
  std::atomic<uint32_t> autonomousClockWaiters{0};

  /// An asynchronous libcd transfer is itself an exact hardware-clock demand:
  /// sectors cannot mature while a frame-driven title is between VSync calls.
  /// This mirrors cdRemaining in an atomic form safe for the host scheduler.
  std::atomic<bool> cdClockDemand{false};

  // CDROM sync flags
  /// Set by `Bios::triggerCdromEvent(CdlComplete)` to signal CdSync
  /// completion.  Replaces `cd_sync_byte` BSS slot.
  std::atomic<uint8_t> cdSyncByte{0};

  /// Set by `Bios::triggerCdromEvent(CdlDataReady|CdlDataEnd)` to signal
  /// CdReady completion.  Replaces `cd_ready_byte` BSS slot.
  std::atomic<uint8_t> cdReadyByte{0};

  // CDROM read bookkeeping
  // Touched only from the game thread (libcd HLE entries) and the
  // CDROM IRQ handler running on the same thread (synchronous host
  // model).  Plain integers -- no atomic semantics required yet.
  uint32_t cdRemaining = 0; ///< Sectors still pending in the active read.
  uint32_t cdDestPtr   = 0; ///< PS1 RAM addr where the next sector lands.
  uint32_t cdWordCount = 0; ///< Words per sector (512 raw, 585 with subhdr).

  // CDROM async callbacks
  /// Game-side callback registered by `CdReadCallback` / `CdReadyCallback`.
  /// `bios.cpp` INT1/INT4 dispatch will invoke this once Phase 2.4 lands.
  uint32_t cdDataCb   = 0;
  /// Notify-side callback (`CdDataCallback` legacy alias).
  uint32_t cdNotifyCb = 0;

  // GPU async callback
  /// Display-swap callback registered by `VSyncCallback`.  Replaces
  /// `gpu_swap_cb` BSS slot.
  uint32_t gpuSwapCb = 0;

  // Interrupt / callback management (libetc + libapi).  PSY-Q's libetc
  // routes everything through an indirection struct (psyz/decomp/src/libetc/
  // intr.c -- `struct intr* D_800B7080`); we collapse the same surface to
  // these slots since the runtime models interrupts cooperatively, not via
  // real CPU IRQs.
  /// Mirror of hardware I_MASK (0x1F801074).  Real silicon decides which
  /// IRQs the CPU sees; we keep a software copy so PSY-Q SetIntrMask/
  /// GetIntrMask round-trip the same value.  No effect on dispatch -- host
  /// callbacks are drained at cooperative yield points regardless.
  uint16_t intrMask = 0;
  /// Per-IRQ-line user callback registered via `InterruptCallback(n, fn)`.
  /// Slot 0..6 follow the PSY-Q convention (VBlank, GPU, CDROM, DMA, RTC0,
  /// RTC1, RTC2); slot 7 reserved.  The cooperative dispatcher currently
  /// fires VBlank (0) and root-counter (4-6) callbacks once per host VBlank.
  static constexpr std::size_t kIntrSlots = 8;
  uint32_t intrCallback[kIntrSlots] = {};
  /// Per-channel DMA callback registered via `DMACallback(n, fn)`.  Same
  /// shape and reasoning as `intrCallback`.
  uint32_t dmaCallback[kIntrSlots] = {};
  /// Master enable toggled by `StopCallback` / `RestartCallback`.  Read by
  /// `CheckCallback` only; does not gate dispatch.
  bool callbacksEnabled = true;

  // GPU DrawSync OT slots
  /// Replaces the BSS triplet `gpu_drawsync_base` /
  /// `gpu_drawsync_index_addr` / `gpu_drawsync_count`.
  GpuDrawSync drawSync;

  // Test helpers
  /// Reset every field to its default-constructed value.  Tests call
  /// this in `SetUp()` to isolate cases sharing the singleton.
  void reset();

  PsyqState(const PsyqState &) = delete;
  PsyqState &operator=(const PsyqState &) = delete;

private:
  PsyqState() = default;
  friend PsyqState &psyq_state();
};

/// Access the process-wide PsyqState singleton.
PsyqState &psyq_state();

} // namespace ps1::psyq
