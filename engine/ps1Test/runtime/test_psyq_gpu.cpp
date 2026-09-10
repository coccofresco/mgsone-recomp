// Tests for the Group 1.A libgpu/libetc/libgs HLEs:
//   SetDispMask, LoadImage, StoreImage, MoveImage, ClearImage,
//   DrawSyncCallback, VSyncCallback, SetVideoMode, GetVideoMode,
//   GsInitGraph, GsDefDispBuff, GsSetWorkBase, GsSortClear.
//
// The libgpu transfer/clear ops translate PsyQ args into GP0/GP1 commands.
// The fixture installs a recording HleConfig so each test can assert the exact
// command sequence written by the HLE.

#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

namespace {

enum class GpuBatchEvent : uint8_t { Begin, Word, End };

class PsyqGpuTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  std::vector<uint32_t> gp0;
  std::vector<uint32_t> gp1;
  std::vector<GpuBatchEvent> gp0BatchEvents;
  std::vector<int> gp0BatchDepths;
  int gp0BatchDepth = 0;
  int maxGp0BatchDepth = 0;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    gp0.clear();
    gp1.clear();
    gp0BatchEvents.clear();
    gp0BatchDepths.clear();
    gp0BatchDepth = 0;
    maxGp0BatchDepth = 0;
    HleConfig cfg;
    cfg.beginGP0Batch = [this]() {
      gp0BatchEvents.push_back(GpuBatchEvent::Begin);
      ++gp0BatchDepth;
      maxGp0BatchDepth = std::max(maxGp0BatchDepth, gp0BatchDepth);
    };
    cfg.writeGP0 = [this](uint32_t w) {
      gp0BatchEvents.push_back(GpuBatchEvent::Word);
      gp0BatchDepths.push_back(gp0BatchDepth);
      gp0.push_back(w);
    };
    cfg.endGP0Batch = [this]() {
      gp0BatchEvents.push_back(GpuBatchEvent::End);
      --gp0BatchDepth;
    };
    cfg.writeGP1 = [this](uint32_t w) { gp1.push_back(w); };
    configure(cfg);
    psyq_state().reset();
  }

  void TearDown() override { psyq_state().reset(); }

  // Write a 4-int16 PsyqRect at `p`.
  void writeRect(uint32_t p, int16_t x, int16_t y, int16_t w, int16_t h) {
    mem.write16(p + 0, static_cast<uint16_t>(x));
    mem.write16(p + 2, static_cast<uint16_t>(y));
    mem.write16(p + 4, static_cast<uint16_t>(w));
    mem.write16(p + 6, static_cast<uint16_t>(h));
  }

  void expectSingleBatchAroundEntireGp0Sequence() {
    ASSERT_FALSE(gp0.empty());
    ASSERT_EQ(gp0BatchEvents.size(), gp0.size() + 2u);
    EXPECT_EQ(std::count(gp0BatchEvents.begin(), gp0BatchEvents.end(),
                         GpuBatchEvent::Begin),
              1);
    EXPECT_EQ(std::count(gp0BatchEvents.begin(), gp0BatchEvents.end(),
                         GpuBatchEvent::End),
              1);
    EXPECT_EQ(gp0BatchEvents.front(), GpuBatchEvent::Begin);
    EXPECT_EQ(gp0BatchEvents.back(), GpuBatchEvent::End);
    for (std::size_t i = 1; i + 1 < gp0BatchEvents.size(); ++i)
      EXPECT_EQ(gp0BatchEvents[i], GpuBatchEvent::Word) << "event " << i;

    ASSERT_EQ(gp0BatchDepths.size(), gp0.size());
    for (std::size_t i = 0; i < gp0BatchDepths.size(); ++i)
      EXPECT_EQ(gp0BatchDepths[i], 1) << "GP0 word " << i;
    EXPECT_EQ(maxGp0BatchDepth, 1);
    EXPECT_EQ(gp0BatchDepth, 0);
  }
};

} // namespace

// SetDispMask

TEST_F(PsyqGpuTest, SetDispMaskEnableSendsGP1_03_With0) {
  ctx.r[A0] = 1; // enable
  hle_libgpu_SetDispMask(&ctx);
  ASSERT_EQ(gp1.size(), 1u);
  EXPECT_EQ(gp1[0], 0x03000000u); // bit 0 = 0 -> display ON
}

TEST_F(PsyqGpuTest, SetDispMaskDisableSendsGP1_03_With1) {
  ctx.r[A0] = 0;
  hle_libgpu_SetDispMask(&ctx);
  ASSERT_EQ(gp1.size(), 1u);
  EXPECT_EQ(gp1[0], 0x03000001u);
}

// LoadImage / _dws
//
// LoadImage(rect,data) forwards to _addque2(dws, rect, sizeof(RECT), data)
// (psyz decomp/src/libgpu/sys.c:280-284: `return D_800B8920->addque2(
// (int(*)(u_long,u_long))D_800B8920->dws, (u_long)rect, sizeof(RECT),
// (u_long)p);`). _dws is the queued executor that actually runs the
// transfer -- and it is what accounts for the 1458 calls/run measured
// 2026-07-27 (LoadImage itself is registered but dispatched 0x/run; its
// body only runs because hle_libgpu__dws calls hle_libgpu_LoadImage
// directly in C++, psyq_libgpu.cpp).
//
// _dws's real GP0 sequence (sys.c:745-783, `int _dws(RECT* rect, u_long*
// data)`) is:
//   *GPU_STATUS = STATUS_READY_TO_RECEIVE_CMD;      // sys.c:767 -- GP1, see below
//   *GPU_DATA   = CMD_CLEAR_CACHE;                  // sys.c:768 (0x01000000, sys.c:148)
//   *GPU_DATA   = CMD_COPY_CPU_TO_VRAM;              // sys.c:769 (0xA0000000)
//   *GPU_DATA   = rect.x | rect.y << 16;              // sys.c:770
//   *GPU_DATA   = rect.w | rect.h << 16;              // sys.c:771
//   ... (w*h+1)/2 data words ...                      // sys.c:773-781
// The pre-audit implementation omitted the leading GP0(0x01) Clear Cache
// word entirely -- gpu.cpp's executeClearCache() is a documented NOP for
// this software rasterizer, so the omission produced no visible symptom,
// but the GP0 stream did not match what every real CPU->VRAM upload emits.
//
// Two gaps this audit did NOT close, recorded so they are not rediscovered:
//   * `*GPU_STATUS` is the GP1 port, not GP0. sys.c:767 is GP1(0x04) with
//     DMA direction 0, and sys.c:778 sets 0x04000002. Neither GP1 write is
//     emitted here; the runtime GPU does not model DMA direction for this
//     path, so nothing observable depends on them yet.
//   * sys.c:753-758 clamps rect.w/h against info.w/h and writes the clamped
//     values back into the caller's RECT, and returns -1 when there is
//     nothing to write. Our LoadImage returns early and hle_libgpu__dws
//     forces V0=0, so an empty rect reports success and no clamp is written
//     back. See the note on hle_libgpu_LoadImage in psyq_libgpu.cpp.

TEST_F(PsyqGpuTest, LoadImageEmitsClearCacheThenCpuToVramHeaderAndData) {
  // 4x2 pixels = 8 px = 4 words of pixel data.
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 320, 0, 4, 2);
  uint32_t pixels[4] = {0x12345678u, 0x9ABCDEF0u, 0x11112222u, 0x33334444u};
  for (int i = 0; i < 4; ++i) mem.write32(srcP + i * 4, pixels[i]);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  hle_libgpu_LoadImage(&ctx);

  ASSERT_EQ(gp0.size(), 4u + 4u);
  EXPECT_EQ(gp0[0], 0x01000000u);              // GP0(0x01): Clear Cache
  EXPECT_EQ(gp0[1], 0xA0000000u);              // CPU->VRAM cmd
  EXPECT_EQ(gp0[2], (0u << 16) | 320u);        // Y|X
  EXPECT_EQ(gp0[3], (2u << 16) | 4u);          // H|W
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(gp0[4 + i], pixels[i]);
  expectSingleBatchAroundEntireGp0Sequence();
}

TEST_F(PsyqGpuTest, LoadImageOddPixelCountRoundsUpDataWords) {
  // 3x1 pixels = 3 px -> ceil(3/2)=2 words.
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 0, 0, 3, 1);
  mem.write32(srcP + 0, 0xCAFEBABEu);
  mem.write32(srcP + 4, 0xDEADBEEFu);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  hle_libgpu_LoadImage(&ctx);
  ASSERT_EQ(gp0.size(), 6u); // clear-cache + 3 hdr + 2 data
  EXPECT_EQ(gp0[0], 0x01000000u);
  EXPECT_EQ(gp0[4], 0xCAFEBABEu);
  EXPECT_EQ(gp0[5], 0xDEADBEEFu);
}

TEST_F(PsyqGpuTest, LoadImageZeroSizeIsNoop) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 0, 0);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100100u;
  hle_libgpu_LoadImage(&ctx);
  EXPECT_TRUE(gp0.empty());
}

// `int LoadImage(RECT*, u_long*)` tail-returns _addque2's value (sys.c:280-284
// -- the `return` is on the addque2 call, and there is no early exit), and
// _addque2's immediate path returns 0 (`move v0,zero` at 0x80042148 in the
// Crash binary; LoadImage at 0x800404D0-D8 restores RA and returns straight
// after the jalr). The HLE left V0 untouched, so callers saw whatever the
// previous HLE happened to leave in it.
TEST_F(PsyqGpuTest, LoadImageReturnsZero) {
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 0, 0, 2, 1);
  mem.write32(srcP, 0xABCD1234u);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  ctx.r[V0] = 0xDEADBEEFu; // stale value from a previous call
  hle_libgpu_LoadImage(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// The source has no empty-rect early exit at all, so the zero-size shortcut
// this HLE takes must still leave the same return value behind.
TEST_F(PsyqGpuTest, LoadImageReturnsZeroOnZeroSizeRect) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 0, 0);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100100u;
  ctx.r[V0] = 0xDEADBEEFu;
  hle_libgpu_LoadImage(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// _dws is the symbol actually dispatched 1458x/run (LoadImage itself is
// never dispatched by name); exercise it through the registry rather than
// trusting the delegation in psyq_libgpu.cpp is wired correctly.
TEST_F(PsyqGpuTest, DwsDispatchesLikeLoadImageAndReturnsZero) {
  psyq_register_libgpu_extras();
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 64, 8, 2, 1); // 2 px -> 1 data word
  mem.write32(srcP, 0xABCD1234u);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  ctx.r[V0] = 0xFFFFFFFFu;
  psyq_dispatch("libgpu__dws", &ctx);

  ASSERT_EQ(gp0.size(), 4u + 1u);
  EXPECT_EQ(gp0[0], 0x01000000u);
  EXPECT_EQ(gp0[1], 0xA0000000u);
  EXPECT_EQ(gp0[2], (8u << 16) | 64u);
  EXPECT_EQ(gp0[3], (1u << 16) | 2u);
  EXPECT_EQ(gp0[4], 0xABCD1234u);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// _addque2
//
// Audited 2026-08-08 against the disassembly of _addque2 at 0x80042000 in
// test_roms/Crash Bandicoot /Crash Bandicoot (USA).bin.boot.exe -- the source
// the phase-1 plan admits alongside the decomp, used because the decomp body
// is `INCLUDE_ASM`-only (decomp/src/libgpu/sys.c:866). The earlier note here
// cited psyz's PC reimplementation, which is a port and not admissible; it was
// withdrawn in 750b401. Listing and derivation:
// .superpowers/sdd/phase-1-hle-audit/addque2.dis and task-4c-report.md, and
// the instruction-level summary above hle_libgpu__addque2 in psyq_libgpu.cpp.
//
// The three facts the tests below pin, and where each comes from:
//   * exec is called as exec(p1, p2), p2 taken from a3 -- `move a0,s0;
//     move a1,s2; jalr s3` at 0x8004210C-0x80042114, with s0=a1(p1) and
//     s2=a3(p2) from the prologue at 0x80042010/0x80042028.
//   * `len` (a2) is dead on this path -- the immediate path passes the
//     caller's own p1 through and never reaches the ring-copy loop at
//     0x80042164, which is the only reader of s1=len.
//   * the return value is 0, NOT exec's -- `move v0,zero` at 0x80042148, in
//     the delay slot of the jump to the epilogue. Callers tail-return it
//     (LoadImage at 0x800404D0 restores RA immediately after the jalr), so
//     this is observable to the game.

// Installs a stand-in for the dispatched device routine so these tests can see
// what exec receives and prove _addque2 discards what exec returns.
extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);

namespace {
struct ScopedDispatchHook {
  explicit ScopedDispatchHook(void (*fn)(recomp_context *, uint32_t)) {
    g_testRecompDispatchHook = fn;
  }
  ~ScopedDispatchHook() { g_testRecompDispatchHook = nullptr; }
};

// Observations recorded by the stand-in exec.
struct ExecObservation {
  int calls = 0;
  uint32_t addr = 0, a0 = 0, a1 = 0;
};
ExecObservation g_exec;
} // namespace

TEST_F(PsyqGpuTest, AddQue2CallsExecWithP1AndP2AndReturnsZeroNotExecsValue) {
  psyq_register_libgpu_extras();
  g_exec = ExecObservation{};
  ScopedDispatchHook hook([](recomp_context *c, uint32_t addr) {
    g_exec.calls++;
    g_exec.addr = addr;
    g_exec.a0 = c->r[A0];
    g_exec.a1 = c->r[A1];
    c->r[V0] = 0x5A5A5A5Au; // exec returns nonzero; _addque2 must discard it
  });

  ctx.r[A0] = 0x80012340u; // exec
  ctx.r[A1] = 0x80100000u; // p1
  ctx.r[A2] = 0x99u;       // len -- dead on the immediate path
  ctx.r[A3] = 0x80200000u; // p2
  ctx.r[RA] = 0x80099999u;

  psyq_dispatch("libgpu__addque2", &ctx);

  EXPECT_EQ(g_exec.calls, 1) << "exec must be invoked exactly once";
  EXPECT_EQ(g_exec.addr, 0x80012340u) << "a0 selects the routine to dispatch";
  EXPECT_EQ(g_exec.a0, 0x80100000u) << "exec's a0 is p1 (0x8004210C)";
  EXPECT_EQ(g_exec.a1, 0x80200000u)
      << "exec's a1 is p2 from a3, not a2=len (0x80042114)";
  EXPECT_EQ(ctx.r[V0], 0u)
      << "immediate path returns 0, not exec's value (0x80042148)";
  EXPECT_EQ(ctx.r[RA], 0x80099999u) << "RA must be restored after the call";
}

// _addque(exec, p1, p2) is _addque2 with len=0: its whole body at 0x80041FDC is
// `move a3,a2; move a2,zero; jal 0x80042000`. So p2 comes from a2 here, and the
// return value is the same 0.
TEST_F(PsyqGpuTest, AddQueTakesP2FromA2AndAlsoReturnsZero) {
  psyq_register_libgpu_extras();
  g_exec = ExecObservation{};
  ScopedDispatchHook hook([](recomp_context *c, uint32_t addr) {
    g_exec.calls++;
    g_exec.addr = addr;
    g_exec.a0 = c->r[A0];
    g_exec.a1 = c->r[A1];
    c->r[V0] = 0x5A5A5A5Au;
  });

  ctx.r[A0] = 0x80012340u; // exec
  ctx.r[A1] = 0x80100000u; // p1
  ctx.r[A2] = 0x80200000u; // p2 (3-arg form)
  ctx.r[A3] = 0xBADBAD00u; // must be ignored

  psyq_dispatch("libgpu__addque", &ctx);

  EXPECT_EQ(g_exec.calls, 1);
  EXPECT_EQ(g_exec.a0, 0x80100000u);
  EXPECT_EQ(g_exec.a1, 0x80200000u) << "_addque takes p2 from a2";
  EXPECT_EQ(ctx.r[V0], 0u);
}

// Pins OUR defensive null guard, not PsyQ semantics: the real _addque2 has no
// null check (its callers always pass a valid device routine). This test exists
// so the guard is not removed by accident, and it must not be read as evidence
// that the hardware behaves this way.
TEST_F(PsyqGpuTest, AddQue2NullExecTakesOurDefensiveGuardNotPsyqSemantics) {
  psyq_register_libgpu_extras();
  ctx.r[A0] = 0; // exec = NULL
  ctx.r[A1] = 0x80100000u;
  ctx.r[A3] = 0x80200000u;
  ctx.r[V0] = 0xDEADBEEFu;
  psyq_dispatch("libgpu__addque2", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// StoreImage

TEST_F(PsyqGpuTest, StoreImageEmitsVramToCpuHeader) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 64, 240, 16, 8);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100200u;
  hle_libgpu_StoreImage(&ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0xC0000000u);
  EXPECT_EQ(gp0[1], (240u << 16) | 64u);
  EXPECT_EQ(gp0[2], (8u   << 16) | 16u);
  expectSingleBatchAroundEntireGp0Sequence();
}

// MoveImage

TEST_F(PsyqGpuTest, MoveImageEmitsVramToVramSequence) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 32, 32);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 320; // dx
  ctx.r[A2] = 256; // dy
  hle_libgpu_MoveImage(&ctx);
  ASSERT_EQ(gp0.size(), 4u);
  EXPECT_EQ(gp0[0], 0x80000000u);
  EXPECT_EQ(gp0[1], (0u   << 16) | 0u);
  EXPECT_EQ(gp0[2], (256u << 16) | 320u);
  EXPECT_EQ(gp0[3], (32u  << 16) | 32u);
  expectSingleBatchAroundEntireGp0Sequence();
}

// ClearImage

TEST_F(PsyqGpuTest, ClearImageEmitsFillRect) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 320, 240);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x12; // r
  ctx.r[A2] = 0x34; // g
  ctx.r[A3] = 0x56; // b
  hle_libgpu_ClearImage(&ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0x02000000u | (0x56u << 16) | (0x34u << 8) | 0x12u);
  EXPECT_EQ(gp0[1], (0u   << 16) | 0u);
  EXPECT_EQ(gp0[2], (240u << 16) | 320u);
  expectSingleBatchAroundEntireGp0Sequence();
}

// Sync callbacks

TEST_F(PsyqGpuTest, DrawSyncCallbackStoresAndReturnsPrev) {
  ctx.r[A0] = 0x80012340u;
  ctx.r[V0] = 0xDEADu;
  hle_libgpu_DrawSyncCallback(&ctx);
  // First call: previous should be 0 (registry just-installed).
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 0xBEEFCAFEu;
  hle_libgpu_DrawSyncCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80012340u);
}

TEST_F(PsyqGpuTest, VSyncCallbackStoresAndReturnsPrev) {
  ctx.r[A0] = 0x80044000u;
  hle_libgpu_VSyncCallback(&ctx);
  // First install returns previous (0 -- singleton was reset in SetUp).
  EXPECT_EQ(ctx.r[V0], 0u);
  // The HLE writes through to psyq_state().gpuSwapCb so bios.cpp's VBlank
  // dispatch can find the swap callback without per-game BSS configuration.
  EXPECT_EQ(psyq_state().gpuSwapCb, 0x80044000u);

  // Subsequent install returns whatever we just wrote.
  ctx.r[A0] = 0;
  hle_libgpu_VSyncCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80044000u);
  EXPECT_EQ(psyq_state().gpuSwapCb, 0u);
}

// Video mode

TEST_F(PsyqGpuTest, SetGetVideoModeRoundTrip) {
  ctx.r[A0] = 0; // NTSC
  hle_libgpu_SetVideoMode(&ctx);
  hle_libgpu_GetVideoMode(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 1; // PAL
  hle_libgpu_SetVideoMode(&ctx);
  // Prev should have been NTSC=0
  EXPECT_EQ(ctx.r[V0], 0u);
  hle_libgpu_GetVideoMode(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);

  // Restore to NTSC so other tests start clean.
  ctx.r[A0] = 0;
  hle_libgpu_SetVideoMode(&ctx);
}

// libgs stubs

TEST_F(PsyqGpuTest, LibgsStubsAreNoopAndDoNotCrash) {
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 0; ctx.r[A3] = 0;
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsInitGraph(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsDefDispBuff(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsSetWorkBase(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsSortClear(&ctx));
  EXPECT_TRUE(gp0.empty());
  EXPECT_TRUE(gp1.empty());
}

// GetClut
//
// Audited 2026-08-06 against psx-spx (docs/graphicsprocessingunitgpu.md:
// 366-374, "Clut Attribute"), which documents the packed layout as hardware
// fact for the v0 (1 MB VRAM) GPU this project targets:
//   bits 0-5   X coordinate X/16 (16-halfword steps)
//   bits 6-14  Y coordinate 0-511 (9 bits)
//   bit 15     unused
// The psyz decomp's GetClut (decomp/src/libgpu/prim.c:9) only forwards to an
// SDK-internal `getClut` macro not present in the decomp tree, so psx-spx is
// the citable source here, not sys.c/prim.c directly. Verdict: already
// correct on first read -- no implementation change made, this test exists
// purely to pin the encoding.

TEST_F(PsyqGpuTest, GetClutPacksXShr4AndYIntoPsxSpxLayout) {
  // x=512 (multiple of 16, x>>4=32), y=300 -> (300&0x1FF)<<6 | 32
  ctx.r[A0] = 512;
  ctx.r[A1] = 300;
  hle_libgpu_GetClut(&ctx);
  EXPECT_EQ(ctx.r[V0], (300u << 6) | 32u);
}

TEST_F(PsyqGpuTest, GetClutXIsDividedBy16NotPassedThrough) {
  // x must be a CLUT-cell index (x/16), never the raw VRAM X coordinate.
  ctx.r[A0] = 128; // 128/16 = 8
  ctx.r[A1] = 0;
  hle_libgpu_GetClut(&ctx);
  EXPECT_EQ(ctx.r[V0], 8u);
}

TEST_F(PsyqGpuTest, GetClutYOccupiesBits6Through14) {
  ctx.r[A0] = 0;
  ctx.r[A1] = 511; // max in-range Y for the v0 (1 MB VRAM) GPU
  hle_libgpu_GetClut(&ctx);
  EXPECT_EQ(ctx.r[V0], 511u << 6);
  EXPECT_EQ(ctx.r[V0] & 0x3Fu, 0u) << "low 6 bits must stay reserved for X";
}

// SetDrawMode
//
// SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT *tw) must encode
// its arguments into the two GP0 words of the DR_MODE primitive.  The
// pre-2026-07 implementation wrote fixed 0xE1000000 / 0xE2000000
// placeholders, which forced texpage 0 on every textured primitive the game
// queued -- ~229000 GP0(0x7C) sprites per 15s run (remeasured 2026-08-09;
// the 456720 first recorded here was the same figure doubled by the
// publish-twice bug in GPU::publishMetrics(), fixed in eb15d52).
// Texpage 0 points at VRAM (0,0), where the framebuffer band lives, instead
// of the loaded texture pages at (512..1023, 384). Those pages do hold real
// data (7944 distinct colours), so this is a wrong-address bug, not missing
// data. Encoding confirmed against the psyz decomp reference (workspace
// clone at PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c):
//   SetDrawMode: sys.c:503-506.
//   get_mode (retail/production-GPU branch -- info.version 0/3, per the 1MB
//     VRAM height table at sys.c:108; Crash targets retail hardware, not the
//     2MB-VRAM devkit branch taken when info.version is 1 or 2): sys.c:624-630.
//   get_tw: sys.c:662-673.
// 5th arg (RECT *tw) travels on the stack at sp+16 (o32 ABI), confirmed via
// the recompiled_out.cpp callers of func_80041054 (SetDrawMode's HLE'd
// address): MEM_WRITE32(ctx, ctx->r29 + 16, ...).

TEST_F(PsyqGpuTest, SetDrawModeEncodesTexpageAndFlagsIntoGp0Word) {
  psyq_register_libgpu_extras();
  const uint32_t p = 0x80100000u;
  const int dfe = 1, dtd = 0;
  const int tpage = 0x1A; // texture page base the game would pass

  ctx.r[SP] = 0x801FFF00u;
  ctx.r[A0] = p;
  ctx.r[A1] = static_cast<uint32_t>(dfe);
  ctx.r[A2] = static_cast<uint32_t>(dtd);
  ctx.r[A3] = static_cast<uint32_t>(tpage);
  mem.write32(ctx.r[SP] + 16u, 0); // RECT *tw = NULL

  psyq_dispatch("libgpu_SetDrawMode", &ctx);

  const uint32_t mode = mem.read32(p + 4);
  EXPECT_EQ(mode >> 24, 0xE1u) << "must remain a GP0(E1) command";
  EXPECT_EQ(mode & 0x1FFu, static_cast<uint32_t>(tpage) & 0x1FFu)
      << "texpage bits were dropped -- this is the 2026-07 regression";
  EXPECT_EQ((mode >> 10) & 1u, static_cast<uint32_t>(dfe));
  EXPECT_EQ((mode >> 9) & 1u, static_cast<uint32_t>(dtd));
  EXPECT_EQ(mem.read8(p + 3), 2u) << "DR_MODE len must stay 2";
}

// get_tw(NULL) returns the literal value 0 (sys.c:672 `return 0;`), not a
// bare GP0(0xE2) command -- a null texture window becomes a GP0(0x00) NOP,
// which psx-spx notes is often inserted between Texpage and Rectangle
// commands anyway.
TEST_F(PsyqGpuTest, SetDrawModeWithNullTextureWindowEmitsZeroWord) {
  psyq_register_libgpu_extras();
  const uint32_t p = 0x80100000u;
  ctx.r[SP] = 0x801FFF00u;
  ctx.r[A0] = p;
  ctx.r[A1] = 0;
  ctx.r[A2] = 0;
  ctx.r[A3] = 0;
  mem.write32(ctx.r[SP] + 16u, 0); // RECT *tw = NULL

  psyq_dispatch("libgpu_SetDrawMode", &ctx);

  EXPECT_EQ(mem.read32(p + 8), 0x00000000u);
}

TEST_F(PsyqGpuTest, SetDrawModeEncodesTextureWindowRectIntoGp0Word) {
  psyq_register_libgpu_extras();
  const uint32_t p  = 0x80100000u;
  const uint32_t tw = 0x80100100u;
  ctx.r[SP] = 0x801FFF00u;
  ctx.r[A0] = p;
  ctx.r[A1] = 0;
  ctx.r[A2] = 0;
  ctx.r[A3] = 0;
  writeRect(tw, /*x=*/8, /*y=*/16, /*w=*/64, /*h=*/32);
  mem.write32(ctx.r[SP] + 16u, tw);

  psyq_dispatch("libgpu_SetDrawMode", &ctx);

  // maskX = (-w & 0xFF) >> 3 = (-64 & 0xFF) >> 3 = 0x18
  // maskY = (-h & 0xFF) >> 3 = (-32 & 0xFF) >> 3 = 0x1C
  // offsX = (x & 0xFF) >> 3  = (8 & 0xFF)  >> 3  = 0x01
  // offsY = (y & 0xFF) >> 3  = (16 & 0xFF) >> 3  = 0x02
  // word = E2000000 | offsY<<15 | offsX<<10 | maskY<<5 | maskX
  const uint32_t expected = 0xE2000000u | (0x02u << 15) | (0x01u << 10) |
                             (0x1Cu << 5) | 0x18u;
  EXPECT_EQ(mem.read32(p + 8), expected);
}

// GetTPage
//
// GetTPage(tp, abr, x, y) packs a texpage description into the u_short that
// feeds SetDrawMode's `tpage` argument (see the SetDrawMode tests above) --
// an error here reintroduces the same wrong-texpage symptom through a
// different HLE. Encoding confirmed against the psyz decomp reference
// (workspace clone, PS1Recomp-workspace/psyz/): GetTPage forwards directly
// to the getTPage() macro (decomp/src/libgpu/prim.c:5-6 -- `return
// getTPage(tp, abr, x, y);`), defined at
// chrono-cross-decomp/include/psyq/libgpu.h:262-264:
//   #define getTPage(tp, abr, x, y) \
//       ((((tp) & 0x3) << 7) | (((abr) & 0x3) << 5) | (((y) & 0x100) >> 4) | \
//        (((x) & 0x3ff) >> 6) | (((y) & 0x200) << 2))
// This is byte-for-byte what ps1Runtime's HLE already computes.
//
// Bit 11 (the `(y & 0x200) << 2` term) is psx-spx's GP0(E1h) "Texture page Y
// Base 2 (N*512), only for 2 MB VRAM" (workspace clone,
// PS1Recomp-workspace/psx-spx.github.io/docs/graphicsprocessingunitgpu.md:385,
// 899). It is part of the confirmed PsyQ macro -- the source does not mask
// it out -- so it stays. It is inert on this project's target: SetDrawMode
// already forwards it unmasked (`tpage & 0x9FF`), but gpu.cpp separately
// discards bit 11 when latching the mode word (`currentTexpage_ = cmd &
// 0x1FF`, gpu.cpp:766), and this target's y coordinates never reach 512, so
// no rendering path currently observes it (2026-07-22 project finding).
// Dropping the term here would be an undocumented deviation from the
// confirmed source, not a fix -- so the audit keeps it and documents why.

namespace {
uint32_t referenceGetTPage(int tp, int abr, int x, int y) {
  return static_cast<uint32_t>(((tp & 0x3) << 7) | ((abr & 0x3) << 5) |
                                ((y & 0x100) >> 4) | ((x & 0x3FF) >> 6) |
                                ((y & 0x200) << 2)) &
         0xFFFFu;
}
} // namespace

// x=0,y=0 / x=512,y=0 / x=768,y=384 are the real Crash Bandicoot texture
// page origins (see the SetDrawMode regression comment above). Sweep every
// tp (0-3) and abr (0-3) at each origin against the source-derived formula,
// not the implementation under test.
TEST_F(PsyqGpuTest, GetTPageEncodesRealCrashTexturePages) {
  psyq_register_libgpu_extras();
  struct Origin {
    int x, y;
  };
  const Origin origins[] = {{0, 0}, {512, 0}, {768, 384}};
  for (const Origin &o : origins) {
    for (int tp = 0; tp < 4; ++tp) {
      for (int abr = 0; abr < 4; ++abr) {
        ctx.reset();
        ctx.mem = &mem;
        ctx.r[A0] = static_cast<uint32_t>(tp);
        ctx.r[A1] = static_cast<uint32_t>(abr);
        ctx.r[A2] = static_cast<uint32_t>(o.x);
        ctx.r[A3] = static_cast<uint32_t>(o.y);

        psyq_dispatch("libgpu_GetTPage", &ctx);

        EXPECT_EQ(ctx.r[V0], referenceGetTPage(tp, abr, o.x, o.y))
            << "tp=" << tp << " abr=" << abr << " x=" << o.x
            << " y=" << o.y;
      }
    }
  }
}

// Isolates the two y-derived bits the table sweep above exercises only in
// combination: bit 4 of the result (y's bit 8, in-range VRAM Y) and bit 11
// (y's bit 9, the 2 MB-VRAM term kept per the source, see block comment
// above).
TEST_F(PsyqGpuTest, GetTPageEncodesYBit8AndBit11FromSource) {
  psyq_register_libgpu_extras();
  struct Case {
    int y;
    uint32_t expected;
  };
  const Case cases[] = {
      {0, 0x0000u},
      {256, 0x0010u}, // y bit 8 -> result bit 4
      {512, 0x0800u}, // y bit 9 -> result bit 11 (2 MB VRAM term)
      {768, 0x0810u}, // both bits set together
  };
  for (const Case &c : cases) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[A0] = 0;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = static_cast<uint32_t>(c.y);

    psyq_dispatch("libgpu_GetTPage", &ctx);

    EXPECT_EQ(ctx.r[V0], c.expected) << "y=" << c.y;
  }
}

// PutDrawEnv
//
// PutDrawEnv(env) builds a DR_ENV command block via SetDrawEnv2 (psyz decomp
// src/libgpu/sys.c:362-373, 561-620) and queues it through addque2/cwc; the
// GP0(0xE1) word it produces comes from the same get_mode() helper
// SetDrawMode uses (sys.c:503-507 for SetDrawMode itself, get_mode at
// sys.c:628-631, confirmed applicable to this target in the SetDrawMode
// audit above):
//   (dtd ? 0xE1000200 : 0xE1000000) | (dfe ? 0x400 : 0) | (tpage & 0x9FF)
// DRAWENV's field layout (chrono-cross-decomp/include/psyq/libgpu.h:361-371)
// confirms dfe lives
// at +0x17 (offset 23, right after dtd at +0x16/offset 22) -- both already
// documented in this file's SetDefDrawEnv comment.
//
// Audited 2026-07-28: the pre-audit hle_PutDrawEnv (psyq_hle.cpp) read only
// dtd and masked tpage with 0x7FF (bits 0-10) instead of 0x9FF (bits 0-8 +
// bit 11). Per psx-spx (GP0(E1h), workspace clone
// PS1Recomp-workspace/psx-spx.github.io/docs/graphicsprocessingunitgpu.md:
// 376-388), bits 9-10 of GP0(E1h) are exclusively "Dither" and "Drawing to
// display area allowed" -- supplied by dtd/dfe, never by the raw tpage
// value -- so 0x7FF let stray high bits of env->tpage leak into those two
// flags, and dfe (env->dfe, offset 23) was never read or encoded at all.
// This is the same mask this audit's Task 2 pass already applied to
// SetDrawMode (test_psyq_gpu.cpp above); PutDrawEnv had drifted from it.
//
// GP0(0xE2) Texture Window and GP0(0xE6) Mask Bit Setting: SetDrawEnv2 always
// emits both (sys.c:574, 575/601), and the header comment above already
// claimed GP0(0xE2) -- but the implementation never emitted either. Fixing
// that here would add GP0 words to hle_PutDrawEnv's output, which would
// break test_psyq_hle.cpp's PutDrawEnvEmitsGP0Commands /
// PutDrawEnvBottomRightEncoding (exact `gp0Words.size()==4` assertions) --
// a file outside this task's edit scope. Left as a documented, confirmed gap
// for a follow-up that can touch that file; not fixed in this pass.

TEST_F(PsyqGpuTest, PutDrawEnvMasksTpageAndDropsStrayDtdDfeBitsFromRawValue) {
  const uint32_t env = 0x80100000u;
  // clip/offset arbitrary but valid; the assertion is on the E1 word only.
  writeRect(env, 0, 0, 320, 240);       // clip.x/y/w/h at +0
  mem.write16(env + 8, 0);              // ofs.x
  mem.write16(env + 10, 0);             // ofs.y
  // tpage has every bit 0-10 set, including the two (9,10) that PsyQ
  // reserves exclusively for dtd/dfe -- a real DRAWENV should never carry
  // them, but nothing stops stray bits from surviving if the mask is wrong.
  mem.write16(env + 20, 0x7FFu);        // tpage
  mem.write8(env + 22, 0);              // dtd = 0
  mem.write8(env + 23, 0);              // dfe = 0

  ctx.r[A0] = env;
  hle_PutDrawEnv(&ctx);

  // Task 4b (gap 2) reordered PutDrawEnv's GP0 words to match SetDrawEnv2
  // (sys.c:561-575: E3, E4, E5, E1, ...), so E1 is no longer gp0[0] -- find
  // it by opcode instead of hardcoding a position that gap 2/3 now move.
  auto e1 = std::find_if(gp0.begin(), gp0.end(),
                          [](uint32_t w) { return (w >> 24) == 0xE1u; });
  ASSERT_NE(e1, gp0.end());
  // get_mode(dfe=0, dtd=0, tpage=0x7FF) = 0xE1000000 | (0x7FF & 0x9FF)
  //                                     = 0xE10001FF
  EXPECT_EQ(*e1, 0xE10001FFu)
      << "tpage bits 9-10 (dtd/dfe-reserved) must not leak through unmasked";
}

TEST_F(PsyqGpuTest, PutDrawEnvEncodesDfeBitFromEnv) {
  const uint32_t env = 0x80100000u;
  writeRect(env, 0, 0, 320, 240);
  mem.write16(env + 8, 0);
  mem.write16(env + 10, 0);
  mem.write16(env + 20, 0);   // tpage = 0
  mem.write8(env + 22, 0);    // dtd = 0
  mem.write8(env + 23, 1);    // dfe = 1 (draw-to-display-area enable)

  ctx.r[A0] = env;
  hle_PutDrawEnv(&ctx);

  // See PutDrawEnvMasksTpageAndDropsStrayDtdDfeBitsFromRawValue above: find
  // E1 by opcode, its position moved with the Task 4b gap 2 reorder.
  auto e1 = std::find_if(gp0.begin(), gp0.end(),
                          [](uint32_t w) { return (w >> 24) == 0xE1u; });
  ASSERT_NE(e1, gp0.end());
  // get_mode(dfe=1, dtd=0, tpage=0) = 0xE1000000 | 0x400 = 0xE1000400
  EXPECT_EQ(*e1, 0xE1000400u)
      << "env->dfe (offset 23) must reach GP0(E1) bit 10, per get_mode";
}

// Registry wiring

TEST_F(PsyqGpuTest, RegistryDispatchesAllNewNames) {
  psyq_register_libgpu_extras();
  // Each name must dispatch without aborting. For HLEs that read RAM (rect /
  // src), give them a valid empty rect so no actual GP0 traffic is generated.
  uint32_t rectP = 0x80100400u;
  writeRect(rectP, 0, 0, 0, 0);

  const char *names[] = {
      "libgpu_SetDispMask",      "libgpu_LoadImage",
      "libgpu_StoreImage",       "libgpu_MoveImage",
      "libgpu_ClearImage",       "libgpu_DrawSyncCallback",
      "libetc_VSyncCallback",    "libetc_SetVideoMode",
      "libetc_GetVideoMode",
      "libgs_GsInitGraph",       "libgs_GsDefDispBuff",
      "libgs_GsSetWorkBase",     "libgs_GsSortClear",
  };
  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[A0] = rectP; // safe for both rect-takers and arg-takers
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx))
        << "dispatch failed for: " << n;
  }
}
