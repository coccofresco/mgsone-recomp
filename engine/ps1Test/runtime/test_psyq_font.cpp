// Tests for the libgpu Fnt* HLE family (Group 1.D -- debug-font HLEs).
//
// Strategy:
//   - Each Fnt function is exercised in isolation against a Memory-backed
//     recomp_context, with no Bios/GPU plumbing required (psyq_hle's
//     getConfig().writeGP0 stays null in tests, which is the documented
//     "headless" path that FntFlush tolerates).
//   - The text-buffer accessor `psyq_font_slot_buffer()` lets us assert
//     FntPrint accumulation without poking PS1 RAM.
//   - One end-to-end registry test confirms each canonical name dispatches
//     after `psyq_register_libgpu_font()`.

#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_font.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqFontTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  std::vector<uint32_t> gp0Sink;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    ctx.bios = nullptr;
    psyq_font_reset_for_tests();

    // Capture GP0 writes so FntFlush's optional fillrect can be observed.
    HleConfig cfg{};
    cfg.writeGP0 = [this](uint32_t w) { gp0Sink.push_back(w); };
    ps1::psyq::configure(cfg);

    // Park the stack high in RAM so FntOpen's sp+0x10/sp+0x14 reads land in
    // valid memory.  0x801FFF00 mirrors physical 0x1FFF00 -- the same SP
    // PsyQ bootloaders set at startup.
    ctx.r[SP] = 0x801FFF00u;
  }

  void TearDown() override {
    // Reset HLE config so we don't leak the lambda capture across tests.
    ps1::psyq::configure(HleConfig{});
    psyq_font_reset_for_tests();
  }

  // Helper: stash a NUL-terminated string at `addr` in PS1 RAM.
  void writeString(uint32_t addr, const char *s) {
    while (*s) mem.write8(addr++, static_cast<uint8_t>(*s++));
    mem.write8(addr, 0);
  }
};

} // namespace

// FntOpen

TEST_F(PsyqFontTest, FntOpenReturnsZeroFirstCall) {
  ctx.r[A0] = 32; ctx.r[A1] = 32;
  ctx.r[A2] = 256; ctx.r[A3] = 64;
  mem.write32(ctx.r[SP] + 0x10u, 0); // isbg = 0
  mem.write32(ctx.r[SP] + 0x14u, 64); // n = 64
  hle_libgpu_FntOpen(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqFontTest, FntOpenAllocatesUniqueIdsUntilSlotCap) {
  // 8 slots maximum; 9th call clamps to 7 instead of returning an error.
  for (int i = 0; i < 8; ++i) {
    ctx.r[A0] = 0; ctx.r[A1] = 0;
    ctx.r[A2] = 64; ctx.r[A3] = 16;
    mem.write32(ctx.r[SP] + 0x10u, 0);
    mem.write32(ctx.r[SP] + 0x14u, 16);
    hle_libgpu_FntOpen(&ctx);
    EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>(i));
  }
  // Overflow -> clamped to last slot.
  hle_libgpu_FntOpen(&ctx);
  EXPECT_EQ(ctx.r[V0], 7u);
}

TEST_F(PsyqFontTest, FntOpenStoresIsbgAndCapacity) {
  ctx.r[A0] = 10; ctx.r[A1] = 20;
  ctx.r[A2] = 100; ctx.r[A3] = 30;
  mem.write32(ctx.r[SP] + 0x10u, 1); // isbg
  mem.write32(ctx.r[SP] + 0x14u, 4); // capacity = 4 chars
  hle_libgpu_FntOpen(&ctx);
  uint32_t id = ctx.r[V0];

  // FntPrint a 6-char string; capacity=4 so the buffer should truncate.
  writeString(0x80100000u, "ABCDEF");
  ctx.r[A0] = id;
  ctx.r[A1] = 0x80100000u;
  hle_libgpu_FntPrint(&ctx);
  EXPECT_STREQ(psyq_font_slot_buffer(static_cast<int>(id)), "ABCD");
}

// FntPrint -- formatting

TEST_F(PsyqFontTest, FntPrintAppendsLiteral) {
  // Open with no capacity limit (n=0 -> unlimited in our impl).
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 320; ctx.r[A3] = 30;
  mem.write32(ctx.r[SP] + 0x10u, 0);
  mem.write32(ctx.r[SP] + 0x14u, 0);
  hle_libgpu_FntOpen(&ctx);
  uint32_t id = ctx.r[V0];

  writeString(0x80100000u, "Hello, World!");
  ctx.r[A0] = id;
  ctx.r[A1] = 0x80100000u;
  hle_libgpu_FntPrint(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  EXPECT_STREQ(psyq_font_slot_buffer(static_cast<int>(id)), "Hello, World!");

  // Repeated FntPrint accumulates into the same slot until Flush.
  writeString(0x80100100u, " More");
  ctx.r[A1] = 0x80100100u;
  hle_libgpu_FntPrint(&ctx);
  EXPECT_STREQ(psyq_font_slot_buffer(static_cast<int>(id)),
               "Hello, World! More");
}

TEST_F(PsyqFontTest, FntPrintFormatsIntegersAndStrings) {
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 320; ctx.r[A3] = 30;
  mem.write32(ctx.r[SP] + 0x10u, 0);
  mem.write32(ctx.r[SP] + 0x14u, 0);
  hle_libgpu_FntOpen(&ctx);
  uint32_t id = ctx.r[V0];

  writeString(0x80100000u, "i=%d hex=%x s=%s pct=%%");
  writeString(0x80100200u, "ok"); // %s arg lives in PS1 RAM
  ctx.r[A0] = id;
  ctx.r[A1] = 0x80100000u;
  ctx.r[A2] = static_cast<uint32_t>(-7);   // %d -> -7
  ctx.r[A3] = 0xCAFEu;                     // %x -> cafe
  // 3rd variadic arg lives at sp+0x10 -- but sp+0x10 is also FntOpen's isbg
  // slot, so we have to move SP forward to fresh memory before the call.
  ctx.r[SP] = 0x801FF000u;
  mem.write32(ctx.r[SP] + 0x10u, 0x80100200u); // %s -> "ok"
  hle_libgpu_FntPrint(&ctx);
  EXPECT_STREQ(psyq_font_slot_buffer(static_cast<int>(id)),
               "i=-7 hex=cafe s=ok pct=%");
}

TEST_F(PsyqFontTest, FntPrintRejectsUnopenedSlot) {
  // No FntOpen has run, so slot 3 is unused.  FntPrint must fail cleanly.
  writeString(0x80100000u, "ignored");
  ctx.r[A0] = 3;
  ctx.r[A1] = 0x80100000u;
  hle_libgpu_FntPrint(&ctx);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1);
}

// FntFlush

TEST_F(PsyqFontTest, FntFlushEmitsFillRectWhenIsbgSet) {
  ctx.r[A0] = 16; ctx.r[A1] = 24;
  ctx.r[A2] = 100; ctx.r[A3] = 12;
  mem.write32(ctx.r[SP] + 0x10u, 1); // isbg = 1 -> fill background
  mem.write32(ctx.r[SP] + 0x14u, 32);
  hle_libgpu_FntOpen(&ctx);
  uint32_t id = ctx.r[V0];

  ctx.r[A0] = id;
  hle_libgpu_FntFlush(&ctx);
  EXPECT_EQ(ctx.r[V0], id);

  ASSERT_EQ(gp0Sink.size(), 3u);
  EXPECT_EQ(gp0Sink[0], 0x02000000u);                 // GP0(0x02) FillRect
  EXPECT_EQ(gp0Sink[1], (24u << 16) | 16u);           // y << 16 | x
  EXPECT_EQ(gp0Sink[2], (12u << 16) | 100u);          // h << 16 | w
}

TEST_F(PsyqFontTest, FntFlushNoBackgroundEmitsNothing) {
  ctx.r[A0] = 0; ctx.r[A1] = 0;
  ctx.r[A2] = 32; ctx.r[A3] = 32;
  mem.write32(ctx.r[SP] + 0x10u, 0); // isbg = 0
  mem.write32(ctx.r[SP] + 0x14u, 16);
  hle_libgpu_FntOpen(&ctx);
  uint32_t id = ctx.r[V0];

  ctx.r[A0] = id;
  hle_libgpu_FntFlush(&ctx);
  EXPECT_TRUE(gp0Sink.empty());
}

TEST_F(PsyqFontTest, FntFlushClearsBuffer) {
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 320; ctx.r[A3] = 30;
  mem.write32(ctx.r[SP] + 0x10u, 0);
  mem.write32(ctx.r[SP] + 0x14u, 0);
  hle_libgpu_FntOpen(&ctx);
  uint32_t id = ctx.r[V0];

  writeString(0x80100000u, "foo");
  ctx.r[A0] = id;
  ctx.r[A1] = 0x80100000u;
  hle_libgpu_FntPrint(&ctx);
  EXPECT_STREQ(psyq_font_slot_buffer(static_cast<int>(id)), "foo");

  ctx.r[A0] = id;
  hle_libgpu_FntFlush(&ctx);
  EXPECT_STREQ(psyq_font_slot_buffer(static_cast<int>(id)), "");
}

// FntLoad -- bookkeeping NOP

TEST_F(PsyqFontTest, FntLoadDoesNotCrash) {
  ctx.r[A0] = 960; ctx.r[A1] = 256;
  hle_libgpu_FntLoad(&ctx);
  // No GP0 traffic -- we don't ship a glyph table.
  EXPECT_TRUE(gp0Sink.empty());
}

// FntSystem

TEST_F(PsyqFontTest, FntSystemReturnsPreviousDefault) {
  ctx.r[A0] = 0;
  hle_libgpu_FntSystem(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u); // initial default is 0

  ctx.r[A0] = 3;
  hle_libgpu_FntSystem(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u); // returns prev = 0

  ctx.r[A0] = 5;
  hle_libgpu_FntSystem(&ctx);
  EXPECT_EQ(ctx.r[V0], 3u); // returns prev = 3
}

TEST_F(PsyqFontTest, FntPrintSystemDefaultRoutesToFntSystemSlot) {
  // Open two slots -- we'll redirect FntPrint(-1, ...) to slot 1 via FntSystem.
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 320; ctx.r[A3] = 30;
  mem.write32(ctx.r[SP] + 0x10u, 0);
  mem.write32(ctx.r[SP] + 0x14u, 0);
  hle_libgpu_FntOpen(&ctx); // id 0
  hle_libgpu_FntOpen(&ctx); // id 1

  ctx.r[A0] = 1;
  hle_libgpu_FntSystem(&ctx);

  writeString(0x80100000u, "via default");
  ctx.r[A0] = static_cast<uint32_t>(-1);
  ctx.r[A1] = 0x80100000u;
  hle_libgpu_FntPrint(&ctx);

  EXPECT_STREQ(psyq_font_slot_buffer(0), "");
  EXPECT_STREQ(psyq_font_slot_buffer(1), "via default");
}

// Registry coverage

TEST_F(PsyqFontTest, RegistryDispatchesAllFontNames) {
  psyq_register_libgpu_font();

  // FntOpen first so dependent dispatches have a valid slot to address.
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 64; ctx.r[A3] = 16;
  mem.write32(ctx.r[SP] + 0x10u, 0);
  mem.write32(ctx.r[SP] + 0x14u, 0);
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_FntOpen", &ctx));
  uint32_t id = ctx.r[V0];

  writeString(0x80100000u, "x");
  ctx.r[A0] = id; ctx.r[A1] = 0x80100000u;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_FntPrint", &ctx));

  ctx.r[A0] = id;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_FntFlush", &ctx));

  ctx.r[A0] = 960; ctx.r[A1] = 256;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_FntLoad", &ctx));

  ctx.r[A0] = 0;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_FntSystem", &ctx));
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_SetDumpFnt", &ctx));
}
