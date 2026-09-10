#pragma once
/**
 * @file psyq_pad.h
 * @brief HLE for the libetc Pad/Controller routines (Group 1.E).
 *
 * Games that use the post-PsyQ-3.x `libetc` pad API (`PadInit`, `PadRead`,
 * `PadStartCom`, `PadStopCom`, `PadInitDirect`, `PadGetState`) bypass the
 * BIOS B0:0x12/0x13/0x14 syscalls already wrapped by `psyq_libapi.cpp` and
 * talk to SIO0 directly.  When those entry points are detected by signature
 * matching but left unregistered the runtime aborts in `psyq_dispatch`; this
 * module satisfies them by forwarding to the SDL-backed
 * `input::InputController` exposed via `Bios::inputController()`.
 *
 * The active-low button bitmask used by `InputController::buttonState()`
 * already matches the wire format the original PsyQ pad subsystem returns
 * (bit clear = pressed) so no additional inversion is needed at the HLE
 * boundary -- `PadRead` packs `port2:port1` into the 32-bit return word as
 * real `libetc` does.
 *
 * `PadInitDirect(buf1, buf2)` records the per-port 34-byte status-buffer
 * pointers; the buffers are refreshed lazily on every `PadRead` call (close
 * enough to the real "every VBlank" cadence since games poll once per frame).
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// PadInit(mode) -- initialise the pad subsystem.  `mode` is reserved and
/// ignored by real libetc; we just enable polling and return 0 in `$v0`.
void hle_libetc_PadInit(recomp_context *ctx);

/// PadStartCom() -- re-acquire SIO for pad communication after MemCard I/O.
/// NOP in our runtime (SIO multiplexing is implicit), returns 0.
void hle_libetc_PadStartCom(recomp_context *ctx);

/// PadStopCom() -- release SIO so MemCard transfers can run undisturbed.
/// NOP, returns 0.
void hle_libetc_PadStopCom(recomp_context *ctx);

/// PadInitDirect(buf1, buf2) -- register per-port 34-byte pad-status buffers.
/// `buf1` services port 0, `buf2` services port 1.  Either may be NULL.
/// Returns 0 in `$v0`.
void hle_libetc_PadInitDirect(recomp_context *ctx);

/// PadGetState(port) -- return the link-state code.  We collapse the 6-state
/// PsyQ machine to two values: `PadStateStable` (4) when a pad is attached,
/// `PadStateDiscovery` (0) otherwise.  Games typically just compare to 4.
void hle_libetc_PadGetState(recomp_context *ctx);

/// PadRead(n) -- return `(port2 << 16) | port1` of active-low button words.
/// The `n` arg is reserved and ignored.  When no input controller is
/// attached returns `0xFFFFFFFF` (no buttons pressed on either port).
void hle_libetc_PadRead(recomp_context *ctx);

/// PsyQ link-state values returned by `PadGetState`.  Matches `<libetc.h>`.
enum PadState : uint32_t {
  PAD_STATE_DISCONNECTED = 0,
  PAD_STATE_FIND_PAD = 1,
  PAD_STATE_FIND_CTP1 = 2,
  PAD_STATE_FIND_CTP2 = 3,
  PAD_STATE_REQUEST_INFO = 4,
  PAD_STATE_EXEC_COMMAND = 5,
  PAD_STATE_STABLE = 6,
  PAD_STATE_ERROR = 7,
};

constexpr PadState PAD_STATE_DISCOVERY = PAD_STATE_DISCONNECTED;

struct PadModuleSnapshot {
  uint32_t buf1Addr = 0;
  uint32_t buf2Addr = 0;
  bool active = false;
};

PadModuleSnapshot psyq_pad_snapshot();
void psyq_pad_restore(const PadModuleSnapshot &state);

/// Reset module state (active flag + buffer pointers).  Test-only hook.
void psyq_pad_reset_for_tests();

/// Refresh the PadInitDirect buffers from the current input state.  Called
/// internally by `PadRead`; exposed for tests that want to assert the buffer
/// contents without going through the dispatch path.
void psyq_pad_refresh_direct_buffers(recomp_context *ctx);

/// PadInfoMode(port, term, offs). The current-ID query used by libpad clients
/// returns the connected controller's protocol ID nibble (4 digital, 7
/// analog/DualShock).
void hle_libpad_PadInfoMode(recomp_context *ctx);

/// Register every Pad* HLE in this header into the runtime registry.
void psyq_register_libetc_pad();

} // namespace ps1::psyq
