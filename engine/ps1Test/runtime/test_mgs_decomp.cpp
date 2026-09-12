#include "runtime/cpu_context.h"
#include "runtime/games/mgs_decomp.h"
#include "runtime/memory.h"
#include "runtime/mips_interpreter.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
using namespace ps1;
using namespace ps1::games::mgs;
namespace {
class MgsDecompTest : public ::testing::Test {
protected:
  Memory memory;
  recomp_context ctx{};
  static constexpr uint32_t output = 0x80170000, stack = 0x801ff000;
  bool existed = false;
  std::string previous;
  void SetUp() override {
    existed = std::getenv("PS1_MGS_DECOMP") != nullptr;
    if (existed)
      previous = std::getenv("PS1_MGS_DECOMP");
    setenv("PS1_MGS_DECOMP", "1", 1);
    configureDecompiled(true);
    ctx.mem = &memory;
    ctx.r29 = stack;
    ctx.r31 = 0x80000000;
  }
  void TearDown() override {
    configureDecompiled(false);
    if (existed)
      setenv("PS1_MGS_DECOMP", previous.c_str(), 1);
    else
      unsetenv("PS1_MGS_DECOMP");
  }
};
} // namespace
TEST_F(MgsDecompTest, SharedSearchWrapsAndPreservesEmptySlotContract) {
  memory.write16(UsaTextureTable + 511 * 12, 511);
  memory.write16(UsaTextureTable, 1023);
  EXPECT_EQ(findSourceTexture(memory, 1023), UsaTextureTable);
  ctx.r4 = 1535;
  ctx.r5 = output;
  ASSERT_TRUE(tryDecompiledCall(&ctx, 0x8001E254));
  EXPECT_EQ(ctx.r2, 0u);
  EXPECT_EQ(memory.read32(output), UsaTextureTable + 12);
  for (unsigned i = 0; i < 512; ++i)
    memory.write16(UsaTextureTable + i * 12, i + 1);
  ctx.r4 = 65000;
  ASSERT_TRUE(tryDecompiledCall(&ctx, 0x8001E254));
  EXPECT_EQ(ctx.r2, 0u);
  EXPECT_EQ(memory.read32(output), 0u);
}
TEST_F(MgsDecompTest, UnsupportedAndRestoredCallsDeclineWithoutMutation) {
  ctx.r4 = 0x1234;
  ctx.r5 = output;
  const std::vector<uint8_t> before(memory.ramPtr(),
                                    memory.ramPtr() + Memory::RAM_SIZE);
  const auto registers = std::to_array(ctx.r);
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E310));
  EXPECT_EQ(registers, std::to_array(ctx.r));
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
  ctx.nativeResumePc = 0x8001E328;
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E310));
  EXPECT_EQ(ctx.nativeResumePc, 0x8001E328u);
  EXPECT_EQ(registers, std::to_array(ctx.r));
  ctx.nativeResumePc = 0;
  ctx.r4 = ~0u;
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E254));
  ctx.r4 = 7;
  ctx.r5 = 0x801fffff;
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E254));
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E378));
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
  setenv("PS1_MGS_DECOMP", "0", 1);
  configureDecompiled(true);
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E2E8));
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
}
TEST_F(MgsDecompTest, CompiledCMatchesUsaInstructionsAcrossTextureLifecycle) {
  const char *path = std::getenv("PS1_TEST_MGS_USA_EXE");
  if (!path)
    GTEST_SKIP() << "optional locally extracted USA EXE";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file);
  const std::vector<uint8_t> exe((std::istreambuf_iterator<char>(file)), {});
  ASSERT_GE(exe.size(), 2048u);
  ASSERT_EQ(std::memcmp(exe.data(), "PS-X EXE", 8), 0);
  const auto word = [&](unsigned p) {
    return uint32_t(exe[p]) | uint32_t(exe[p + 1]) << 8 |
           uint32_t(exe[p + 2]) << 16 | uint32_t(exe[p + 3]) << 24;
  };
  const uint32_t base = word(0x18), size = word(0x1c);
  ASSERT_EQ(base, 0x80010000u);
  ASSERT_LE(uint64_t(size) + 2048, exe.size());
  ASSERT_LE((base & 0x1fffff) + uint64_t(size), Memory::RAM_SIZE);
  std::memcpy(memory.ramPtr() + (base & 0x1fffff), exe.data() + 2048, size);
  ASSERT_EQ(memory.read32(0x8001E254), 0x04810002u);
  Memory oracle;
  std::mt19937 random(0x444743);
  const auto compare = [&](uint32_t entry, bool returnsValue) {
    SCOPED_TRACE(::testing::Message() << std::hex << entry);
    std::memcpy(oracle.ramPtr(), memory.ramPtr(), Memory::RAM_SIZE);
    recomp_context native = ctx;
    native.mem = &oracle;
    const auto before = std::to_array(ctx.r);
    ASSERT_TRUE(tryDecompiledCall(&ctx, entry));
    ASSERT_TRUE(mips::interpretStaticCall(oracle.ramPtr(), &native, entry, base,
                                          base + size));
    if (returnsValue)
      EXPECT_EQ(ctx.r2, native.r2);
    // Stack spills and temporary registers are not C ABI outputs. Compare
    // all persistent RAM and all callee-preserved registers instead.
    const uint32_t scratch = (stack & 0x1fffff) - 64;
    EXPECT_EQ(std::memcmp(memory.ramPtr(), oracle.ramPtr(), scratch), 0);
    EXPECT_EQ(std::memcmp(memory.ramPtr() + (stack & 0x1fffff),
                          oracle.ramPtr() + (stack & 0x1fffff),
                          Memory::RAM_SIZE - (stack & 0x1fffff)),
              0);
    for (unsigned reg :
         {0u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 28u, 29u, 30u, 31u}) {
      EXPECT_EQ(ctx.r[reg], before[reg]);
      EXPECT_EQ(ctx.r[reg], native.r[reg]);
    }
  };
  for (unsigned i = 0; i < 512 * 12; ++i)
    memory.write8(UsaTextureTable + i, random());
  compare(0x8001E2E8, false);
  for (unsigned n = 0; n < 64; ++n) {
    const uint16_t id = 511 + n * 512;
    ctx.r4 = id;
    ctx.r5 = n & 1;
    ctx.r6 = n & 3;
    ctx.r7 = output + 16;
    memory.write32(stack + 16, output + 32);
    memory.write32(stack + 20, ctx.r5 ? 256 : 16);
    memory.write16(output + 16, (random() % 16) * 64 + random() % 32);
    memory.write16(output + 18, random() % 512);
    memory.write16(output + 20, 1 + random() % 64);
    memory.write16(output + 22, 1 + random() % 256);
    memory.write16(output + 32, (random() % 64) * 16);
    memory.write16(output + 34, random() % 512);
    compare(0x8001E378, false);
    ctx.r4 = id;
    ctx.r5 = output;
    compare(0x8001E254, true);
    compare(0x8001E310, true);
    EXPECT_EQ(ctx.r2, findSourceTexture(memory, id));
  }
  ctx.r4 = 511;
  ctx.r5 = 0;
  ctx.r6 = 0;
  ctx.r7 = output + 16;
  EXPECT_FALSE(tryDecompiledCall(&ctx, 0x8001E378));
  memory.write8(findSourceTexture(memory, 511) + 2, 1);
  compare(0x8001E378, false);
}
