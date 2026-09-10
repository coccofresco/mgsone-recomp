#include "runtime/cpu_context.h"
#include "runtime/games/crash3_scenery_construction.h"
#include "runtime/gte.h"
#include "runtime/gte_color_math.h"
#include "runtime/memory.h"
#include "runtime/mips_interpreter.h"

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

namespace {
using namespace ps1::games::crash3;
uint32_t packedXy(const std::array<int16_t, 3> &v) {
  return static_cast<uint16_t>(v[0]) |
         (static_cast<uint32_t>(static_cast<uint16_t>(v[1])) << 16u);
}
uint32_t packedRgb(const std::array<float, 3> &v) {
  return static_cast<uint32_t>(v[0]) | (static_cast<uint32_t>(v[1]) << 8u) |
         (static_cast<uint32_t>(v[2]) << 16u);
}
SceneryConstructionState state() {
  SceneryConstructionState s;
  s.twoPointLighting = true;
  s.points = {{{0, 0}, {0, 9000}}};
  return s;
}
} // namespace

TEST(GteColorMath, PureDistanceCueMatchesArchitecturalGte) {
  std::mt19937 random(421);
  ps1::CPUContext ctx{};
  for (uint32_t sample = 0; sample < 20000; ++sample) {
    const uint32_t rgb = random() & 0xffffffu;
    const std::array<float, 3> color{float(rgb & 255u),
                                     float((rgb >> 8u) & 255u),
                                     float((rgb >> 16u) & 255u)};
    std::array<int32_t, 3> far{};
    for (uint32_t channel = 0; channel < 3; ++channel) {
      far[channel] = static_cast<int16_t>(random());
      ctx.cop2c[ps1::GTE_RFC + channel] = far[channel];
    }
    const int16_t factor = static_cast<int16_t>(random());
    ctx.cop2d[ps1::GTE_IR0] = factor;
    ctx.cop2d[ps1::GTE_RGBC] = rgb;
    ps1::GTE::DPCS(&ctx, true, false);
    EXPECT_EQ(packedRgb(ps1::gte_math::distanceCueColor(color, far, factor)),
              ctx.cop2d[ps1::GTE_RGB2] & 0xffffffu);
  }
}

TEST(Crash3SceneryConstruction,
     ActualPairedProgramsNotProjectionSelectLighting) {
  EXPECT_TRUE(sceneryUsesTwoPointLighting(0x8004305c, 0x800430d4));
  EXPECT_FALSE(sceneryUsesTwoPointLighting(0x800417ac, 0x80041a28));
  EXPECT_FALSE(sceneryUsesTwoPointLighting(0x8004305c, 0x80041ba0));
}

TEST(Crash3SceneryConstruction, FlagsAndDelaySlotDefineAmbientAndPassThrough) {
  const auto s = state();
  SceneryConstructionVertex v{0, {0, 0, 4000}, {93, 48, 64}};
  const auto unflagged = selectSceneryPrimitiveLighting(s, {&v, 1});
  EXPECT_TRUE(unflagged.flatBlack);
  v.rawXy = 1u << 19u;
  const auto ambient = selectSceneryPrimitiveLighting(s, {&v, 1});
  EXPECT_FALSE(ambient.flatBlack);
  EXPECT_EQ(shadeSceneryConstructionVertex(s, ambient, v),
            (std::array<float, 3>{27, 14, 19}));
  v.rawXy = 1u << 18u;
  EXPECT_EQ(shadeSceneryConstructionVertex(s, ambient, v), v.color255);
  v.rawXy = 3u << 18u;
  EXPECT_EQ(shadeSceneryConstructionVertex(s, ambient, v),
            (std::array<float, 3>{27, 14, 19}));
}

TEST(Crash3SceneryConstruction, FirstCornerSelectsProgramNotEveryCorner) {
  const auto s = state();
  std::array<SceneryConstructionVertex, 3> v{
      {{0, {0, 0, 3000}, {128, 128, 128}},
       {0, {0, 0, 0}, {128, 128, 128}},
       {0, {0, 0, 9000}, {128, 128, 128}}}};
  EXPECT_TRUE(selectSceneryPrimitiveLighting(s, v).flatBlack);
  std::swap(v[0], v[1]);
  const auto selected = selectSceneryPrimitiveLighting(s, v);
  EXPECT_EQ(selected.pointMask, 1);
  EXPECT_FALSE(selected.flatBlack);
  EXPECT_EQ(shadeSceneryConstructionVertex(s, selected, v[0]), v[0].color255);
  EXPECT_EQ(shadeSceneryConstructionVertex(s, selected, v[2]),
            (std::array<float, 3>{0, 0, 0}));
}

TEST(Crash3SceneryConstruction, PackedBorrowAndBroadPhaseBoundaryAreExact) {
  auto s = state();
  s.points[0].packedXy = 1;
  SceneryConstructionVertex v{0, {0, 0, 2098}, {128, 128, 128}};
  EXPECT_EQ(selectSceneryPrimitiveLighting(s, {&v, 1}).pointMask, 1);
  v.model[2] = 2099;
  EXPECT_EQ(selectSceneryPrimitiveLighting(s, {&v, 1}).pointMask, 0);
}

TEST(Crash3SceneryConstruction, LocalDonorMipsDifferentialOptIn) {
  const char *path = std::getenv("PS1_CRASH3_CONSTRUCTOR_ORACLE_RAM");
  if (path == nullptr)
    GTEST_SKIP() << "Provide a local 2 MiB SwanStation RAM dump; no game bytes "
                    "in test data";
  ps1::Memory memory;
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.read(reinterpret_cast<char *>(memory.ramPtr()), 0x200000));
  ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
  ASSERT_EQ(memory.read32(0x8004305cu), 0xac6100b0u);
  ASSERT_EQ(memory.read32(0x800430d4u), 0xac6100b0u);
  // Harness exit stubs OUTSIDE the constructor under test. Conditional tail
  // branches otherwise continue through the ordinary packet assembler.
  for (uint32_t exit : {0x800418ccu, 0x80041ba0u}) {
    memory.write32(exit, 0x03400008u); // jr k0
    memory.write32(exit + 4u, 0u);
  }
  std::mt19937 random(0x4305c);
  constexpr uint32_t scratch = 0x1f800000u, packet = 0x80180000u;
  std::array<uint32_t, 4> maskCounts{};
  uint32_t flatCount = 0;
  for (uint32_t sample = 0; sample < 2000; ++sample) {
    const uint32_t count = sample % 2 ? 4u : 3u;
    recomp_context ctx{};
    ctx.mem = &memory;
    auto s = state();
    for (uint32_t point = 0; point < 2; ++point) {
      const std::array<int16_t, 3> position{
          int16_t(int(random() % 4000) - 2000),
          int16_t(int(random() % 4000) - 2000),
          int16_t(int(random() % 4000) - 2000)};
      s.points[point] = {packedXy(position), position[2]};
      memory.write32(scratch + 124u + point * 12u, s.points[point].packedXy);
      memory.write32(scratch + 128u + point * 12u, s.points[point].z);
    }
    std::array<SceneryConstructionVertex, 4> corners{};
    for (uint32_t corner = 0; corner < count; ++corner) {
      auto &v = corners[corner];
      v.rawXy = (random() & 3u) << 18u;
      v.model = {int16_t(int(random() % 6000) - 3000),
                 int16_t(int(random() % 6000) - 3000),
                 int16_t(int(random() % 6000) - 3000)};
      v.color255 = {float(random() & 255u), float(random() & 255u),
                    float(random() & 255u)};
      ctx.r[corner == 0 ? 1 : corner == 1 ? 7 : corner == 2 ? 8 : 4] = v.rawXy;
      ctx.r[corner == 3 ? 5 : 13 + corner] = packedRgb(v.color255);
      if (count == 4 && corner == 0) {
        memory.write32(scratch + 436u, packedXy(v.model));
        memory.write32(scratch + 440u, static_cast<int32_t>(v.model[2]));
      } else {
        const uint32_t slot = corner == 3 ? 0u : corner;
        ctx.cop2d[slot * 2] = packedXy(v.model);
        ctx.cop2d[slot * 2 + 1] = static_cast<int32_t>(v.model[2]);
      }
    }
    ctx.r[3] = scratch;
    ctx.r[20] = scratch;
    ctx.r[23] = packet;
    ctx.r[31] = ctx.r[26] = 0x80001234u;
    memory.write32(scratch + 112u, ctx.r[31]);
    memory.write32(packet + 4u, 0xdeadbeefu);
    const auto selection =
        selectSceneryPrimitiveLighting(s, {corners.data(), count});
    ++maskCounts[selection.pointMask];
    flatCount += selection.flatBlack;
    ASSERT_TRUE(ps1::mips::interpretStaticCall(
        memory.ramPtr(), &ctx, count == 3 ? 0x8004305cu : 0x800430d4u,
        0x80042ca8u, 0x80043170u))
        << sample;
    if (selection.flatBlack) {
      ASSERT_EQ(memory.read32(packet + 4u),
                count == 3 ? 0x20000000u : 0x28000000u)
          << sample;
      continue;
    }
    ASSERT_EQ(memory.read32(packet + 4u), 0xdeadbeefu) << sample;
    for (uint32_t corner = 0; corner < count; ++corner)
      ASSERT_EQ(packedRgb(shadeSceneryConstructionVertex(s, selection,
                                                         corners[corner])),
                ctx.r[corner == 3 ? 5 : 13 + corner] & 0xffffffu)
          << "sample=" << sample << " corner=" << corner;
  }
  for (const auto count : maskCounts)
    EXPECT_GT(count, 0u);
  EXPECT_GT(flatCount, 0u);
}
