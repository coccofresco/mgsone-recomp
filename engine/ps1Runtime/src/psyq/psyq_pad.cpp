#include "runtime/psyq/psyq_pad.h"
#include "runtime/bios/bios.h"
#include "runtime/input/input.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <mutex>

namespace ps1::psyq {

namespace {

// Per-port direct-mode buffer addresses set by PadInitDirect.  Either may be
// 0, in which case PadRead skips refreshing that side -- the "no buffer"
// case is normal when only one slot is wired or the game uses PadRead's
// return value exclusively.
struct PadModuleState {
  uint32_t buf1Addr = 0; // port 0 (slot 1)
  uint32_t buf2Addr = 0; // port 1 (slot 2)
  bool active = false;   // PadInit / PadStartCom raise; PadStopCom lowers
};

PadModuleState g_state;
std::mutex g_stateMutex;

bool inputTraceEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("PS1_INPUT_TRACE");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

input::InputController *inputOf(recomp_context *ctx) {
  return ctx && ctx->bios ? ctx->bios->inputController() : nullptr;
}

// Mirror of Bios::updatePadBuffers's byte layout, factored so PadInitDirect
// and PadRead can refresh a single 34-byte buffer at a time. Real PsyQ stores
// the two serial button bytes as a native little-endian uint16_t: low byte
// (Select/Start/D-pad), then high byte (shoulders/face buttons). Analog devices
// append right X/Y and left X/Y at bytes 4..7.
void writePadBuffer(recomp_context *ctx, input::InputController *input,
                    uint32_t bufAddr, int port) {
  if (bufAddr == 0)
    return;

  if (!input) {
    // No backend: synthesise a "controller absent" header so the game's
    // type-check (byte 1 == 0xFF) trips and it skips the slot cleanly.
    ctx->mem->write8(bufAddr + 0, 0xFF);
    ctx->mem->write8(bufAddr + 1, 0xFF);
    return;
  }

  const input::InputController::Snapshot state = input->snapshot();
  const auto &pad = state.ports[port];
  input::PadType padType = pad.padType;
  if (padType == input::PadType::None) {
    ctx->mem->write8(bufAddr + 0, 0xFF);
    ctx->mem->write8(bufAddr + 1, 0xFF);
    return;
  }

  uint16_t buttons = input->latchedButtonState(port);
  ctx->mem->write8(bufAddr + 0, 0x00);                          // status OK
  ctx->mem->write8(bufAddr + 1, static_cast<uint8_t>(padType)); // type ID
  ctx->mem->write8(bufAddr + 2, static_cast<uint8_t>(buttons & 0xFF));
  ctx->mem->write8(bufAddr + 3,
                   static_cast<uint8_t>((buttons >> 8) & 0xFF));
  if (padType == input::PadType::Analog ||
      padType == input::PadType::DualShock) {
    ctx->mem->write8(bufAddr + 4, pad.analogRX);
    ctx->mem->write8(bufAddr + 5, pad.analogRY);
    ctx->mem->write8(bufAddr + 6, pad.analogLX);
    ctx->mem->write8(bufAddr + 7, pad.analogLY);
  }
  if (inputTraceEnabled()) {
    static uint16_t lastButtons[2] = {0, 0};
    if (lastButtons[port] != buttons) {
      lastButtons[port] = buttons;
      fmt::print("[InputTrace] PsyQ port {} buffer=0x{:08X} type=0x{:02X} "
                 "bytes={:02X} {:02X}, state=0x{:04X}\n",
                 port + 1, bufAddr, static_cast<uint8_t>(padType),
                 buttons & 0xFF, (buttons >> 8) & 0xFF, buttons);
    }
  }
}

} // namespace

//  PadInit(mode) -- `mode` is documented as reserved (caller passes 0).
//  Returns 0 in $v0.  We mark the module active so subsequent PadRead /
//  direct-buffer refreshes do real work, mirroring how libetc flips its
//  internal `PadInit` flag.
void hle_libetc_PadInit(recomp_context *ctx) {
  std::lock_guard lock(g_stateMutex);
  g_state.active = true;
  ctx->r[V0] = 0;
}

//  PadStartCom() -- re-enable pad SIO traffic.  Real libetc unmasks IRQ7
//  and resumes the per-VBlank pad poll; in our model SIO is always live
//  on the host side, so this is bookkeeping only.
void hle_libetc_PadStartCom(recomp_context *ctx) {
  std::lock_guard lock(g_stateMutex);
  g_state.active = true;
  ctx->r[V0] = 0;
}

//  PadStopCom() -- temporarily halt pad polling so MemCard ops own SIO.
//  PadRead/PadGetState below still answer truthfully -- disabling them
//  would cause games that poll input during a save dialog to misread.
void hle_libetc_PadStopCom(recomp_context *ctx) {
  std::lock_guard lock(g_stateMutex);
  g_state.active = false;
  ctx->r[V0] = 0;
}

//  PadInitDirect(buf1, buf2) -- register the user-supplied 34-byte status
//  buffers that the pad subsystem refreshes each VBlank.  We don't have a
//  hook on the VBlank thread for libetc, so we refresh the buffers here
//  (initial seed) and again on every PadRead -- close enough since games
//  always call PadRead immediately after VSync.
void hle_libetc_PadInitDirect(recomp_context *ctx) {
  std::lock_guard lock(g_stateMutex);
  g_state.buf1Addr = ctx->r[A0];
  g_state.buf2Addr = ctx->r[A1];
  g_state.active   = true;

  auto *input = inputOf(ctx);
  writePadBuffer(ctx, input, g_state.buf1Addr, 0);
  writePadBuffer(ctx, input, g_state.buf2Addr, 1);

  ctx->r[V0] = 0;
}

//  PadGetState(port) -- boil the 6-state PsyQ link machine down to the two
//  outcomes our backend can actually distinguish.  Games overwhelmingly
//  branch on `state == PAD_STATE_STABLE`, so returning that whenever the
//  port reports a known PadType is sufficient.  Unknown ports report
//  Discovery so the game's "wait for stable" loops still bail out cleanly.
void hle_libetc_PadGetState(recomp_context *ctx) {
  const int port = ctx->r[A0] == 0x10 ? 1 : (ctx->r[A0] == 0 ? 0 : -1);
  auto *input = inputOf(ctx);
  if (!input || port < 0 || port > 1) {
    ctx->r[V0] = PAD_STATE_DISCOVERY;
    return;
  }
  ctx->r[V0] = (input->getPadType(port) == input::PadType::None)
                   ? PAD_STATE_DISCOVERY
                   : PAD_STATE_STABLE;
}

void hle_libpad_PadInfoMode(recomp_context *ctx) {
  const int port = ctx->r[A0] == 0x10 ? 1 : (ctx->r[A0] == 0 ? 0 : -1);
  auto *input = inputOf(ctx);
  if (!input || port < 0 || port > 1 ||
      input->getPadType(port) == input::PadType::None) {
    ctx->r[V0] = 0;
    return;
  }

  // InfoModeCurID (term 1) is the query used by SDK clients before selecting
  // analog mode. The protocol type's high nibble is the public mode ID.
  if (ctx->r[A1] == 1) {
    ctx->r[V0] = static_cast<uint8_t>(input->getPadType(port)) >> 4;
    return;
  }
  ctx->r[V0] = 0;
}

//  PadRead(n) -- packed 32-bit pad word: port 1 in the low half, port 2 in
//  the high half.  Each half is the PsyQ active-low button mask (bit clear
//  = pressed) which is exactly what the VBlank-latched input snapshot
//  returns; no extra inversion needed.
//
//  Whether the module is "active" doesn't gate reading on real libetc --
//  PadRead just samples the most recent VBlank snapshot -- so we ignore
//  `g_state.active` here and always answer.
void hle_libetc_PadRead(recomp_context *ctx) {
  std::lock_guard lock(g_stateMutex);
  auto *input = inputOf(ctx);

  uint16_t port1 = input ? input->latchedButtonState(0) : 0xFFFF;
  uint16_t port2 = input ? input->latchedButtonState(1) : 0xFFFF;

  // Refresh direct-mode buffers as a side-effect -- see PadInitDirect.
  writePadBuffer(ctx, input, g_state.buf1Addr, 0);
  writePadBuffer(ctx, input, g_state.buf2Addr, 1);

  ctx->r[V0] = (static_cast<uint32_t>(port2) << 16) |
               static_cast<uint32_t>(port1);
}

// Test-only state hooks

void psyq_pad_reset_for_tests() {
  std::lock_guard lock(g_stateMutex);
  g_state = PadModuleState{};
}

PadModuleSnapshot psyq_pad_snapshot() {
  std::lock_guard lock(g_stateMutex);
  return {g_state.buf1Addr, g_state.buf2Addr, g_state.active};
}

void psyq_pad_restore(const PadModuleSnapshot &state) {
  std::lock_guard lock(g_stateMutex);
  g_state = {state.buf1Addr, state.buf2Addr, state.active};
}

void psyq_pad_refresh_direct_buffers(recomp_context *ctx) {
  std::lock_guard lock(g_stateMutex);
  if (!g_state.active)
    return;
  auto *input = inputOf(ctx);
  writePadBuffer(ctx, input, g_state.buf1Addr, 0);
  writePadBuffer(ctx, input, g_state.buf2Addr, 1);
}

// Registry wiring

void psyq_register_libetc_pad() {
  psyq_register("libetc_PadInit",       &hle_libetc_PadInit);
  psyq_register("libetc_PadStartCom",   &hle_libetc_PadStartCom);
  psyq_register("libetc_PadStopCom",    &hle_libetc_PadStopCom);
  psyq_register("libetc_PadInitDirect", &hle_libetc_PadInitDirect);
  psyq_register("libetc_PadGetState",   &hle_libetc_PadGetState);
  psyq_register("libetc_PadRead",       &hle_libetc_PadRead);
  // libpad and libetc expose the same direct-buffer protocol. Recompilation
  // configs may identify either public wrappers or the SDK implementation
  // entry, but both terminate in this platform-owned state machine.
  psyq_register("libpad_PadInitDirect", &hle_libetc_PadInitDirect);
  psyq_register("libpad_PadInitDirectImpl", &hle_libetc_PadInitDirect);
  psyq_register("libpad_PadGetState",   &hle_libetc_PadGetState);
  psyq_register("libpad_PadInfoMode",   &hle_libpad_PadInfoMode);
  psyq_register("libpad_PadStartCom", &hle_libetc_PadStartCom);
  psyq_register("libpad_PadStopCom", &hle_libetc_PadStopCom);
  // The current host exposes no actuator protocol; report zero actuators
  // rather than letting native SIO access uninitialized SDK-private state.
  psyq_register("libpad_PadInfoAct", +[](recomp_context* c) { c->r[V0] = 0; });
  psyq_register("libpad_PadSetAct", +[](recomp_context* c) { c->r[V0] = 0; });
  psyq_register("libpad_PadSetActAlign", +[](recomp_context* c) { c->r[V0] = 1; });
  psyq_register("libpad_PadSetMainMode", +[](recomp_context* c) { c->r[V0] = 0; });
}

} // namespace ps1::psyq
