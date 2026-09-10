#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <cstdint>
#include <fmt/format.h>
#include <unordered_set>

// Declared by the generated recompiled_out.cpp (and the stub build).
void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace ps1::psyq {

namespace {

// PsyQ primitive layout: byte 3 = `len` (words after the tag word), byte 7 =
// `code` (GP0 command nibble).  setlen/setcode macros from <libgpu.h>.
inline void writeLen(recomp_context *ctx, uint32_t p, uint8_t len) {
  ctx->mem->write8(p + 3, len);
}
inline void writeCode(recomp_context *ctx, uint32_t p, uint8_t code) {
  ctx->mem->write8(p + 7, code);
}

// Reads the int16 RECT { x, y, w, h } at the given PS1 RAM pointer.
struct PsyqRect {
  int16_t x, y, w, h;
};

inline PsyqRect readRect(recomp_context *ctx, uint32_t p) {
  PsyqRect r;
  r.x = static_cast<int16_t>(ctx->mem->read16(p + 0));
  r.y = static_cast<int16_t>(ctx->mem->read16(p + 2));
  r.w = static_cast<int16_t>(ctx->mem->read16(p + 4));
  r.h = static_cast<int16_t>(ctx->mem->read16(p + 6));
  return r;
}

inline void writeGP0(uint32_t w) {
  const auto &cfg = getConfig();
  if (cfg.writeGP0)
    cfg.writeGP0(w);
}

inline void writeGP1(uint32_t w) {
  const auto &cfg = getConfig();
  if (cfg.writeGP1)
    cfg.writeGP1(w);
}

class GpuBatchGuard {
public:
  GpuBatchGuard() {
    const auto &cfg = getConfig();
    active_ = static_cast<bool>(cfg.beginGP0Batch) &&
              static_cast<bool>(cfg.endGP0Batch);
    if (active_)
      cfg.beginGP0Batch();
  }
  ~GpuBatchGuard() {
    if (active_)
      getConfig().endGP0Batch();
  }

private:
  bool active_ = false;
};

// Module-level state shared across libgpu HLE calls.
// videoMode: 0 = NTSC (default), 1 = PAL.
int g_videoMode = 0;

// DrawSyncCallback PS1-side function pointer. Recorded so we can return the
// previous one; not actually invoked (runtime GPU is synchronous).
// VSyncCallback's swap routine lives in psyq_state().gpuSwapCb so the BIOS
// VBlank thread can dispatch it without round-tripping through this module.
uint32_t g_drawSyncCallback = 0;

// "Once-per-name" warning helper for stubbed libgs entries.
void warnOnceFor(const char *name) {
  static std::unordered_set<std::string> seen;
  if (seen.insert(name).second)
    fmt::print(stderr,
               "[PSYQ] {} stubbed (NOP) -- no-op for current HLE coverage\n",
               name);
}

} // namespace

// GetClut(x, y) -> packed CLUT id. y occupies the high 9 bits, x the low 6
// (x is always a multiple of 16, hence the >>4).
//
// Audited 2026-08-06: the psyz decomp's GetClut (decomp/src/libgpu/prim.c:9,
// `u_short GetClut(int x, int y) { return getClut(x, y); }`) only forwards
// to an SDK-internal `getClut` macro whose body is not present in the
// decomp tree (proprietary SDK header, not admissible). The packed layout
// is independently documented as hardware fact -- not an SDK internal -- by
// psx-spx (docs/graphicsprocessingunitgpu.md:366-374, "Clut Attribute"),
// which for the v0 (1 MB VRAM) GPU this project targets gives:
//   bits 0-5   X coordinate X/16 (16-halfword steps)
//   bits 6-14  Y coordinate 0-511 (9 bits)
//   bit 15     unused
// Verdict: already correct -- (y & 0x1FF) << 6 | (x >> 4) & 0x3F matches
// exactly. No change made.
void hle_libgpu_GetClut(recomp_context *ctx) {
  uint32_t x = ctx->r[A0];
  uint32_t y = ctx->r[A1];
  ctx->r[V0] = ((y & 0x1FFu) << 6) | ((x >> 4) & 0x3Fu);
}

// SetShadeTex(p, tge): toggle the raw-texture bit of the primitive's code.
//   tge != 0 -> raw texture (no shading)
//   tge == 0 -> modulated (shaded) texture
void hle_libgpu_SetShadeTex(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  bool tge = ctx->r[A1] != 0;
  uint8_t code = ctx->mem->read8(p + 7);
  code = tge ? (code | 0x01u) : (code & 0xFEu);
  ctx->mem->write8(p + 7, code);
}

// Primitive initialisers -- set len + code byte, leave colour/coords untouched.
//   POLY_F4 : 4 verts, flat shaded, untextured. code = 0x28, len = 5.
//   POLY_FT4: 4 verts, flat shaded, textured.   code = 0x2C, len = 9.
//   SPRT    : variable-size textured sprite.    code = 0x64, len = 4.
//   SPRT_8  : 8x8 textured sprite.              code = 0x74, len = 3.
//   SPRT_16 : 16x16 textured sprite.            code = 0x7C, len = 3.

void hle_libgpu_SetPolyF4(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 5);
  writeCode(ctx, p, 0x28);
}

void hle_libgpu_SetPolyFT4(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 9);
  writeCode(ctx, p, 0x2C);
}

void hle_libgpu_SetSprt(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 4);
  writeCode(ctx, p, 0x64);
}

void hle_libgpu_SetSprt8(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 3);
  writeCode(ctx, p, 0x74);
}

void hle_libgpu_SetSprt16(recomp_context *ctx) {
  uint32_t p = ctx->r[A0];
  writeLen(ctx, p, 3);
  writeCode(ctx, p, 0x7C);
}

// Group 1.A -- display-area / VRAM-transfer / video-mode HLEs
//
// All take args in the standard PsyQ a0..a3 + stack-spill convention; values
// are converted to GP0/GP1 commands and pushed via `writeGP0`/`writeGP1`.

// SetDispMask(mask): GP1(0x03, mask).  mask=1 enable, mask=0 disable.
void hle_libgpu_SetDispMask(recomp_context *ctx) {
  uint32_t mask = ctx->r[A0] & 0x1u;
  // GP1(0x03): bit 0 = display disable (1 = OFF, 0 = ON).
  // PsyQ SetDispMask(1) = enable, so we invert here.
  writeGP1(0x03000000u | (mask ? 0u : 1u));
}

// LoadImage(rect*, src*) -- GP0(0x01) Clear Cache + GP0(0xA0) + (w*h+1)/2
// data words.
//
// LoadImage(rect,data) forwards to _addque2(dws, rect, sizeof(RECT), data)
// (psyz decomp/src/libgpu/sys.c:280-284), and _dws -- the queued executor
// this function's body doubles as (see hle_libgpu__dws below) -- always
// issues a GP0(0x01) "Clear Cache" word immediately before the CPU->VRAM
// transfer header (sys.c:767-769: `*GPU_STATUS = STATUS_READY_TO_RECEIVE_CMD;
// *GPU_DATA = CMD_CLEAR_CACHE; *GPU_DATA = ... CMD_COPY_CPU_TO_VRAM;`, with
// CMD_CLEAR_CACHE = 0x01000000 at sys.c:148). Audited 2026-07-28: the
// pre-audit implementation omitted this leading word. gpu.cpp's
// executeClearCache() is a documented NOP for this software rasterizer, so
// the omission had no visible symptom, but the GP0 stream did not match what
// real PsyQ emits on every one of the 1458 CPU->VRAM uploads measured per
// run (2026-07-27).
// Two deviations from _dws (sys.c:745-783) that this audit did not close, left
// as behaviour changes with game-visible risk rather than silent fixes:
//   * sys.c:753-754 clamps rect.w/h against info.w/h and writes the clamped
//     values back into the CALLER's RECT. We do not write back, so a game
//     passing an oversized rect keeps its original values.
//   * sys.c:756-758 returns -1 when there is nothing to write. We return early
//     with V0=0, so an empty rect reports success. Note this deviation is
//     about _dws's OWN return; LoadImage never propagates it either way,
//     because it returns what _addque2 hands back (0) -- see below.
// Neither is exercised by Crash's 1458 uploads (all in-range, none empty), so
// changing them now would be an unmeasured behaviour change on the one path
// that currently renders. Revisit if a game depends on either.
//
// Return value: `int LoadImage(RECT*, u_long*)` tail-returns _addque2's value
// (sys.c:280-284 -- the `return` is on the addque2 call, with no early exit),
// and _addque2's immediate path returns 0, proven by disassembly of the Crash
// binary (`move v0,zero` at 0x80042148; LoadImage at 0x800404D0-D8 restores RA
// and returns straight after the jalr). See hle_libgpu__addque2 below for the
// full trace. So V0 = 0 on every path, including the empty-rect early return,
// which the source does not have at all.
void hle_libgpu_LoadImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  uint32_t src = ctx->r[A1];
  ctx->r[V0] = 0;
  if (r.w <= 0 || r.h <= 0)
    return;
  GpuBatchGuard batch;

  writeGP0(0x01000000u); // GP0(0x01): Clear Cache
  writeGP0(0xA0000000u);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));

  // Data: w * h pixels at 16bpp = (w*h + 1) / 2 32-bit words.
  uint32_t pixels = static_cast<uint32_t>(r.w) * static_cast<uint32_t>(r.h);
  uint32_t words = (pixels + 1u) / 2u;
  for (uint32_t i = 0; i < words; ++i)
    writeGP0(ctx->mem->read32(src + i * 4));
}

// StoreImage(rect*, dst*) -- GP0(0xC0); drains GPUREAD into PS1 RAM.
// The runtime GPU implements VRAM->CPU via GPUREAD register polling. We can't
// reach `gpuRead_` directly from here, but the GPU's CPU->VRAM/VRAM->CPU state
// machine processes the rect on GP0(0xC0) submission. Until a `readGPUREAD`
// drain hook is exposed, this stub just pumps the command and leaves the
// destination buffer untouched. Logged so misuse is visible.
void hle_libgpu_StoreImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  if (r.w <= 0 || r.h <= 0)
    return;
  GpuBatchGuard batch;
  writeGP0(0xC0000000u);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
  warnOnceFor("libgpu_StoreImage");
}

// MoveImage(rect*, x, y) -- GP0(0x80) VRAM->VRAM blit.
void hle_libgpu_MoveImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  uint16_t dx = static_cast<uint16_t>(ctx->r[A1]);
  uint16_t dy = static_cast<uint16_t>(ctx->r[A2]);
  if (r.w <= 0 || r.h <= 0)
    return;
  GpuBatchGuard batch;
  writeGP0(0x80000000u);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(dy) << 16 | static_cast<uint32_t>(dx));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
}

// ClearImage(rect*, r, g, b) -- GP0(0x02) FillRect.
void hle_libgpu_ClearImage(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  uint8_t cr = static_cast<uint8_t>(ctx->r[A1]);
  uint8_t cg = static_cast<uint8_t>(ctx->r[A2]);
  uint8_t cb = static_cast<uint8_t>(ctx->r[A3]);
  if (r.w <= 0 || r.h <= 0)
    return;
  GpuBatchGuard batch;
  writeGP0(0x02000000u | (static_cast<uint32_t>(cb) << 16) |
           (static_cast<uint32_t>(cg) << 8) | cr);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
}

// DrawSyncCallback(fn): record + return previous. Not invoked by the runtime
// because the GPU is fully synchronous (DrawSync always returns 0 immediately).
void hle_libgpu_DrawSyncCallback(recomp_context *ctx) {
  uint32_t prev = g_drawSyncCallback;
  g_drawSyncCallback = ctx->r[A0];
  ctx->r[V0] = prev;
}

// VSyncCallback(fn): record into psyq_state().gpuSwapCb + return previous.
// bios.cpp::triggerVBlankEvent queues this via queueCallbackWithArg(a0=4)
// so the game thread can dispatch it from drainPendingCallbacks safely.
void hle_libgpu_VSyncCallback(recomp_context *ctx) {
  auto &slot = psyq_state().gpuSwapCb;
  uint32_t prev = slot;
  slot = ctx->r[A0];
  ps1::metrics::trace("psyq.vsync_callback_register",
                      {{"callback", slot}, {"previous", prev}});
  ctx->r[V0] = prev;
}

// SetVideoMode(mode): 0=NTSC, 1=PAL. Stores in module state, returns prev.
// Real impl would also reissue GP1(0x08) with the new VRES bits; deferred
// until a game actually exercises mid-run mode switching.
void hle_libgpu_SetVideoMode(recomp_context *ctx) {
  int prev = g_videoMode;
  g_videoMode = static_cast<int>(ctx->r[A0]) & 0x1;
  ctx->r[V0] = static_cast<uint32_t>(prev);
}

void hle_libgpu_GetVideoMode(recomp_context *ctx) {
  ctx->r[V0] = static_cast<uint32_t>(g_videoMode);
}

int getVideoMode() { return g_videoMode; }

void setVideoMode(int mode) { g_videoMode = mode & 0x1; }

// Group 1.A -- libgs scene-graph stubs
//
// libgs is a higher-level wrapper around libgpu; neither Rayman nor Crash
// links it. These NOP stubs keep the registry dispatch happy and warn once
// per name so missing real implementations are visible.

void hle_libgs_GsInitGraph(recomp_context *ctx) {
  (void)ctx;
  warnOnceFor("libgs_GsInitGraph");
}
void hle_libgs_GsDefDispBuff(recomp_context *ctx) {
  (void)ctx;
  warnOnceFor("libgs_GsDefDispBuff");
}
// checkRECT(log, rect) -- PSY-Q internal debug validator (GPU_printf on a
// bad rect when info.level >= 1). Pure diagnostics; NOP matches the
// release-mode behaviour (psyz decomp libgpu/sys.c:249).
void hle_libgpu_checkRECT(recomp_context *ctx) { (void)ctx; }

// _addque2(exec, p1, len, p2) -- PSY-Q internal GPU op queue (libgpu sys.c).
// LoadImage/StoreImage/ClearImage/DrawOTag/PutDrawEnv all submit their device
// routine through here.
//
// Audited 2026-08-08 by disassembling the Crash binary, which the phase-1 plan
// admits as a source. The previous note here cited psyz's PC reimplementation
// (psyz/src/psyz/*.c) -- a port, not the decomp -- and was withdrawn in
// 750b401; the decomp body is `INCLUDE_ASM` only (psyz
// decomp/src/libgpu/sys.c:866, with no matching entry under
// asm/nonmatchings/libgpu/sys/). This comment replaces that citation entirely.
//
// Source: test_roms/Crash Bandicoot /Crash Bandicoot (USA).bin.boot.exe,
// PS-EXE t_addr=0x80010000, 0x800 header => file offset = va - 0x80010000 +
// 0x800. _addque2 spans 0x80042000..0x800422E0 (file 0x32800..0x32AE0).
// Full listing: .superpowers/sdd/phase-1-hle-audit/addque2.dis (gitignored).
//
// That 0x80042000 really is libgpu's _addque2 is established by the binary
// itself, not by the config's naming:
//   * 0x80042000 and 0x80041FDC sit as adjacent words at 0x80054A2C/0x80054A28
//     in a function-pointer table whose slot 0 (0x80054A24) holds the string
//     "$Id: sys.c,v 1.120 1996/05/01 12:09:07 noda Exp $" -- PSY-Q libgpu's
//     own RCS tag.
//   * 0x80041FDC's whole body is `a3 = a2; a2 = 0; tailcall 0x80042000` --
//     which is `_addque` as the decomp spells it out at
//     psyz decomp/src/libgpu/sys.c:862-864, `int _addque(int (*exec)(u_long,
//     u_long), u_long p1, u_long p2) { return _addque2(exec, p1, 0, p2); }`.
//     So the routine 0x80041FDC tail-calls is _addque2 by construction.
//   * Every caller loads that table and calls slot +0x08. LoadImage
//     (0x80040484) passes a0 = slot +0x20 (= _dws, 0x800419E4, the address
//     configs/crash_recomp.toml:528-529 also assigns to libgpu__dws) and
//     a2 = 8 = sizeof(RECT) -- exactly sys.c:280-284's `return
//     D_800B8920->addque2(D_800B8920->dws, rect, sizeof(RECT), p)`.
//
// Semantics derived instruction by instruction (register file: s3=exec,
// s0=p1, s1=len, s2=p2):
//
//   0x80042024  arm a VSync(-1)-based timeout (0x80042864)
//   0x8004204c  if ((head+1) & 0x3F) == tail  -> queue full: poll 0x80042898,
//               return -1 if it reports timeout, else run _exeque and retry
//   0x8004206c  saved_mask = SetIntrMask(0)      (0x8003E870 swaps *0x1F801074)
//   0x80042094  if queue-enable byte [0x80054A6D] == 0 -> immediate path
//               else immediate path only if ALL of:
//                 head == tail                    (queue empty)
//                 !(*D2_CHCR & 0x01000000)        (GPU DMA not busy)
//                 [0x80054A78] == 0               (no DrawSync callback)
//   immediate path (0x800420EC..0x80042148):
//               spin until (*GPUSTAT & 0x04000000); exec(p1, p2) via `jalr s3`;
//               record exec/p1/p2 into 0x80054B60/64/68; SetIntrMask(saved);
//               *** return 0 *** (`move v0,zero` at 0x80042148)
//   enqueue path (0x8004214C..0x800422C0):
//               register _exeque as the DMA callback, copy len/4 words of p1
//               into a 96-byte ring slot, store exec/p2/(&slot.buf or p1),
//               head = (head+1) & 0x3F, restore mask, run _exeque, and
//               return (head - tail) & 0x3F -- the queue depth.
//
// The register identities above are read out of the binary, not assumed: the
// pointer table holds 0x1F801814 (GPUSTAT/GP1) at 0x80054B40, 0x1F8010A8
// (D2_CHCR) at 0x80054B4C and 0x1F801074 (I_MASK) at 0x800549C4, and the
// timeout printf at 0x800428E8 formats exactly those, in that order, against
// "GPU timeout:que=%d,stat=%08x,chcr=%08x,madr=%08x," (0x80011350).
//
// What this HLE models is the immediate path, and that is the correct choice
// here: this runtime's GPU is synchronous, so the queue is always empty, DMA2
// is never left busy, and Crash never installs a DrawSync callback -- the three
// conditions that select it. Consequences, all confirmed above:
//   * `len` (A2) is genuinely unused: the immediate path hands the caller's own
//     p1 to exec and never touches the ring's copy buffer.
//   * p2 comes from A3, and exec is called as exec(p1, p2).
//   * the return value is 0, not exec's. This was the one proven divergence --
//     we previously let exec's V0 leak through, which happened to be 0 for
//     every executor we dispatch (_dws and _cwc both force V0=0) but was not
//     guaranteed. Callers tail-return this value (LoadImage at 0x800404D0-D8
//     restores RA and returns straight after the jalr), so it is observable.
//
// Not modelled, deliberately: the interrupt-mask critical section, the GPUSTAT
// ready-to-receive spin, the 64-slot ring and its len/4 word copy, the queue
// -full retry with its VSync(-1) + 240 deadline, and the -1 timeout return.
// All of them exist only to serialise against an asynchronous GPU/DMA that this
// runtime does not have. Note the ring copy has no bound check against the
// 84-byte slot buffer, so a hypothetical len > 84 would corrupt the next slot;
// no caller in this binary passes anything but 0 or 8.
//
// The `exec == 0` guard is a defensive addition with no source counterpart --
// 0x80042110 is an unconditional `jalr s3`. Every real caller (LoadImage/_dws,
// StoreImage/_drs, ClearImage/_clr, DrawOTag's and PutDrawEnv's _cwc) passes a
// valid routine. The test that pins it says so explicitly.
void hle_libgpu__addque2(recomp_context *ctx) {
  uint32_t exec = ctx->r[A0];
  uint32_t p1 = ctx->r[A1];
  uint32_t p2 = ctx->r[A3]; // (A2 = len; dead on the immediate path)
  if (exec == 0) {
    ctx->r[V0] = 0;
    return;
  }
  uint32_t ra = ctx->r[RA];
  ctx->r[A0] = p1;
  ctx->r[A1] = p2;
  recomp_dispatch(ctx->mem->ramPtr(), ctx, exec);
  ctx->r[RA] = ra;
  ctx->r[V0] = 0; // immediate path returns 0, not exec's value (0x80042148)
}

// _addque(exec, p1, p2) -- 3-arg sibling of _addque2. Its entire body at
// 0x80041FDC is `a3 = a2; a2 = 0; jal _addque2`, so it is _addque2 with len=0,
// with the same return value. See the _addque2 note above for the audit.
void hle_libgpu__addque(recomp_context *ctx) {
  uint32_t exec = ctx->r[A0];
  uint32_t p1 = ctx->r[A1];
  uint32_t p2 = ctx->r[A2];
  if (exec == 0) {
    ctx->r[V0] = 0;
    return;
  }
  uint32_t ra = ctx->r[RA];
  ctx->r[A0] = p1;
  ctx->r[A1] = p2;
  recomp_dispatch(ctx->mem->ramPtr(), ctx, exec);
  ctx->r[RA] = ra;
  ctx->r[V0] = 0;
}

// _dws(rect*, data*) -- device write: the queued executor behind LoadImage
// (CPU RAM -> VRAM). Identical argument layout and GP0 sequence to LoadImage
// (psyz decomp/src/libgpu/sys.c:745-783, `int _dws(RECT*, u_long*)`) --
// confirmed as the same routine LoadImage's addque2 call dispatches to
// (sys.c:280-284). This is the 1458-calls/run item from the phase-1
// measurement: _dws is what actually runs each frame; LoadImage itself is
// never dispatched by name (it only runs here via this direct C++ call).
void hle_libgpu__dws(recomp_context *ctx) {
  hle_libgpu_LoadImage(ctx);
  ctx->r[V0] = 0;
}

// _drs(rect*, data*) -- device read: the executor behind StoreImage.
// NOTE (2026-07-28 audit): sys.c's _drs (sys.c:787-831) issues the same
// leading GP0(0x01) Clear Cache word _dws does before its VRAM->CPU header.
// StoreImage/_drs is out of scope for this audit pass (not in the phase-1
// mandatory list -- its VRAM->CPU drain is separately stubbed, see
// hle_libgpu_StoreImage's own comment); flagging here so a future
// StoreImage audit does not have to rediscover it.
void hle_libgpu__drs(recomp_context *ctx) {
  hle_libgpu_StoreImage(ctx);
  ctx->r[V0] = 0;
}

// _clr(rect*, color) -- the executor behind ClearImage/ClearImage2.
// `color` is packed (b<<16)|(g<<8)|r; bit 31 selects the semi-transparent
// variant which GP0(0x02) does not model, so it is masked off.
void hle_libgpu__clr(recomp_context *ctx) {
  PsyqRect r = readRect(ctx, ctx->r[A0]);
  uint32_t color = ctx->r[A1] & 0x00FFFFFFu;
  if (r.w <= 0 || r.h <= 0) {
    ctx->r[V0] = 0;
    return;
  }
  writeGP0(0x02000000u | color);
  writeGP0(static_cast<uint32_t>(r.y & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.x & 0xFFFF));
  writeGP0(static_cast<uint32_t>(r.h & 0xFFFF) << 16 |
           static_cast<uint32_t>(r.w & 0xFFFF));
  ctx->r[V0] = 0;
}

void hle_libgs_GsSetWorkBase(recomp_context *ctx) {
  (void)ctx;
  warnOnceFor("libgs_GsSetWorkBase");
}
void hle_libgs_GsSortClear(recomp_context *ctx) {
  (void)ctx;
  warnOnceFor("libgs_GsSortClear");
}

void psyq_register_libgpu_extras() {
  psyq_register("libgpu_GetClut", &hle_libgpu_GetClut);
  psyq_register("libgpu_SetShadeTex", &hle_libgpu_SetShadeTex);
  psyq_register("libgpu_SetPolyF4", &hle_libgpu_SetPolyF4);
  psyq_register("libgpu_SetPolyFT4", &hle_libgpu_SetPolyFT4);
  psyq_register("libgpu_SetSprt", &hle_libgpu_SetSprt);
  psyq_register("libgpu_SetSprt8", &hle_libgpu_SetSprt8);
  psyq_register("libgpu_SetSprt16", &hle_libgpu_SetSprt16);

  // Group 1.A
  psyq_register("libgpu_SetDispMask", &hle_libgpu_SetDispMask);
  psyq_register("libgpu_LoadImage", &hle_libgpu_LoadImage);
  psyq_register("libgpu_StoreImage", &hle_libgpu_StoreImage);
  psyq_register("libgpu_MoveImage", &hle_libgpu_MoveImage);
  psyq_register("libgpu_ClearImage", &hle_libgpu_ClearImage);
  psyq_register("libgpu_DrawSyncCallback", &hle_libgpu_DrawSyncCallback);
  psyq_register("libgpu_checkRECT", &hle_libgpu_checkRECT);
  psyq_register("libgpu__addque", &hle_libgpu__addque);
  psyq_register("libgpu__addque2", &hle_libgpu__addque2);
  psyq_register("libgpu__dws", &hle_libgpu__dws);
  psyq_register("libgpu__drs", &hle_libgpu__drs);
  psyq_register("libgpu__clr", &hle_libgpu__clr);
  // VSyncCallback / SetVideoMode / GetVideoMode live in libetc per
  // psyq_signatures.toml (verified for v3.5/v4.0 LIBETC).
  psyq_register("libetc_VSyncCallback", &hle_libgpu_VSyncCallback);
  // Crash Bandicoot calls VSyncCallbacks (plural) -- same semantics in PsyQ
  // (registers a per-frame callback into psyq_state().gpuSwapCb).
  psyq_register("libetc_VSyncCallbacks", &hle_libgpu_VSyncCallback);
  psyq_register("libetc_SetVideoMode", &hle_libgpu_SetVideoMode);
  psyq_register("libetc_GetVideoMode", &hle_libgpu_GetVideoMode);

  psyq_register("libgs_GsInitGraph", &hle_libgs_GsInitGraph);
  psyq_register("libgs_GsDefDispBuff", &hle_libgs_GsDefDispBuff);
  psyq_register("libgs_GsSetWorkBase", &hle_libgs_GsSetWorkBase);
  psyq_register("libgs_GsSortClear", &hle_libgs_GsSortClear);

  // Crash Bandicoot extras: bit-pack helpers psyz/decomp/src/libgpu/prim.c
  // shows are trivial.  GetTPage packs (tp,abr,x,y) into a u_short; SetDrawMode
  // sets the first two words of a DR_MODE struct (mode word + tex window).
  psyq_register("libgpu_GetTPage", [](recomp_context *ctx) {
    int tp = static_cast<int>(ctx->r[A0]);
    int abr = static_cast<int>(ctx->r[A1]);
    int x = static_cast<int>(ctx->r[A2]);
    int y = static_cast<int>(ctx->r[A3]);
    uint32_t r = ((tp & 3) << 7) | ((abr & 3) << 5) | ((y & 0x100) >> 4) |
                 ((x & 0x3FF) >> 6) | ((y & 0x200) << 2);
    ctx->r[V0] = r & 0xFFFFu;
  });
  // SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT *tw):
  // DR_MODE { u_long tag; u_long code[2]; }; code[0] = GP0(E1) draw mode,
  // code[1] = GP0(E2) texture window. 5th arg (tw) travels on the stack at
  // sp+16 (o32 ABI) -- confirmed from recompiled_out.cpp's callers of
  // func_80041054 (SetDrawMode's HLE'd address), which write the RECT*
  // via MEM_WRITE32(ctx, ctx->r29 + 16, ...) right before the call.
  //
  // Encoding confirmed against the psyz decomp reference (workspace clone,
  // PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c):
  //   SetDrawMode: sys.c:503-506 -- setlen(p,2); code[0]=get_mode(...);
  //     code[1]=get_tw(tw).
  //   get_mode: sys.c:624-630. Two branches key off info.version (GPU
  //     hardware type, not PsyQ SDK version): 1/2 select an old
  //     large-coordinate devkit GPU encoding; every other value selects the
  //     retail/production encoding used below. info.version is 0 or 3 for
  //     1 MB VRAM (the D_800B89BC height table at sys.c:108), which is what
  //     Crash (and every retail PS1) targets, so the retail branch applies:
  //       (dtd ? 0xE1000200 : 0xE1000000) | (dfe ? 0x400 : 0) |
  //       (tpage & 0x9FF)
  //     i.e. bit9 = dither (dtd), bit10 = draw-in-display-area-allowed
  //     (dfe), and tpage supplies bits 0-8 plus bit 11 unmasked -- matching
  //     psx-spx's GP0(E1h) layout (bit11 = "texpage Y base 2, 2MB VRAM
  //     only"). PsyQ does not mask bit 11 out for retail builds; it is
  //     simply never set by GetTPage for in-range (<512) VRAM Y coordinates
  //     on 1 MB hardware, so it is always 0 here in practice (matches the
  //     2026-07-22 project finding that bit 11 does not apply to this
  //     target).
  //   get_tw: sys.c:662-673 -- returns the literal value 0 when tw==NULL
  //     (i.e. GP0(0x00), a NOP -- not a GP0(0xE2) command). For a non-null
  //     RECT {x,y,w,h}, it packs offsetY<<15 | offsetX<<10 | maskY<<5 |
  //     maskX (all in 8px steps), matching psx-spx's GP0(E2h) layout.
  psyq_register("libgpu_SetDrawMode", [](recomp_context *ctx) {
    uint32_t p = ctx->r[A0];
    if (p == 0)
      return;
    bool dfe = ctx->r[A1] != 0;
    bool dtd = ctx->r[A2] != 0;
    uint32_t tpage = ctx->r[A3];
    uint32_t tw = ctx->mem->read32(ctx->r[SP] + 16); // RECT *tw, o32 stack arg

    writeLen(ctx, p, 2);

    uint32_t mode = (dtd ? 0xE1000200u : 0xE1000000u) | (dfe ? 0x400u : 0u) |
                    (tpage & 0x9FFu);
    ctx->mem->write32(p + 4, mode);

    uint32_t twWord = 0;
    if (tw != 0) {
      PsyqRect r = readRect(ctx, tw);
      auto lowByte = [](int32_t v) { return static_cast<uint32_t>(v) & 0xFFu; };
      uint32_t offsX = lowByte(r.x) >> 3;
      uint32_t offsY = lowByte(r.y) >> 3;
      uint32_t maskX = lowByte(-static_cast<int32_t>(r.w)) >> 3;
      uint32_t maskY = lowByte(-static_cast<int32_t>(r.h)) >> 3;
      twWord =
          0xE2000000u | (offsY << 15) | (offsX << 10) | (maskY << 5) | maskX;
    }
    ctx->mem->write32(p + 8, twWord);
  });
}

} // namespace ps1::psyq
