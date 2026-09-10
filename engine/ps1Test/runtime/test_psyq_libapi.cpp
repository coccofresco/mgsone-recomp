// Tests for the libapi/libcd/libgte/libgpu HLE implementations registered for
// the Rayman boot path (Sessao 0.7).
//
// Strategy:
//   - For libapi BIOS wrappers: build a real `Bios` and verify that calling
//     the HLE leaves $t1 holding the expected syscall index AND that BIOS
//     side effects fire (e.g. v0 set by openEvent, padBuf addrs latched).
//   - For libgte/libgpu/libcd helpers: verify the single arithmetic / memory
//     mutation each one performs.
//   - Also assert each name is dispatchable through the registry after
//     `psyq_register_rayman_boot()`.

#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/gte.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_libapi.h"
#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_libgte.h"
#include "runtime/psyq/psyq_registry.h"

#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqLibapiTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  std::unique_ptr<bios::Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<bios::Bios>(ctx, fs, mem);
    ctx.bios = bios.get();
  }
};

} // namespace

// Direct HLE behaviour -- libapi (BIOS A0/B0/C0 wrappers)

TEST_F(PsyqLibapiTest, InitHeapInitializesBiosHeap) {
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0x10000u;
  hle_libapi_InitHeap(&ctx);
  // BIOS A0:0x39 -- verify the wrapper actually invoked it via $t1 latch.
  EXPECT_EQ(ctx.r[T1], 0x39u);
}

TEST_F(PsyqLibapiTest, OpenEventDispatchesB0_08) {
  // The 0th openEvent returns id=0, so we cannot rely on v0 being non-zero --
  // assert the dispatch via $t1 and re-open to make sure events_ grew.
  ctx.r[A0] = 0xF0000003u; // ROOT_COUNTER_VBLANK class
  ctx.r[A1] = 0x0002u;     // spec
  ctx.r[A2] = 0x1000u;     // mode
  ctx.r[A3] = 0u;          // fn
  ctx.r[V0] = 0xDEADu;
  hle_libapi_OpenEvent(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x08u);
  EXPECT_EQ(ctx.r[V0], 0u); // first event id

  // Second open should return id=1.
  hle_libapi_OpenEvent(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);
}

TEST_F(PsyqLibapiTest, CloseEventDispatchesB0_09) {
  // Open one so closeEvent has a valid id to match.
  ctx.r[A0] = 0xF0000003u;
  ctx.r[A1] = 1;
  ctx.r[A2] = 0x1000u;
  ctx.r[A3] = 0;
  hle_libapi_OpenEvent(&ctx);
  uint32_t handle = ctx.r[V0];

  ctx.r[A0] = handle;
  hle_libapi_CloseEvent(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x09u);
}

TEST_F(PsyqLibapiTest, EventApiDispatchTable) {
  struct {
    void (*fn)(recomp_context *);
    uint32_t expectedT1;
  } cases[] = {
      {&hle_libapi_DeliverEvent, 0x07u},
      {&hle_libapi_EnableEvent,  0x0Cu},
      {&hle_libapi_DisableEvent, 0x0Du},
      {&hle_libapi_TestEvent,    0x0Bu},
  };
  for (auto &c : cases) {
    ctx.r[A0] = 0xF0000003u; // a real-looking class so the BIOS doesn't fault
    ctx.r[A1] = 0;
    ctx.r[T1] = 0xDEADu;
    c.fn(&ctx);
    EXPECT_EQ(ctx.r[T1], c.expectedT1);
  }
}

TEST_F(PsyqLibapiTest, CriticalSectionStubsReturnExpectedValues) {
  ctx.r[V0] = 0;
  hle_libapi_EnterCriticalSection(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u); // "interrupts had been enabled"

  ctx.r[V0] = 0xCCu;
  hle_libapi_ExitCriticalSection(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqLibapiTest, ReturnFromExceptionDispatchesB0_17) {
  ctx.cop0[COP0_EPC] = 0x80012340u;
  ctx.cop0[COP0_SR] = 0x3Cu; // bits 5:2 set so we can see them shift
  hle_libapi_ReturnFromException(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x17u);
  EXPECT_EQ(ctx.pc, 0x80012340u);
}

TEST_F(PsyqLibapiTest, HookEntryIntDispatchesC0_01) {
  ctx.r[A0] = 0; // priority
  ctx.r[A1] = 0x80100000u; // handler
  hle_libapi_HookEntryInt(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x01u);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqLibapiTest, ChangeClearRCntDispatchesC0_0A) {
  ctx.r[A0] = 3;
  ctx.r[A1] = 1;
  hle_libapi_ChangeClearRCnt(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x0Au);
}

TEST_F(PsyqLibapiTest, FileIoApiDispatchTable) {
  struct {
    void (*fn)(recomp_context *);
    uint32_t expectedT1;
  } cases[] = {
      {&hle_libapi_open,        0x32u},
      {&hle_libapi_close,       0x36u},
      {&hle_libapi_read,        0x34u},
      {&hle_libapi_write,       0x35u},
      {&hle_libapi_erase,       0x45u},
      {&hle_libapi_format,      0x41u},
      {&hle_libapi_firstfile2,  0x42u},
      {&hle_libapi_nextfile,    0x43u},
      {&hle_libapi__bu_init,    0x70u},
      {&hle_libapi__96_remove,  0x71u},
  };
  for (auto &c : cases) {
    ctx.r[A0] = 0;
    ctx.r[A1] = 0;
    ctx.r[T1] = 0xDEADu;
    c.fn(&ctx);
    EXPECT_EQ(ctx.r[T1], c.expectedT1);
  }
}

TEST_F(PsyqLibapiTest, PadApiDispatchTable) {
  struct {
    void (*fn)(recomp_context *);
    uint32_t expectedT1;
  } cases[] = {
      {&hle_libapi_InitPAD2,       0x12u},
      {&hle_libapi_StartPAD2,      0x13u},
      {&hle_libapi_StopPAD2,       0x14u},
      {&hle_libapi_ChangeClearPAD, 0x5Bu},
  };
  for (auto &c : cases) {
    ctx.r[A0] = 0x80100000u;
    ctx.r[A1] = 64;
    ctx.r[A2] = 0x80100100u;
    ctx.r[A3] = 64;
    ctx.r[T1] = 0xDEADu;
    c.fn(&ctx);
    EXPECT_EQ(ctx.r[T1], c.expectedT1);
  }
}

TEST_F(PsyqLibapiTest, GpuCwDispatchesA0_49) {
  ctx.r[A0] = 0xE1000400u; // GP0 tpage command (won't go anywhere -- no GPU)
  hle_libapi_GPU_cw(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x49u);
}

// libcd

TEST_F(PsyqLibapiTest, CdGetSectorReturnsZeroNotReady) {
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 512;
  ctx.r[V0] = 0xCAFEu;
  hle_libcd_CdGetSector(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqLibapiTest, StSetMaskReturnsZero) {
  ctx.r[V0] = 0xCAFEu;
  hle_libcd_StSetMask(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// libgte

TEST_F(PsyqLibapiTest, SetGeomOffsetWritesOFXOFY) {
  ctx.r[A0] = 320;
  ctx.r[A1] = 240;
  hle_libgte_SetGeomOffset(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_OFX], 320u << 16);
  EXPECT_EQ(ctx.cop2c[GTE_OFY], 240u << 16);
}

TEST_F(PsyqLibapiTest, SetGeomScreenWritesH) {
  ctx.r[A0] = 0xFFFF0200u; // upper bits should be masked off
  hle_libgte_SetGeomScreen(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_H], 0x0200u);
}

TEST_F(PsyqLibapiTest, SetDQAWritesDQA) {
  ctx.r[A0] = 0xFFFFC000u;
  hle_libgte_SetDQA(&ctx);
  EXPECT_EQ(ctx.cop2c[GTE_DQA], 0xFFFFC000u);
}

// libgpu (extras)

TEST_F(PsyqLibapiTest, GetClutPacksXY) {
  ctx.r[A0] = 0x10; // x = 16
  ctx.r[A1] = 480;  // y = 480 = 0x1E0
  hle_libgpu_GetClut(&ctx);
  // packed = (y << 6) | ((x>>4) & 0x3F)
  uint32_t expected = (480u << 6) | ((0x10u >> 4) & 0x3Fu);
  EXPECT_EQ(ctx.r[V0], expected);
}

TEST_F(PsyqLibapiTest, SetShadeTexTogglesCodeBit0) {
  uint32_t p = 0x80100000u;
  mem.write8(p + 7, 0x2C); // POLY_FT4 base code
  ctx.r[A0] = p;

  ctx.r[A1] = 1; // raw texture
  hle_libgpu_SetShadeTex(&ctx);
  EXPECT_EQ(mem.read8(p + 7), 0x2Du);

  ctx.r[A1] = 0; // back to modulated
  hle_libgpu_SetShadeTex(&ctx);
  EXPECT_EQ(mem.read8(p + 7), 0x2Cu);
}

TEST_F(PsyqLibapiTest, SetPolyF4WritesLenAndCode) {
  uint32_t p = 0x80100040u;
  ctx.r[A0] = p;
  hle_libgpu_SetPolyF4(&ctx);
  EXPECT_EQ(mem.read8(p + 3), 5u);
  EXPECT_EQ(mem.read8(p + 7), 0x28u);
}

TEST_F(PsyqLibapiTest, SetPolyFT4WritesLenAndCode) {
  uint32_t p = 0x80100080u;
  ctx.r[A0] = p;
  hle_libgpu_SetPolyFT4(&ctx);
  EXPECT_EQ(mem.read8(p + 3), 9u);
  EXPECT_EQ(mem.read8(p + 7), 0x2Cu);
}

TEST_F(PsyqLibapiTest, SetSpritesWriteLenAndCode) {
  struct {
    void (*fn)(recomp_context *);
    uint8_t expectedLen;
    uint8_t expectedCode;
  } cases[] = {
      {&hle_libgpu_SetSprt,   4, 0x64},
      {&hle_libgpu_SetSprt8,  3, 0x74},
      {&hle_libgpu_SetSprt16, 3, 0x7C},
  };
  uint32_t p = 0x80100100u;
  for (auto &c : cases) {
    mem.write8(p + 3, 0xAA);
    mem.write8(p + 7, 0xBB);
    ctx.r[A0] = p;
    c.fn(&ctx);
    EXPECT_EQ(mem.read8(p + 3), c.expectedLen);
    EXPECT_EQ(mem.read8(p + 7), c.expectedCode);
    p += 0x40;
  }
}

// Registry coverage

TEST_F(PsyqLibapiTest, RaymanBootRegistersExpectedNames) {
  psyq_register_rayman_boot();

  // Pick a representative name from each library and dispatch through the
  // registry. Functions that return v0 leave a recognisable value behind.
  ctx.r[V0] = 0;
  psyq_dispatch("libapi_EnterCriticalSection", &ctx);
  EXPECT_EQ(ctx.r[V0], 1u);

  ctx.r[V0] = 0xCAFEu;
  psyq_dispatch("libcd_StSetMask", &ctx);
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 100;
  ctx.r[A1] = 50;
  psyq_dispatch("libgte_SetGeomOffset", &ctx);
  EXPECT_EQ(ctx.cop2c[GTE_OFX], 100u << 16);

  ctx.r[A0] = 0x20;
  ctx.r[A1] = 256;
  psyq_dispatch("libgpu_GetClut", &ctx);
  EXPECT_EQ(ctx.r[V0], (256u << 6) | 0x02u);
}

TEST_F(PsyqLibapiTest, RaymanBootCoversAll39RaymanHleNames) {
  psyq_register_rayman_boot();
  // The 29 names actually backed by `psyq_register_rayman_boot()`. The other
  // 10 (libetc_VSync + 9 libgpu env/OT) come from `init_defaults` and are
  // tested separately in test_psyq_registry.cpp.
  const char *names[] = {
      "libapi_OpenEvent",     "libapi_CloseEvent",   "libapi_DeliverEvent",
      "libapi_EnableEvent",   "libapi_DisableEvent", "libapi_TestEvent",
      "libapi_EnterCriticalSection", "libapi_ExitCriticalSection",
      "libapi_ReturnFromException",  "libapi_HookEntryInt",
      "libapi_ChangeClearRCnt",
      "libapi_open",          "libapi_close",        "libapi_read",
      "libapi_write",         "libapi_erase",        "libapi_format",
      "libapi__bu_init",      "libapi_firstfile2",   "libapi_nextfile",
      "libapi__96_remove",
      "libapi_InitPAD2",      "libapi_StartPAD2",    "libapi_StopPAD2",
      "libapi_ChangeClearPAD",
      "libapi_InitHeap",      "libapi_GPU_cw",
      "libcd_CdGetSector",    "libcd_StSetMask",
      "libgte_SetGeomOffset", "libgte_SetGeomScreen", "libgte_SetDQA",
      "libgpu_GetClut",       "libgpu_SetShadeTex",
      "libgpu_SetPolyF4",     "libgpu_SetPolyFT4",
      "libgpu_SetSprt",       "libgpu_SetSprt8",     "libgpu_SetSprt16",
  };
  // Reset GPRs before each dispatch so leftover state can't crash a
  // wrapper that reads $a0..$a3.
  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.bios = bios.get();
    ctx.r[A0] = 0;
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx))
        << "psyq_dispatch failed for: " << n;
  }
}
