// Tests for the libc HLE family (Group 1.F -- memory/string/printf subset).
//
// Strategy:
//   - For BIOS-delegating HLEs (memcpy/memset/.../strcpy/.../abs/rand): build
//     a real Bios + recomp_context and assert the syscall fired by checking
//     `$t1` (latched index) AND the side effect in PS1 RAM or `$v0`.
//     Same shape as test_psyq_libapi.cpp.
//   - For standalone HLEs (atoi/printf/sprintf): drive directly and check
//     `$v0` and the produced PS1-RAM bytes.  printf's stdout output is
//     redirected via freopen() so the gate behaviour can be observed in a
//     headless test.
//   - One end-to-end registry test confirms `libc_<name>` AND a sample of
//     `lib<X>_<name>` aliases dispatch after `psyq_register_libc()`.

#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_libc.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstring>
#include <gtest/gtest.h>
#include <memory>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqLibcTest : public ::testing::Test {
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
    // Park SP somewhere safe so printf/sprintf stack-arg fetches don't
    // tread on test scratch buffers.
    ctx.r[SP] = 0x801FFF00u;
    psyq_libc_reset_for_tests();
  }

  void writeString(uint32_t addr, const char *s) {
    while (*s) mem.write8(addr++, static_cast<uint8_t>(*s++));
    mem.write8(addr, 0);
  }

  std::string readString(uint32_t addr, size_t cap = 256) {
    std::string s;
    for (size_t i = 0; i < cap; ++i) {
      uint8_t b = mem.read8(addr + static_cast<uint32_t>(i));
      if (b == 0) break;
      s.push_back(static_cast<char>(b));
    }
    return s;
  }
};

} // namespace

// Memory routines -- delegate to bios A0:0x2A..0x2D

TEST_F(PsyqLibcTest, MemcpyDispatchesA0_2A) {
  writeString(0x80100000u, "abcdef");
  ctx.r[A0] = 0x80100100u;
  ctx.r[A1] = 0x80100000u;
  ctx.r[A2] = 6;
  hle_libc_memcpy(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x2Au);
  EXPECT_EQ(ctx.r[V0], 0x80100100u); // returns dst
  EXPECT_EQ(readString(0x80100100u), "abcdef");
}

TEST_F(PsyqLibcTest, MemsetDispatchesA0_2B) {
  ctx.r[A0] = 0x80100100u;
  ctx.r[A1] = 0xAB;
  ctx.r[A2] = 4;
  hle_libc_memset(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x2Bu);
  for (uint32_t i = 0; i < 4; ++i)
    EXPECT_EQ(mem.read8(0x80100100u + i), 0xAB);
}

TEST_F(PsyqLibcTest, MemmoveDispatchesA0_2C) {
  // Overlapping forward-shift: dst > src, so naive memcpy would corrupt.
  writeString(0x80100000u, "ABCDEF");
  ctx.r[A0] = 0x80100002u;
  ctx.r[A1] = 0x80100000u;
  ctx.r[A2] = 4;
  hle_libc_memmove(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x2Cu);
  EXPECT_EQ(readString(0x80100000u), "ABABCD");
}

TEST_F(PsyqLibcTest, MemcmpDispatchesA0_2D) {
  writeString(0x80100000u, "abcd");
  writeString(0x80100100u, "abce");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0x80100100u;
  ctx.r[A2] = 4;
  hle_libc_memcmp(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x2Du);
  EXPECT_NE(ctx.r[V0], 0u); // strings differ at byte 3
}

// String routines -- delegate to bios A0:0x15..0x1B (psx-spx/openbios layout)

TEST_F(PsyqLibcTest, StrcatDispatchesA0_15) {
  writeString(0x80100000u, "foo");
  writeString(0x80100100u, "bar");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0x80100100u;
  hle_libc_strcat(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x15u);
  EXPECT_EQ(readString(0x80100000u), "foobar");
}

TEST_F(PsyqLibcTest, StrcmpDispatchesA0_17) {
  writeString(0x80100000u, "abc");
  writeString(0x80100100u, "abc");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0x80100100u;
  hle_libc_strcmp(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x17u);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqLibcTest, StrncmpDispatchesA0_18) {
  writeString(0x80100000u, "abcdef");
  writeString(0x80100100u, "abcXYZ");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0x80100100u;
  ctx.r[A2] = 3;
  hle_libc_strncmp(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x18u);
  EXPECT_EQ(ctx.r[V0], 0u); // first 3 bytes equal
}

TEST_F(PsyqLibcTest, StrcpyDispatchesA0_19) {
  writeString(0x80100000u, "hello");
  ctx.r[A0] = 0x80100100u;
  ctx.r[A1] = 0x80100000u;
  hle_libc_strcpy(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x19u);
  EXPECT_EQ(readString(0x80100100u), "hello");
}

TEST_F(PsyqLibcTest, StrncpyDispatchesA0_1A) {
  writeString(0x80100000u, "hello");
  ctx.r[A0] = 0x80100100u;
  ctx.r[A1] = 0x80100000u;
  ctx.r[A2] = 3;
  hle_libc_strncpy(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x1Au);
  EXPECT_EQ(mem.read8(0x80100100u), 'h');
  EXPECT_EQ(mem.read8(0x80100101u), 'e');
  EXPECT_EQ(mem.read8(0x80100102u), 'l');
}

TEST_F(PsyqLibcTest, StrlenDispatchesA0_1B) {
  writeString(0x80100000u, "hello world");
  ctx.r[A0] = 0x80100000u;
  hle_libc_strlen(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x1Bu);
  EXPECT_EQ(ctx.r[V0], 11u);
}

// Math / RNG -- delegate to bios A0:0x10/0x11/0x1E/0x1F

TEST_F(PsyqLibcTest, AbsDispatchesA0_10) {
  ctx.r[A0] = static_cast<uint32_t>(-42);
  hle_libc_abs(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x10u);
  EXPECT_EQ(ctx.r[V0], 42u);
}

TEST_F(PsyqLibcTest, RandIsDeterministicAfterSrand) {
  ctx.r[A0] = 12345u;
  hle_libc_srand(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x30u);

  hle_libc_rand(&ctx);
  EXPECT_EQ(ctx.r[T1], 0x2Fu);
  uint32_t r1 = ctx.r[V0];

  // Re-seed and re-roll -- same seed must produce the same first sample.
  ctx.r[A0] = 12345u;
  hle_libc_srand(&ctx);
  hle_libc_rand(&ctx);
  EXPECT_EQ(ctx.r[V0], r1);
}

// atoi -- standalone parse

TEST_F(PsyqLibcTest, AtoiParsesDecimal) {
  writeString(0x80100000u, "42");
  ctx.r[A0] = 0x80100000u;
  hle_libc_atoi(&ctx);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), 42);
}

TEST_F(PsyqLibcTest, AtoiParsesNegativeWithLeadingWhitespace) {
  writeString(0x80100000u, "   -1234trailing");
  ctx.r[A0] = 0x80100000u;
  hle_libc_atoi(&ctx);
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), -1234);
}

TEST_F(PsyqLibcTest, AtoiReturnsZeroForGarbage) {
  writeString(0x80100000u, "abc");
  ctx.r[A0] = 0x80100000u;
  hle_libc_atoi(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqLibcTest, AtoiReturnsZeroForNullPointer) {
  ctx.r[A0] = 0;
  hle_libc_atoi(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// printf -- standalone, gated on PS1_BIOS_DEBUG

TEST_F(PsyqLibcTest, PrintfReturnsCharCountEvenWhenSilent) {
  // PS1_BIOS_DEBUG is unset in the gtest harness by default, so stdout
  // stays quiet.  We still want a meaningful character count back.
  writeString(0x80100000u, "x=%d y=%d");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 7;
  ctx.r[A2] = static_cast<uint32_t>(-3);
  hle_libc_printf(&ctx);
  // "x=7 y=-3" -- 8 characters.
  EXPECT_EQ(ctx.r[V0], 8u);
}

TEST_F(PsyqLibcTest, PrintfHandlesPercentS) {
  writeString(0x80100000u, "hello %s!");
  writeString(0x80100020u, "world");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0x80100020u;
  hle_libc_printf(&ctx);
  // "hello world!" -- 12 chars.
  EXPECT_EQ(ctx.r[V0], 12u);
}

TEST_F(PsyqLibcTest, PrintfTreatsNullStringAsLiteralNull) {
  writeString(0x80100000u, "v=%s");
  ctx.r[A0] = 0x80100000u;
  ctx.r[A1] = 0; // NULL pointer -> "(null)"
  hle_libc_printf(&ctx);
  EXPECT_EQ(ctx.r[V0], 8u); // "v=(null)" -- 8 chars
}

// sprintf -- writes formatted result to PS1 RAM

TEST_F(PsyqLibcTest, SprintfWritesIntoBuffer) {
  writeString(0x80100020u, "x=%d hex=%x");
  ctx.r[A0] = 0x80100100u; // dst buffer
  ctx.r[A1] = 0x80100020u; // fmt
  ctx.r[A2] = 7;
  ctx.r[A3] = 0xCAFEu;
  hle_libc_sprintf(&ctx);
  EXPECT_EQ(readString(0x80100100u), "x=7 hex=cafe");
  EXPECT_EQ(ctx.r[V0], 12u);
  // Trailing NUL must be present.
  EXPECT_EQ(mem.read8(0x80100100u + 12u), 0u);
}

TEST_F(PsyqLibcTest, SprintfHandlesNullBuffer) {
  // libc tolerates buf=NULL (no write); we just want the count back.
  writeString(0x80100020u, "n=%d");
  ctx.r[A0] = 0;
  ctx.r[A1] = 0x80100020u;
  ctx.r[A2] = 5;
  hle_libc_sprintf(&ctx);
  EXPECT_EQ(ctx.r[V0], 3u); // "n=5" -- 3 chars
}

TEST_F(PsyqLibcTest, SprintfPercentPercentPassthrough) {
  writeString(0x80100020u, "100%%");
  ctx.r[A0] = 0x80100100u;
  ctx.r[A1] = 0x80100020u;
  hle_libc_sprintf(&ctx);
  EXPECT_EQ(readString(0x80100100u), "100%");
}

// Registry coverage -- canonical + aliased prefixes

TEST_F(PsyqLibcTest, RegistryDispatchesCanonicalAndAliases) {
  psyq_register_libc();

  // Canonical (libc_) -- must be defined for every entry.
  ctx.r[A0] = 0x80100100u;
  ctx.r[A1] = 0x55;
  ctx.r[A2] = 1;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libc_memset", &ctx));
  EXPECT_EQ(mem.read8(0x80100100u), 0x55);

  // libgpu_memset -- the alias actually emitted in Crash's recompiled_out.
  ctx.r[A0] = 0x80100101u;
  ctx.r[A1] = 0xAA;
  ctx.r[A2] = 1;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libgpu_memset", &ctx));
  EXPECT_EQ(mem.read8(0x80100101u), 0xAA);

  // libcd_memcpy -- the alias actually emitted by the matcher.
  writeString(0x80100200u, "yo");
  ctx.r[A0] = 0x80100210u;
  ctx.r[A1] = 0x80100200u;
  ctx.r[A2] = 3;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libcd_memcpy", &ctx));
  EXPECT_EQ(readString(0x80100210u), "yo");

  // libapi__memmove -- underscore-prefixed PsyQ alias.
  writeString(0x80100300u, "abc");
  ctx.r[A0] = 0x80100310u;
  ctx.r[A1] = 0x80100300u;
  ctx.r[A2] = 3;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libapi__memmove", &ctx));
  EXPECT_EQ(readString(0x80100310u), "abc");

  // libc_atoi standalone path.
  writeString(0x80100400u, "99");
  ctx.r[A0] = 0x80100400u;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libc_atoi", &ctx));
  EXPECT_EQ(static_cast<int32_t>(ctx.r[V0]), 99);
}
