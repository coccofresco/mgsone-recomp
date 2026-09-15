#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
#include "runtime/gpu/gpu.h"
#include <cstring>
#include <fstream>
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

namespace {
struct ScreenActorFixture {
  std::vector<uint8_t> ram = std::vector<uint8_t>(2 * 1024 * 1024);
  static constexpr uint32_t actor = 0x80100000, prims = 0x80110000;
  ScreenActorFixture() { configure(true, false); }
  ~ScreenActorFixture() {
    configure(false, false);
    captureScreenEffects({});
  }
  void word(uint32_t a, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
      ram[(a & 0x1fffff) + i] = v >> (8 * i);
  }
  void install(uint32_t level, const char *name) {
    uint32_t head = 0x800af390 + 68 * level;
    word(head + 4, actor);
    word(actor, head);
    word(actor + 4, head + 32);
    word(head + 32, actor);
    word(actor + 8, 0x80070000);
    word(actor + 20, 0x80120000);
    std::memcpy(ram.data() + 0x120000, name, std::strlen(name) + 1);
    word(actor + 32, prims);
  }
  void visor(uint32_t color = 0x62287838) {
    install(2, "scn_mask.c");
    for (unsigned b = 0; b < 2; ++b) {
      word(prims + 20 + b * 16, color);
      word(prims + 24 + b * 16, 0xff90ff60);
      word(prims + 28 + b * 16, 0x00e00140);
      for (unsigned i = 0; i < 112; ++i) {
        auto p = prims + 64 + (112 * b + i) * 16;
        word(p + 4, 0x62404040);
        word(p + 8, 0xff60u | (uint32_t(uint16_t(-112 + 2 * int(i))) << 16));
        word(p + 12, 0x00010140);
      }
    }
    captureScreenEffects(ram);
  }
  DrawPacket tile(BlendMode mode) {
    DrawPacket p;
    p.opcode = 0x62;
    p.vertexCount = 4;
    p.route = PacketRoute::Screen2D;
    p.state.blend.enabled = true;
    p.state.blend.mode = mode;
    p.vertices[1].x = 320;
    p.vertices[2].y = 224;
    return p;
  }
};
VulkanReplayBakedBatch effectBatch(MgsScreenEffectKind kind, uint32_t count) {
  VulkanReplayBakedBatch b;
  b.internalScale = 4;
  b.vertices.resize(6);
  for (uint32_t part = 0; part < count; ++part) {
    VulkanReplayBakedDraw d;
    d.vertexCount = 6;
    d.segmentCount = 2;
    auto &s = d.segments[0];
    s.scissor = {1280, 0, 1280, 896};
    s.uniforms.mgsProjection = {float(part), 1048576, 77, 0};
    s.uniforms.mgsViewRows[3] = {float(count), float(kind), 0, 0};
    s.uniforms.textureState[2] =
        replayDrawFlagBit(ReplayDrawFlag::SemiTransparent);
    d.feedbackPlanes =
        vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Color);
    b.commands.push_back({part, d});
  }
  return b;
}
} // namespace

TEST(MgsScreenEffectsTest,
     VisorOwnershipRequiresCompleteUsaLayoutAndLiveLinks) {
  ScreenActorFixture f;
  f.visor();
  auto tile = f.tile(BlendMode::BackgroundPlusForeground);
  bindScreenEffect(tile, f.prims + 20);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::NightVision);
  EXPECT_EQ(tile.screenEffect.parts, 113u);
  auto line = f.tile(BlendMode::BackgroundMinusForeground);
  bindScreenEffect(line, f.prims + 68 + 111 * 16);
  EXPECT_EQ(line.screenEffect.part, 112u);
  tile.screenEffect = {};
  bindScreenEffect(tile, f.prims + 21);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::None);
  f.word(f.actor, 0);
  captureScreenEffects(f.ram);
  bindScreenEffect(tile, f.prims + 20);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::None);
  f.visor(0x62000050);
  bindScreenEffect(tile, f.prims + 20);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::ThermalVision);
  tile.screenEffect = {};
  f.word(f.prims + 68 + 80 * 16, 0x62404041);
  captureScreenEffects(f.ram);
  bindScreenEffect(tile, f.prims + 20);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::None);
  f.visor();
  tile.state.mask.checkMaskBit = true;
  bindScreenEffect(tile, f.prims + 20);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::None);
  tile.state.mask.checkMaskBit = false;
  captureScreenEffects({});
  bindScreenEffect(tile, f.prims + 20);
  EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::None);
}

TEST(MgsScreenEffectsTest,
     FullVisorPassReplaces113PacketsAndReadsOnlyItsBuffer) {
  auto b = effectBatch(MgsScreenEffectKind::NightVision, 113);
  prepareScreenReads(b);
  ASSERT_EQ(b.commands.size(), 1u);
  const auto &d = std::get<VulkanReplayBakedDraw>(b.commands[0].payload);
  EXPECT_EQ(d.segmentCount, 1u);
  EXPECT_EQ(d.feedbackPlanes, 0u);
  EXPECT_EQ(d.route, PacketRoute::Screen2D);
  EXPECT_EQ(d.segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::None);
  EXPECT_EQ(d.uniforms.destinationState, (std::array<uint32_t, 4>{}));
  EXPECT_TRUE(d.uniforms.textureState[2] &
              replayDrawFlagBit(ReplayDrawFlag::ScreenEffect));
  auto read = screenRead(d, b);
  ASSERT_TRUE(read);
  EXPECT_EQ(read->source, (VulkanReplayBakedRect{320, 0, 320, 224}));
  EXPECT_EQ(b.vertices[d.firstVertex + 4].positionNative,
            (std::array<float, 2>{640, 224}));
}

TEST(MgsScreenEffectsTest,
     IncompleteDuplicateOrInterleavedActorPassStaysAtomic) {
  for (unsigned defect = 0; defect < 4; ++defect) {
    auto b = effectBatch(MgsScreenEffectKind::NightVision, 113);
    if (defect == 0)
      b.commands.pop_back();
    if (defect == 1)
      std::get<VulkanReplayBakedDraw>(b.commands.back().payload)
          .segments[0]
          .uniforms.mgsProjection[0] = 0;
    if (defect == 2)
      b.commands.insert(b.commands.begin() + 50,
                        {999, VulkanReplayBakedFillRect{}});
    if (defect == 3)
      std::get<VulkanReplayBakedDraw>(b.commands.back().payload)
          .segments[0]
          .uniforms.mgsProjection[2] = 78;
    auto size = b.commands.size();
    prepareScreenReads(b);
    EXPECT_EQ(b.commands.size(), size);
    for (const auto &c : b.commands)
      if (auto *d = std::get_if<VulkanReplayBakedDraw>(&c.payload))
        EXPECT_FALSE(d->uniforms.textureState[2] &
                     replayDrawFlagBit(ReplayDrawFlag::ScreenEffect));
  }
}

TEST(MgsScreenEffectsTest,
     TemporalHistoryReadsBothBuffersAndRespectsCinematicCrop) {
  auto b = effectBatch(MgsScreenEffectKind::TemporalBlur, 2);
  prepareScreenReads(b);
  ASSERT_EQ(b.commands.size(), 1u);
  const auto &d = std::get<VulkanReplayBakedDraw>(b.commands[0].payload);
  auto read = screenRead(d, b);
  ASSERT_TRUE(read);
  EXPECT_EQ(read->source, (VulkanReplayBakedRect{0, 0, 640, 224}));
  EXPECT_EQ(read->destination, (VulkanReplayBakedRect{320, 24, 320, 160}));
}

TEST(MgsScreenEffectsTest, FadeUsesRenderedTileEnvelopeAndRejectsWrongBlend) {
  ScreenActorFixture f;
  f.install(3, "fadeio.c");
  f.word(f.actor + 36, f.prims);
  f.word(f.actor + 52, 2);
  captureScreenEffects(f.ram);
  auto p = f.tile(BlendMode::BackgroundPlusForeground);
  p.vertices[0].color = {128, 128, 128};
  bindScreenEffect(p, f.prims + 36);
  EXPECT_EQ(p.screenEffect.kind, MgsScreenEffectKind::FadeWhite);
  EXPECT_EQ(p.screenEffect.part, 0u);
  EXPECT_FLOAT_EQ(p.screenEffect.parameters[0][0], 128.f / 255);
  p.screenEffect = {};
  p.state.blend.mode = BlendMode::BackgroundMinusForeground;
  bindScreenEffect(p, f.prims + 36);
  EXPECT_EQ(p.screenEffect.kind, MgsScreenEffectKind::None);
}

TEST(MgsScreenEffectsTest, SensorTimeTracksGuestClockPauseAndRewind) {
  ScreenActorFixture f;
  f.visor();
  const auto phase = [&](uint32_t time, uint32_t pause) {
    f.word(0x800ada48, time);
    f.word(0x800ae0a0, pause);
    captureScreenEffects(f.ram);
    auto p = f.tile(BlendMode::BackgroundPlusForeground);
    bindScreenEffect(p, f.prims + 20);
    EXPECT_EQ(p.screenEffect.kind, MgsScreenEffectKind::NightVision);
    return p.screenEffect.parameters[2][3];
  };
  EXPECT_EQ(phase(1337, 0), 1337);
  EXPECT_EQ(phase(1338, 0), 1338);
  EXPECT_EQ(phase(1340, 1), 0);
  EXPECT_EQ(phase(2000, 1), 0);
  EXPECT_EQ(phase(1337, 0), 1337);
}

TEST(MgsScreenEffectsTest, UsaExecutableProvesClockPauseAndThermalAddresses) {
  const char *path = std::getenv("PS1_TEST_MGS_USA_EXE");
  if (!path)
    GTEST_SKIP();
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file);
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(file)), {});
  ASSERT_GE(exe.size(), 2048u);
  ASSERT_EQ(std::memcmp(exe.data(), "PS-X EXE", 8), 0);
  const auto word = [&](uint32_t offset) {
    return uint32_t(exe.at(offset)) | uint32_t(exe.at(offset + 1)) << 8 |
           uint32_t(exe.at(offset + 2)) << 16 |
           uint32_t(exe.at(offset + 3)) << 24;
  };
  const auto instruction = [&](uint32_t address) {
    return word(2048 + address - word(0x18));
  };
  EXPECT_EQ(instruction(0x8009ba24), 0x3c1c800bu); // gp = 0x800ad9fc
  EXPECT_EQ(instruction(0x8009ba28), 0x279cd9fcu);
  EXPECT_EQ(instruction(0x80014c30), 0x8f82004cu); // GV_Time = gp+76
  EXPECT_EQ(instruction(0x80014c40), 0xaf82004cu);
  EXPECT_EQ(instruction(0x80014c80), 0x8c42e0a0u); // GV_PauseLevel
  EXPECT_EQ(instruction(0x8007b604), 0x8c42db3cu); // GM_GameStatus
  EXPECT_EQ(instruction(0x8007b614), 0x34420008u); // STATE_THERMG
  EXPECT_EQ(instruction(0x8007b61c), 0xac22db3cu);
}
