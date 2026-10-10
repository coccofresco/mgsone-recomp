#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
#include "runtime/gpu/gpu.h"
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::games::mgs;
using namespace ps1::gpu::replay;

TEST(MgsScreenEffectsTest, FocusRequiresOwnedTwoHalvesAndRetainsLensParameters) {
  struct Profile {Profile(){configure(true,false);} ~Profile(){configure(false,false);captureScreenEffects({});}} profile;
  std::vector<uint8_t> ram(2*1024*1024);
  const auto word=[&](uint32_t p,uint32_t v){for(unsigned i=0;i<4;++i)ram[(p&0x1fffff)+i]=v>>(8*i);};
  constexpr uint32_t head=0x800af390+7*68, actor=0x80100000, prims=0x80110000;
  word(head+4,actor);word(actor,head);word(actor+4,head+32);word(head+32,actor);
  word(actor+8,0x80070000);word(actor+20,0x80120000);
  std::memcpy(ram.data()+0x120000,"focus.c",8);
  word(actor+32,prims);word(actor+52,10000);word(actor+60,2000);
  captureScreenEffects(ram);
  DrawPacket p;p.opcode=0x66;p.vertexCount=4;p.textured=true;p.state.blend.enabled=true;
  p.state.blend.mode=BlendMode::HalfBackgroundPlusHalfForeground;
  p.state.texturePage.depth=TextureDepth::Direct15;
  p.vertices[1].x=160;p.vertices[2].y=224;
  bindScreenEffect(p,prims+116);
  EXPECT_EQ(p.screenEffect.kind,MgsScreenEffectKind::Focus);
  EXPECT_EQ(p.screenEffect.parts,2u);EXPECT_EQ(p.screenEffect.part,0u);
  EXPECT_EQ(p.screenEffect.parameters[0][0],10000.f);
  EXPECT_EQ(p.screenEffect.parameters[0][1],2000.f);
  bindScreenEffect(p,prims+172);EXPECT_EQ(p.screenEffect.part,1u);
  p.screenEffect={};p.vertices[2].y=160;
  bindScreenEffect(p,prims+116);EXPECT_EQ(p.screenEffect.kind,MgsScreenEffectKind::None);
}

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

TEST(MgsScreenEffectsTest, CameraLensGlowDoesNotReadWorldDepth) {
  VulkanReplayBakedBatch batch;batch.internalScale=12;batch.vertices.resize(6);
  VulkanReplayBakedDraw draw;draw.segmentCount=1;draw.vertexCount=6;
  draw.segments[0].scissor={0,0,3840,2688};
  auto &e=draw.segments[0].uniforms.mgsSurfaceEffect;
  e[0][0]=float(MgsSurfaceKind::Glow);e[2][3]=0x100000;
  auto world=screenRead(draw,batch);ASSERT_TRUE(world);EXPECT_TRUE(world->depth);
  e[2][2]=1;
  auto camera=screenRead(draw,batch);ASSERT_TRUE(camera);EXPECT_FALSE(camera->depth);
  EXPECT_EQ(camera->source,world->source);EXPECT_EQ(camera->destination,world->destination);
  e[0][0]=float(MgsSurfaceKind::Smoke);
  auto smoke=screenRead(draw,batch);ASSERT_TRUE(smoke);EXPECT_TRUE(smoke->depth);
}

TEST(MgsScreenEffectsTest, BubblePlumesShareSceneOnlyAcrossConsecutiveDrawsInOneBuffer) {
  VulkanReplayBakedBatch batch;batch.internalScale=1;batch.vertices.resize(6);
  VulkanReplayBakedDraw draw;draw.segmentCount=1;draw.vertexCount=6;
  draw.segments[0].scissor={0,0,320,224};
  auto &effect=draw.segments[0].uniforms.mgsSurfaceEffect;
  effect[0][0]=float(MgsSurfaceKind::Bubble);effect[2][3]=0x100000;
  const auto first=screenRead(draw,batch);ASSERT_TRUE(first);EXPECT_TRUE(first->depth);
  ScreenReadTracker tracker;EXPECT_TRUE(tracker.needsSnapshot(*first));
  effect[2][3]=0x110000;auto next=screenRead(draw,batch);ASSERT_TRUE(next);
  EXPECT_FALSE(tracker.needsSnapshot(*next)); // next actor in the same plume
  effect[0][0]=float(MgsSurfaceKind::Smoke);next=screenRead(draw,batch);ASSERT_TRUE(next);
  EXPECT_TRUE(tracker.needsSnapshot(*next));
  EXPECT_TRUE(tracker.needsSnapshot(*first)); // intervening optical layer
  effect[0][0]=float(MgsSurfaceKind::Bubble);draw.segments[0].scissor.x=320;
  next=screenRead(draw,batch);ASSERT_TRUE(next);EXPECT_TRUE(tracker.needsSnapshot(*next));
  tracker.invalidate(); // ordinary geometry writes the scene
  EXPECT_TRUE(tracker.needsSnapshot(*next));
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
  const auto waterRead=screenRead(pass,batch);
  ASSERT_TRUE(waterRead);
  EXPECT_TRUE(waterRead->depth);
  auto furnace=pass;
  furnace.segments[0].uniforms.mgsViewRows[4][0]=1.0f;
  const auto furnaceRead=screenRead(furnace,batch);
  ASSERT_TRUE(furnaceRead);
  EXPECT_FALSE(furnaceRead->depth);
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

TEST(MgsScreenEffectsTest, CodecBoundaryRequiresLiveMenuAndBothBandPackets) {
  ScreenActorFixture f; f.install(8, "menuman.c");
  constexpr uint32_t begin=0x80130000;
  f.word(f.prims,begin+40);f.word(f.prims+8,begin+8192);
  f.word(f.prims+12,begin);
  for(unsigned buffer=0;buffer<2;++buffer) {
    const unsigned width=buffer ? 192 : 256;
    f.word(begin,4u<<24);f.word(begin+4,0x66181818);
    f.word(begin+8,32u<<16);f.word(begin+12,(32u<<8)|(buffer ? 64 : 0));
    f.word(begin+16,(8u<<16)|width);
    f.word(begin+20,4u<<24);f.word(begin+24,0x66181818);
    f.word(begin+28,(32u<<16)|width);f.word(begin+32,32u<<8);
    f.word(begin+36,(8u<<16)|(320-width));
    const auto packet=[&](unsigned part) {
      DrawPacket p;p.opcode=0x66;p.vertexCount=4;p.textured=true;
      p.state.blend.enabled=true;p.state.blend.mode=BlendMode::BackgroundPlusForeground;
      p.state.texturePage.depth=TextureDepth::Direct15;
      p.vertices[0].x=part ? width : 0;p.vertices[1].x=part ? 320 : width;
      p.vertices[2].y=8;return p;
    };
    captureScreenEffects(f.ram);
    for(unsigned part=0;part<2;++part) {
      auto p=packet(part);bindScreenEffect(p,begin+4+20*part);
      EXPECT_EQ(p.screenEffect.kind,MgsScreenEffectKind::CodecScreen);
      EXPECT_EQ(p.screenEffect.parts,2u);EXPECT_EQ(p.screenEffect.part,part);
      p=packet(part);bindScreenEffect(p,begin+8+20*part);
      EXPECT_EQ(p.screenEffect.kind,MgsScreenEffectKind::None);
    }
    f.word(begin+24,0x66808080);captureScreenEffects(f.ram);
    auto p=packet(0);bindScreenEffect(p,begin+4);
    EXPECT_EQ(p.screenEffect.kind,MgsScreenEffectKind::None);
    f.word(begin+24,0x66181818);f.word(f.actor+8,0);captureScreenEffects(f.ram);
    p=packet(0);bindScreenEffect(p,begin+4);
    EXPECT_EQ(p.screenEffect.kind,MgsScreenEffectKind::None);
    f.word(f.actor+8,0x80070000);
  }
}

TEST(MgsScreenEffectsTest, CodecComposesWholeScreenOnceBeforeUnchangedText) {
  auto b=effectBatch(MgsScreenEffectKind::CodecScreen,2);
  VulkanReplayBakedDraw text; text.vertexCount=6;text.segmentCount=1;
  text.segments[0].uniforms.textureState={15,0x7fbc,0,0};
  b.commands.push_back({99,text});
  prepareScreenReads(b);
  ASSERT_EQ(b.commands.size(),4u);
  const auto &pass=std::get<VulkanReplayBakedDraw>(b.commands[2].payload);
  EXPECT_EQ(pass.uniforms.mgsViewRows[3][1],float(MgsScreenEffectKind::CodecScreen));
  const auto read=screenRead(pass,b);ASSERT_TRUE(read);
  EXPECT_EQ(read->destination,(VulkanReplayBakedRect{320,0,320,224}));
  EXPECT_EQ(b.vertices[pass.firstVertex].positionNative,(std::array<float,2>{320,0}));
  EXPECT_EQ(b.vertices[pass.firstVertex+4].positionNative,(std::array<float,2>{640,224}));
  EXPECT_EQ(std::memcmp(&std::get<VulkanReplayBakedDraw>(b.commands[3].payload).segments[0].uniforms,
                        &text.segments[0].uniforms, sizeof(text.segments[0].uniforms)),0);
  for(unsigned i=0;i<2;++i)
    EXPECT_FALSE(std::get<VulkanReplayBakedDraw>(b.commands[i].payload).segments[0].uniforms.textureState[2]&
                 replayDrawFlagBit(ReplayDrawFlag::ScreenEffect));
  for(unsigned defect=0;defect<3;++defect) {
    auto invalid=effectBatch(MgsScreenEffectKind::CodecScreen,2);
    if(defect==0) invalid.commands.pop_back();
    if(defect==1) invalid.commands.insert(invalid.commands.begin()+1,{99,text});
    if(defect==2) std::get<VulkanReplayBakedDraw>(invalid.commands.back().payload).segments[0].uniforms.mgsProjection[2]++;
    const auto size=invalid.commands.size();prepareScreenReads(invalid);
    EXPECT_EQ(invalid.commands.size(),size);
  }
}

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
     RedMaskUsesCinematicContextAndReturnsToGameplayThermal) {
  ScreenActorFixture f;
  for (const auto status : {0x80000028u, 8u, 0x80000028u}) {
    f.word(0x800adb3c, status);
    f.visor(0x62000050);
    auto tile = f.tile(BlendMode::BackgroundPlusForeground);
    bindScreenEffect(tile, f.prims + 20);
    EXPECT_EQ(tile.screenEffect.kind,
              status & 0x80000000u ? MgsScreenEffectKind::CinematicRedTint
                                   : MgsScreenEffectKind::ThermalVision);
    EXPECT_EQ(tile.screenEffect.parts, 113u);
    if (status & 0x80000000u)
      EXPECT_FLOAT_EQ(tile.screenEffect.parameters[0][0], 80.f / 255.f);
  }
  // The cinematic green camera feed has CRT optics, while gameplay goggles
  // retain their own high-resolution sensor. The palette identity is shared.
  for (const auto status : {0x80000028u,0u}) {
    f.word(0x800adb3c,status);f.visor();
    auto tile = f.tile(BlendMode::BackgroundPlusForeground);
    bindScreenEffect(tile, f.prims + 20);
    EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::NightVision);
    EXPECT_EQ(tile.screenEffect.parameters[0][1],status ? 1.f : 0.f);
  }
}

TEST(MgsScreenEffectsTest,
     FullVisorPassReplaces113PacketsAndReadsOnlyItsBuffer) {
  for (const auto kind :
       {MgsScreenEffectKind::NightVision, MgsScreenEffectKind::ThermalVision,
        MgsScreenEffectKind::CinematicRedTint}) {
    SCOPED_TRACE(uint32_t(kind));
    auto b = effectBatch(kind, 113);
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
    EXPECT_EQ(d.uniforms.mgsViewRows[3][1], float(kind));
  }
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
  // USA lighting/map ABI comes from this executable, not Integral offsets.
  EXPECT_EQ(instruction(0x800329e4), 0x3c15800cu);
  EXPECT_EQ(instruction(0x800329e8), 0x26b5a078u); // MAP[16] = 800ba078
  EXPECT_EQ(instruction(0x80032a10), 0x8f960824u); // map count = gp+824
  EXPECT_EQ(instruction(0x80032a34), 0x3c12800cu);
  EXPECT_EQ(instruction(0x80032a38), 0x26529ff8u); // StageObjs = 800b9ff8
  EXPECT_EQ(instruction(0x80032a40), 0x8f930820u); // stage count = gp+820
  EXPECT_EQ(instruction(0x8001ad74), 0x3c06800au);
  EXPECT_EQ(instruction(0x8001ad78), 0x24c6fad0u); // DG_LightMatrix
  EXPECT_EQ(instruction(0x8001ada4), 0x3c06800au);
  EXPECT_EQ(instruction(0x8001ada8), 0x24c6faf0u); // DG_ColorMatrix
  EXPECT_EQ(instruction(0x8001a8d0), 0xa79100d0u); // DG_Ambient = gp+0xd0
  EXPECT_EQ(instruction(0x8001a8d4), 0xa79000d2u);
  EXPECT_EQ(instruction(0x8001a8d8), 0xa79200d4u);
}

TEST(MgsScreenEffectsTest, StunAndAlarmRequireActorAndExactFullscreenTile) {
  for (const auto kind :
       {MgsScreenEffectKind::StunFlash, MgsScreenEffectKind::AlertTint}) {
    ScreenActorFixture f;
    f.install(3, kind == MgsScreenEffectKind::StunFlash ? "stgfd_io.c"
                                                        : "red_alrt.c");
    f.word(f.prims + 20, 0x62000000);
    f.word(f.prims + 24, 0xff90ff60);
    f.word(f.prims + 28, 0x00e00140);
    captureScreenEffects(f.ram);
    auto tile = f.tile(BlendMode::BackgroundPlusForeground);
    tile.vertices[0].color = {190, 190, 60};
    bindScreenEffect(tile, f.prims + 20);
    EXPECT_EQ(tile.screenEffect.kind, kind);
    EXPECT_FLOAT_EQ(tile.screenEffect.parameters[0][2], 60.f / 255.f);
    tile.screenEffect = {};
    f.word(f.prims + 28, 0x00200140); // similar UI rectangle is not the effect
    captureScreenEffects(f.ram);
    bindScreenEffect(tile, f.prims + 20);
    EXPECT_EQ(tile.screenEffect.kind, MgsScreenEffectKind::None);
    auto batch = effectBatch(kind, 1);
    prepareScreenReads(batch);
    ASSERT_EQ(batch.commands.size(), 1u);
    auto &draw =
        std::get<VulkanReplayBakedDraw>(batch.commands.front().payload);
    EXPECT_TRUE(draw.uniforms.textureState[2] &
                replayDrawFlagBit(ReplayDrawFlag::ScreenEffect));
  }
}

TEST(MgsScreenEffectsTest, ZoomBlurRequiresCompleteQuadrantsAndPreservesCrop) {
  ScreenActorFixture f;
  f.install(7, "blur.c");
  f.word(f.actor + 36, f.prims);
  f.word(f.actor + 84, 2); // outward zoom
  f.word(f.actor + 104, 24);
  f.word(f.actor + 108, 40);
  for (unsigned k = 0; k < 4; ++k)
    f.word(f.prims + k * 40 + 4, 0x2e808080);
  captureScreenEffects(f.ram);
  auto packet = f.tile(BlendMode::HalfBackgroundPlusHalfForeground);
  packet.opcode = 0x2e;
  packet.textured = true;
  packet.state.texturePage.depth = TextureDepth::Direct15;
  bindScreenEffect(packet, f.prims + 3 * 40 + 4);
  EXPECT_EQ(packet.screenEffect.kind, MgsScreenEffectKind::ZoomBlur);
  EXPECT_EQ(packet.screenEffect.part, 3u);
  EXPECT_EQ(packet.screenEffect.parameters[0],
            (std::array<float, 4>{2, 0, 24, 184}));
  auto batch = effectBatch(MgsScreenEffectKind::ZoomBlur, 4);
  for (auto &c : batch.commands)
    std::get<VulkanReplayBakedDraw>(c.payload)
        .segments[0]
        .uniforms.mgsViewRows[0] = {2, 0, 24, 184};
  auto incomplete = batch;
  incomplete.commands.pop_back();
  prepareScreenReads(incomplete);
  EXPECT_EQ(incomplete.commands.size(), 3u);
  EXPECT_FALSE(std::get<VulkanReplayBakedDraw>(incomplete.commands[0].payload)
                   .uniforms.textureState[2] &
               replayDrawFlagBit(ReplayDrawFlag::ScreenEffect));
  prepareScreenReads(batch);
  ASSERT_EQ(batch.commands.size(), 1u);
  const auto &draw = std::get<VulkanReplayBakedDraw>(batch.commands[0].payload);
  EXPECT_EQ(batch.vertices[draw.firstVertex].positionNative[1], 24.f);
  EXPECT_EQ(batch.vertices[draw.firstVertex + 4].positionNative[1], 184.f);
  EXPECT_EQ(screenRead(draw, batch)->source,
            (VulkanReplayBakedRect{0, 0, 640, 224}));
  EXPECT_TRUE(screenRead(draw, batch)->depth);
  packet.screenEffect = {};
  f.word(f.actor + 96, 2); // inactive delay
  captureScreenEffects(f.ram);
  bindScreenEffect(packet, f.prims + 4);
  EXPECT_EQ(packet.screenEffect.kind, MgsScreenEffectKind::None);
}

TEST(MgsScreenEffectsTest, FurnaceProfileRequiresMatchingOwnerNotRedPixels) {
  ScreenActorFixture f;
  f.install(3, "wt_view.c");
  f.word(f.actor + 32, 0); // map, not the common screen actor's prim pointer
  f.word(f.actor + 56, f.prims);
  f.word(f.actor + 60, 0x000a0a40);
  f.word(f.actor + 64, 1);
  f.word(f.actor + 72, 9);
  DrawPacket p;
  p.opcode = 0x64;
  p.vertexCount = 4;
  p.route = PacketRoute::Screen2D;
  captureScreenEffects(f.ram);
  bindWaterEffect(p, f.prims + 60);
  EXPECT_NE(p.waterEffect[0], 0u);
  EXPECT_FALSE(p.furnaceHeat);
  const uint32_t head = 0x800af390 + 5 * 68, furnace = 0x80130000;
  f.word(head + 4, furnace);
  f.word(furnace, head);
  f.word(furnace + 4, head + 32);
  f.word(head + 32, furnace);
  f.word(furnace + 8, 0x80070000);
  f.word(furnace + 20, 0x80120100);
  std::memcpy(f.ram.data() + 0x120100, "furnace.c", 10);
  f.word(furnace + 56, 0x000a0a40);
  captureScreenEffects(f.ram);
  bindWaterEffect(p, f.prims + 60);
  EXPECT_TRUE(p.furnaceHeat);
  f.word(furnace + 40, 1); // different volume
  captureScreenEffects(f.ram);
  bindWaterEffect(p, f.prims + 60);
  EXPECT_FALSE(p.furnaceHeat);
  f.word(furnace + 40, 0);
  f.word(furnace + 4, furnace); // stale/cyclic owner must not qualify
  captureScreenEffects(f.ram);
  bindWaterEffect(p, f.prims + 60);
  EXPECT_FALSE(p.furnaceHeat);
}
