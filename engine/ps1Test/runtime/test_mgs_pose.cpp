#include "runtime/cpu_context.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/memory.h"
#include "runtime/mips_interpreter.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

using namespace ps1;
using namespace ps1::games::mgs;
namespace {
class MgsPoseTest : public ::testing::Test {
protected:
  Memory memory;
  SceneInstance instance;
  static constexpr uint32_t object = 0x80130000, definition = 0x80132000,
                            rots = 0x80135000, adjust = 0x80135100,
                            waist = 0x80135200;
  void identity(uint32_t address) {
    for (unsigned k = 0; k < 32; ++k)
      memory.write8(address + k, 0);
    for (unsigned k : {0u, 8u, 16u})
      memory.write16(address + k, 4096);
  }
  void readNative() {
    for (auto &model : instance.models) {
      model.world = {};
      for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c)
          model.world[c * 4 + r] =
              int16_t(memory.read16(model.guestAddress + (r * 3 + c) * 2)) /
              4096.0f;
        model.world[12 + r] =
            int32_t(memory.read32(model.guestAddress + 20 + r * 4));
      }
      model.world[15] = 1;
    }
  }
  void setup(uint32_t count = 3) {
    instance = {};
    instance.guestAddress = object;
    instance.definition = definition;
    instance.primaryModels = count;
    instance.asset = 0;
    identity(object);
    memory.write32(object + 56, rots);
    for (uint32_t j = 0; j < count; ++j) {
      SceneModel model;
      model.guestAddress = object + 72 + j * 92;
      identity(model.guestAddress);
      memory.write32(model.guestAddress + 64, definition + 32 + j * 88);
      memory.write32(definition + 76 + j * 88, j - 1);
      instance.models.push_back(model);
    }
    readNative();
  }
};
TEST_F(MgsPoseTest, PreservesSubunitHierarchyTranslationWithoutGuestWrites) {
  setup();
  memory.write16(rots + 4, 1); // one PSX angular step, evaluated without a LUT
  memory.write32(definition + 32 + 2 * 88 + 32, 100);
  const std::vector<uint8_t> before(memory.ramPtr(),
                                    memory.ramPtr() + Memory::RAM_SIZE);
  const auto result = reconstructScenePose(memory, instance, 3);
  EXPECT_EQ(result.source, ScenePoseSource::Rotations);
  EXPECT_NEAR(instance.models[2].presentationWorld[13], 0.1533980186, 1e-7);
  EXPECT_NEAR(instance.models[2].presentationWorld[12], 99.99988235, 5e-6);
  EXPECT_FLOAT_EQ(instance.models[2].world[13], 0);
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
}
TEST_F(MgsPoseTest, WaistBasisIsSeparateFromAdjustedFirstJoint) {
  setup();
  memory.write32(object + 60, adjust);
  memory.write32(object + 64, waist);
  memory.write16(waist + 4, 1024);
  memory.write16(adjust + 4, -1024);
  memory.write32(definition + 32 + 88 + 32, 100);
  memory.write32(definition + 32 + 2 * 88 + 32, 100);
  const auto result = reconstructScenePose(memory, instance, 3);
  ASSERT_EQ(result.source, ScenePoseSource::Rotations);
  EXPECT_NEAR(instance.models[0].presentationWorld[1], -1, 1e-6);
  EXPECT_NEAR(instance.models[1].presentationWorld[12], 0, 1e-6);
  EXPECT_NEAR(instance.models[1].presentationWorld[13], 100, 1e-6);
  EXPECT_NEAR(instance.models[2].presentationWorld[13], 200, 1e-6);
}
TEST_F(MgsPoseTest, MovementUsesParentTranslationAndSharedWorldRotation) {
  setup();
  memory.write32(object + 56, 0);
  memory.write32(object + 68, rots);
  memory.write16(object, 0);
  memory.write16(object + 2, uint16_t(-4096));
  memory.write16(object + 6, 4096);
  memory.write16(object + 8, 0);
  memory.write32(object + 20, 1000);
  for (unsigned j = 0; j < 3; ++j)
    memory.write16(rots + j * 8, 10);
  EXPECT_EQ(reconstructScenePose(memory, instance, 3).source,
            ScenePoseSource::Movements);
  EXPECT_FLOAT_EQ(instance.models[2].presentationWorld[12], 1000);
  EXPECT_FLOAT_EQ(instance.models[2].presentationWorld[13], 30);
  EXPECT_FLOAT_EQ(instance.models[2].presentationWorld[1], 1);
}
TEST_F(MgsPoseTest, OnePieceIgnoresJointOperandsAndSupportsLargeScenery) {
  setup(181);
  instance.flags = 0x40;
  memory.write32(object + 56, 0x801fffff);
  memory.write32(object + 28, 12345);
  const auto result = reconstructScenePose(memory, instance, 181);
  EXPECT_EQ(result.source, ScenePoseSource::OnePiece);
  EXPECT_EQ(result.models, 181u);
  EXPECT_FLOAT_EQ(instance.models.back().presentationWorld[14], 12345);
}
TEST_F(MgsPoseTest, InvalidHierarchyOrOperandsCannotPublishAPartialPose) {
  setup();
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    SCOPED_TRACE(scenario);
    memory.write32(object + 56, scenario == 0 ? 0x801fffff : rots);
    memory.write32(definition + 76 + 2 * 88, scenario == 1 ? 2 : 1);
    memory.write32(instance.models[2].guestAddress + 64,
                   definition + 32 + 2 * 88 + (scenario == 2 ? 88 : 0));
    memory.write32(object + 60, scenario == 3 ? 0x801fffff : 0);
    const auto result = reconstructScenePose(memory, instance, 3);
    EXPECT_EQ(result.source, ScenePoseSource::Native);
    EXPECT_TRUE(result.invalidInput);
    for (const auto &model : instance.models)
      EXPECT_EQ(model.world, model.presentationWorld);
  }
}
TEST_F(MgsPoseTest, UnsupportedPoseClearsAPreviouslyReconstructedPose) {
  setup();
  memory.write16(rots, 123);
  ASSERT_EQ(reconstructScenePose(memory, instance, 3).source,
            ScenePoseSource::Rotations);
  memory.write32(object + 56, 0);
  const auto result = reconstructScenePose(memory, instance, 3);
  EXPECT_EQ(result.source, ScenePoseSource::Native);
  EXPECT_FALSE(result.invalidInput);
  for (const auto &model : instance.models)
    EXPECT_EQ(model.world, model.presentationWorld);
}
TEST_F(MgsPoseTest, AttachmentsUseExactRootOwnershipAcrossReorderedQueues) {
  setup();
  memory.write16(rots + 4, 1);
  memory.write32(definition + 32 + 2 * 88 + 32, 100);
  SceneInstance attachment;
  attachment.guestAddress = 0x80138000;
  attachment.flags = 0x40;
  attachment.asset = 0;
  attachment.primaryModels = 1;
  attachment.rootAddress = instance.models[2].guestAddress;
  attachment.models.push_back(instance.models[0]);
  attachment.models[0].guestAddress = attachment.guestAddress + 72;
  identity(attachment.guestAddress);
  identity(attachment.models[0].guestAddress);
  SceneSnapshot scene;
  scene.instances = {attachment, instance}; // child before its owner
  reconstructScenePoses(memory, scene);
  ASSERT_TRUE(scene.instances[0].pose.inheritedRoot);
  EXPECT_EQ(scene.instances[0].models[0].presentationWorld,
            scene.instances[1].models[2].presentationWorld);
  EXPECT_NEAR(scene.instances[0].models[0].presentationWorld[13], 0.1533980186,
              1e-7);
  // A root copy from a different animation sample cannot use the new owner.
  memory.write32(attachment.guestAddress + 20, 1);
  reconstructScenePoses(memory, scene);
  EXPECT_FALSE(scene.instances[0].pose.inheritedRoot);
  EXPECT_FLOAT_EQ(scene.instances[0].models[0].presentationWorld[12], 1);
  EXPECT_FLOAT_EQ(scene.instances[0].models[0].presentationWorld[13], 0);
  // Cyclic roots retain native matrices, with no partial inherited state.
  scene.instances[1].rootAddress = attachment.models[0].guestAddress;
  reconstructScenePoses(memory, scene);
  for (const auto &entry : scene.instances) {
    EXPECT_TRUE(entry.pose.invalidInput);
    for (const auto &model : entry.models)
      EXPECT_EQ(model.world, model.presentationWorld);
  }
}
TEST_F(MgsPoseTest,
       MatchesLocallyExtractedUsaPoseRoutinesAcrossAnimationModes) {
  const char *path = std::getenv("PS1_TEST_MGS_USA_EXE");
  if (!path)
    GTEST_SKIP() << "optional locally extracted USA SLUS_005.94";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file);
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(file)), {});
  ASSERT_GE(exe.size(), 2048u);
  ASSERT_EQ(std::memcmp(exe.data(), "PS-X EXE", 8), 0);
  const auto word = [&](unsigned p) {
    return uint32_t(exe[p]) | uint32_t(exe[p + 1]) << 8 |
           uint32_t(exe[p + 2]) << 16 | uint32_t(exe[p + 3]) << 24;
  };
  const uint32_t base = word(0x18), size = word(0x1c);
  ASSERT_EQ(base, 0x80010000u);
  ASSERT_LE(uint64_t(base & 0x1fffff) + size, Memory::RAM_SIZE);
  ASSERT_LE(uint64_t(size) + 2048, exe.size());
  std::memcpy(memory.ramPtr() + (base & 0x1fffff), exe.data() + 2048, size);
  ASSERT_EQ(memory.read32(0x8001D030), 0x8c890044u);
  ASSERT_EQ(memory.read32(0x8001D170), 0x27bdff98u);
  ASSERT_EQ(memory.read32(0x80096A44), 0x84880004u);
  // Independent oracle executes the original instructions, including the
  // game's LUT and GTE rounding. These instructions are never in the renderer.
  std::mt19937 random(0x4d4753);
  double maximumRotation = 0, maximumTranslation = 0;
  for (unsigned mode = 0; mode < 5; ++mode) {
    for (unsigned sample = 0; sample < 32; ++sample) {
      SCOPED_TRACE(::testing::Message()
                   << "mode " << mode << " sample " << sample);
      setup(6);
      memory.write32(object + 60, mode & 2 ? adjust : 0);
      memory.write32(object + 64, mode & 1 ? waist : 0);
      memory.write32(object + 68, mode == 4 ? rots : 0);
      if (mode == 4)
        memory.write32(object + 56, 0);
      for (unsigned j = 0; j < 6; ++j) {
        for (unsigned axis = 0; axis < 3; ++axis) {
          memory.write16(rots + j * 8 + axis * 2, int(random() % 4096) - 2048);
          memory.write16(adjust + j * 8 + axis * 2,
                         int(random() % 4096) - 2048);
          memory.write32(definition + 32 + j * 88 + 32 + axis * 4,
                         int(random() % 1000) - 500);
        }
      }
      for (unsigned axis = 0; axis < 3; ++axis)
        memory.write16(waist + axis * 2, int(random() % 4096) - 2048);
      recomp_context ctx{};
      ctx.mem = &memory;
      ctx.r4 = waist;
      ctx.r5 = object;
      ctx.r29 = 0x801ff000;
      ctx.r31 = 0x80000000;
      ASSERT_TRUE(mips::interpretStaticCall(memory.ramPtr(), &ctx, 0x80096A44,
                                            base, base + size));
      for (unsigned axis = 0; axis < 3; ++axis)
        memory.write32(object + 20 + axis * 4, 10000 + axis * 2000);
      for (unsigned offset = 0; offset < 32; offset += 4)
        memory.write32(0x1f800020 + offset, memory.read32(object + offset));
      ctx.r4 = object;
      ctx.r5 = 6;
      ctx.r31 = 0x80000000;
      ASSERT_TRUE(mips::interpretStaticCall(memory.ramPtr(), &ctx,
                                            mode == 4 ? 0x8001D030 : 0x8001D170,
                                            base, base + size));
      readNative();
      const std::vector<uint8_t> before(memory.ramPtr(),
                                        memory.ramPtr() + Memory::RAM_SIZE);
      const auto result = reconstructScenePose(memory, instance, 6);
      ASSERT_FALSE(result.invalidInput);
      ASSERT_NE(result.source, ScenePoseSource::Native);
      EXPECT_LT(result.maxRotationError, 0.015);
      EXPECT_LT(result.maxTranslationError, 10.0);
      EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
      maximumRotation = std::max(maximumRotation, result.maxRotationError);
      maximumTranslation =
          std::max(maximumTranslation, result.maxTranslationError);
    }
  }
  RecordProperty("maximum_rotation_error", std::to_string(maximumRotation));
  RecordProperty("maximum_translation_error",
                 std::to_string(maximumTranslation));
}
} // namespace
