#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
#include "runtime/gpu/gpu.h"
#include <cstring>
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::games::mgs;
using namespace ps1::gpu::replay;

TEST(MgsScreenEffectsTest, SnapshotReuseRespectsFeedbackAndInterveningWrites) {
  ScreenReadTracker tracker;
  EXPECT_TRUE(tracker.needsSnapshot({{0, 10, 160, 2}, {0, 6, 160, 2}}));
  EXPECT_FALSE(tracker.needsSnapshot({{0, 12, 160, 2}, {0, 8, 160, 2}}));
  // Reading a prior destination needs a fresh source, even with the same page.
  EXPECT_TRUE(tracker.needsSnapshot({{0, 8, 160, 2}, {0, 10, 160, 2}}));
  EXPECT_FALSE(tracker.needsSnapshot({{320, 10, 160, 2}, {320, 6, 160, 2}}));
  tracker.invalidate();
  EXPECT_TRUE(tracker.needsSnapshot({{320, 12, 160, 2}, {320, 8, 160, 2}}));
}

TEST(MgsScreenEffectsTest, WaterOwnershipRequiresLiveActorAndAlignedSprite) {
  struct Profile {
    Profile() { configure(true, false); }
    ~Profile() {
      configure(false, false);
      captureWaterEffects({});
    }
  } profile;
  std::vector<uint8_t> ram(2 * 1024 * 1024);
  const auto word = [&](uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      ram[(a & 0x1fffff) + i] = v >> (8 * i);
  };
  constexpr uint32_t head = 0x800af45c, actor = 0x80100000, prims = 0x80110000;
  word(head + 4, actor);
  word(actor, head);
  word(actor + 20, 0x80120000);
  std::memcpy(ram.data() + 0x120000, "wt_view.c", 10);
  word(actor + 56, prims);
  word(actor + 64, 1);
  word(actor + 72, 43);
  captureWaterEffects(ram);
  DrawPacket packet;
  packet.opcode = 0x64;
  packet.route = PacketRoute::Screen2D;
  packet.vertexCount = 4;
  bindWaterEffect(packet, prims + 60);
  EXPECT_EQ(packet.waterEffect,
            (std::array<uint32_t, 2>{actor & 0x1fffff, 42}));
  packet.waterEffect = {};
  bindWaterEffect(packet, prims + 61);
  EXPECT_EQ(packet.waterEffect[0], 0u);
  word(actor + 64, 0);
  captureWaterEffects(ram);
  bindWaterEffect(packet, prims + 60);
  EXPECT_EQ(packet.waterEffect[0], 0u);
  word(actor + 64, 1);
  word(actor, 0);
  captureWaterEffects(ram);
  bindWaterEffect(packet, prims + 60);
  EXPECT_EQ(packet.waterEffect[0], 0u);
}

TEST(MgsScreenEffectsTest, CombinesOneActorsStripsWithoutRemovingOtherDraws) {
  VulkanReplayBakedBatch batch;
  batch.internalScale = 12;
  VulkanReplayBakedDraw strip;
  strip.route = PacketRoute::Screen2D;
  strip.segmentCount = 1;
  strip.vertexCount = 6;
  strip.segments[0].scissor = {0, 0, 3840, 2688};
  strip.segments[0].vertexCount = 6;
  auto &u = strip.segments[0].uniforms;
  u.textureState = {0x100, 0,
                    replayDrawFlagBit(ReplayDrawFlag::Textured) |
                        replayDrawFlagBit(ReplayDrawFlag::WaterRefraction),
                    0};
  u.mgsProjection = {11, 1048576, 77, 0};
  batch.vertices.resize(6);
  batch.commands.push_back({0, strip});
  batch.commands.push_back({1, strip});
  VulkanReplayBakedNativeUpload upload;
  batch.commands.push_back({2, upload});
  batch.commands.push_back({3, strip});
  // A new ordered submission is a new pass, even with an unchanged phase.
  strip.segments[0].uniforms.mgsProjection[2] = 78;
  batch.commands.push_back({4, strip});
  prepareScreenReads(batch);
  ASSERT_EQ(batch.commands.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<VulkanReplayBakedNativeUpload>(
      batch.commands[1].payload));
  const auto &pass = std::get<VulkanReplayBakedDraw>(batch.commands[0].payload);
  EXPECT_EQ(batch.vertices[pass.firstVertex].positionNative,
            (std::array<float, 2>{0, 6}));
  EXPECT_EQ(batch.vertices[pass.firstVertex + 4].positionNative,
            (std::array<float, 2>{320, 218}));
  EXPECT_EQ(pass.segments[0].uniforms.destinationState,
            (std::array<uint32_t, 4>{}));
  EXPECT_EQ(pass.segments[0].destinationSnapshot, VulkanReplayBakedRect{});
  EXPECT_EQ(pass.segments[0].uniforms.mgsProjection[0], 11);
  EXPECT_TRUE(pass.uniforms.textureState[2] &
              replayDrawFlagBit(ReplayDrawFlag::ScreenTexture));
}

TEST(MgsScreenEffectsTest, SpriteCaptureRetainsWrappedUvAndNativePixels) {
  struct Profile {
    ~Profile() { configure(false, false); }
  } profile;
  gpu::GPU native, modern;
  gpu::GPU::ReplayConfig cfg;
  cfg.enabled = true;
  cfg.internalScale = 4;
  cfg.trueColor = true;
  cfg.trueDepthMode = ReplayDepthMode::Hybrid;
  modern.configureReplay(cfg);
  const std::array<uint32_t, 9> commands{
      0xa0000000, 250u,       (1u << 16) | 8u, 0x001f03e0, 0x7c007fff,
      0x83e0801f, 0xfffffc00, 0xe1000100,      0xe6000000};
  for (auto *gpu : {&native, &modern}) {
    configure(gpu == &modern, false);
    for (auto w : commands)
      gpu->writeGP0(w);
    for (auto w : {0x65808080u, 10u | (10u << 16), 250u, 8u | (1u << 16)})
      gpu->writeGP0(w);
  }
  for (uint32_t i = 0; i < gpu::GPU::VRAM_WIDTH * gpu::GPU::VRAM_HEIGHT; ++i)
    ASSERT_EQ(native.getVRAM()[i].raw, modern.getVRAM()[i].raw) << i;
  modern.snapshotDisplayBuffer(1);
  const auto frame = modern.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const DrawPacket *sprite = nullptr;
  for (const auto &op : frame->operations())
    if (const auto *p = std::get_if<DrawPacket>(&op))
      sprite = p;
  ASSERT_NE(sprite, nullptr);
  EXPECT_EQ(sprite->opcode, 0x65);
  EXPECT_EQ(sprite->vertices[1].u, 258);
  EXPECT_EQ(sprite->state.textureUvDomain.maxU, 257);
}
