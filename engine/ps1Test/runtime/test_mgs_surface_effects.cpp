#include "runtime/games/mgs_surface_effects.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_screen_effects.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/metrics.h"
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::games::mgs;
using namespace ps1::gpu::replay;
namespace {
using SurfaceKind = MgsSurfaceKind;
struct AnimationSignature {
  const char *source, *symbol;
  uint16_t texture;
  std::array<uint16_t,3> atlas;
  SurfaceKind kind;
  std::span<const uint8_t> bytes;
};
#include "../../ps1Runtime/src/games/mgs_animation_signatures.inc"

class MgsSurfaceEffectsTest : public ::testing::Test {
protected:
  std::vector<uint8_t> ram = std::vector<uint8_t>(2*1024*1024);
  static constexpr uint32_t actor=0x80100000, prim=0x80110000,
                            packs=0x80120000, script=0x80130000;
  void word(uint32_t p,uint32_t v) { for(unsigned i=0;i<4;++i) ram[(p&0x1fffff)+i]=v>>(i*8); }
  void half(uint32_t p,uint16_t v) { ram[p&0x1fffff]=v;ram[(p&0x1fffff)+1]=v>>8; }
  void named(const char *name,unsigned level=6) {
    const uint32_t head=0x800af390+level*68;
    word(head+4,actor); word(actor,head); word(actor+4,head+32); word(head+32,actor);
    word(actor+8,0x80070000); word(actor+20,0x801f0000);
    std::strcpy(reinterpret_cast<char*>(ram.data()+0x1f0000),name);
  }
  void allocation() {
    word(prim+36,0x412);half(prim+42,1);half(prim+48,40);
    half(prim+50,4);half(prim+52,8);half(prim+54,8);
    word(prim+56,0x80118000);word(prim+60,actor+40);
    word(prim+64,packs);word(prim+68,packs+40);
    for(unsigned i=0;i<2;++i) {
      word(packs+i*40+4,0x2e808080);half(packs+i*40+14,0x7000);
      half(packs+i*40+22,0x20);
    }
  }
  DrawPacket packet() {
    DrawPacket p;p.opcode=0x2e;p.vertexCount=4;p.indexCount=6;p.textured=true;
    p.triangleIndices={0,1,2,1,2,3};p.state.texturePage.raw=0x20;p.state.clut.raw=0x7000;
    p.state.blend.enabled=true;p.state.blend.mode=BlendMode::BackgroundPlusForeground;
    for(unsigned k=0;k<4;++k) {
      p.vertices[k].u=10+(k&1)*63;p.vertices[k].v=20+(k>>1)*63;
      p.vertices[k].color={128,128,128};
    }
    return p;
  }
  void animation(const AnimationSignature &s) {
    ram.assign(ram.size(),0);named("anime.c");allocation();
    word(actor+32,0x80140000);half(0x80140000,s.texture);
    word(actor+36,prim);word(actor+72,0x80118000);half(actor+66,1);
    for(unsigned i=0;i<3;++i) half(actor+58+i*2,s.atlas[i]);
    std::copy(s.bytes.begin(),s.bytes.end(),ram.begin()+0x130000);
    const uint32_t start=script+uint32_t(s.bytes[3])*256+s.bytes[4];
    word(actor+76+20,start);word(actor+76+24,start);
  }
  void SetUp() override {configure(true,false);}
  void TearDown() override {configure(false,false);captureSurfaceEffects({});}
};
}

TEST_F(MgsSurfaceEffectsTest, FullBytecodeIdentityAndAtlasAreRequired) {
  for (const auto &s : animationSignatures) {
    SCOPED_TRACE(s.symbol);animation(s);
    const auto before=ram;
    captureSurfaceEffects(ram);
    EXPECT_EQ(ram,before);
    auto p=packet();bindSurfaceEffect(p,packs+4);
    EXPECT_EQ(p.surfaceEffect.kind,s.kind);
    EXPECT_EQ(p.surfaceEffect.actor,actor&0x1fffff);
    // A reused address with just one changed script byte is a different
    // animation even if the texture, atlas and packet look identical.
    ram[0x130001]^=0x40;
    captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
    EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::None);
    animation(s);half(actor+58,s.atlas[0]+1);
    captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
    EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::None);
  }
}

TEST_F(MgsSurfaceEffectsTest, OwnershipSnapshotIsImmutableAndReloadClearsIt) {
  named("spark.c",5);allocation();word(actor+36,prim);
  captureSurfaceEffects(ram);
  ram.assign(ram.size(),0);
  auto p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::Spark);
  auto wrong=packet();bindSurfaceEffect(wrong,packs+5);
  EXPECT_EQ(wrong.surfaceEffect.kind,MgsSurfaceKind::None);
  wrong=packet();wrong.state.clut.raw++;
  bindSurfaceEffect(wrong,packs+4);
  EXPECT_EQ(wrong.surfaceEffect.kind,MgsSurfaceKind::None);
  captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, BubblesPreserveAuthoredDepthOffsetWithoutMovingGeometry) {
  named("bubble_p.c");allocation();word(actor+40,prim);
  half(packs+22,0x60);half(packs+40+22,0x60);
  for (const int offset : {3000,300,0,-200}) {
    half(prim+46,offset);
    const auto original=ram;
    captureSurfaceEffects(ram);
    auto p=packet();const auto vertex=p.vertices[0];
    p.state.texturePage.raw=0x60;
    p.state.blend.mode=BlendMode::BackgroundPlusQuarterForeground;
    bindSurfaceEffect(p,packs+4);
    ASSERT_EQ(p.surfaceEffect.kind,SurfaceKind::Bubble);
    EXPECT_EQ(p.surfaceEffect.parameters[2][0],float(offset));
    EXPECT_EQ(p.state.blend.mode,BlendMode::BackgroundPlusQuarterForeground);
    EXPECT_EQ(p.state.texturePage.raw & 0x60u,0x60u);
    EXPECT_EQ(p.vertices[0].x,vertex.x);
    EXPECT_EQ(p.vertices[0].w,vertex.w);
    EXPECT_EQ(ram,original);
  }
}

TEST_F(MgsSurfaceEffectsTest, AuthoredParticlesRetainSignedOrderingDepth) {
  for(const auto &signature : animationSignatures) {
    SCOPED_TRACE(signature.symbol);animation(signature);
    for(const int offset : {2000,0,-200}) {
      half(prim+46,offset);const auto original=ram;
      captureSurfaceEffects(ram);auto p=packet();const auto vertex=p.vertices[0];
      bindSurfaceEffect(p,packs+4);
      ASSERT_EQ(p.surfaceEffect.kind,signature.kind);
      EXPECT_EQ(p.surfaceEffect.parameters[2][0],float(offset));
      EXPECT_EQ(p.vertices[0].x,vertex.x);EXPECT_EQ(p.vertices[0].w,vertex.w);
      EXPECT_EQ(ram,original);
    }
  }
}

TEST_F(MgsSurfaceEffectsTest, ExplosionVolumeUsesLiveCenteredExtentNotOrderingOffset) {
  for (const auto &signature : animationSignatures) {
    if (signature.kind != SurfaceKind::Explosion) continue;
    SCOPED_TRACE(signature.symbol);
    animation(signature);
    for (const auto size : {std::array<int,2>{500,500}, {3400,3400}, {2000,1200}}) {
      half(actor+40,size[0]/2);half(actor+42,size[1]/2);
      half(actor+44,size[0]);half(actor+46,size[1]);
      for (int raise : {2000,0,-200}) {
        half(prim+46,raise);const auto original=ram;
        captureSurfaceEffects(ram);auto p=packet();const auto vertex=p.vertices[0];
        bindSurfaceEffect(p,packs+4);
        ASSERT_EQ(p.surfaceEffect.kind,SurfaceKind::Explosion);
        EXPECT_FLOAT_EQ(p.surfaceEffect.parameters[2][1],size[0]*0.5f);
        EXPECT_FLOAT_EQ(p.surfaceEffect.parameters[2][2],size[1]*0.5f);
        EXPECT_EQ(p.vertices[0].x,vertex.x);EXPECT_EQ(p.vertices[0].w,vertex.w);
        EXPECT_EQ(ram,original);
      }
    }
    // An uncentered, invalid or camera-space allocation does not invent a
    // physical radius. It retains conservative geometric-plane occlusion.
    for (unsigned invalid=0;invalid<4;++invalid) {
      half(actor+40,500);half(actor+42,500);half(actor+44,1000);half(actor+46,1000);
      word(prim+60,actor+40);word(prim+36,0x412);
      if(invalid==0) half(actor+40,100);
      if(invalid==1) half(actor+44,uint16_t(-100));
      if(invalid==2) word(prim+60,0);
      if(invalid==3) word(prim+36,0x612);
      captureSurfaceEffects(ram);auto p=packet();bindSurfaceEffect(p,packs+4);
      EXPECT_FLOAT_EQ(p.surfaceEffect.parameters[2][1],0.f);
      EXPECT_FLOAT_EQ(p.surfaceEffect.parameters[2][2],0.f);
    }
  }
}

TEST_F(MgsSurfaceEffectsTest, CyclicListAndHiddenOrMalformedPrimitiveNeverBind) {
  named("spark.c",5);allocation();word(actor+36,prim);
  word(actor+4,actor);
  captureSurfaceEffects(ram);auto p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::None);
  named("spark.c",5);word(prim+36,0x512);
  captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::None);
  word(prim+36,0x412);half(prim+48,52);
  captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, BubblePlumeUsesAllThreeAuthoredClusterLayouts) {
  named("bubble_p.c");allocation();word(actor+40,prim);
  half(prim+42,3);word(prim+68,packs+120);
  for(unsigned i=0;i<6;++i) {
    word(packs+i*40+4,0x2e808080);half(packs+i*40+14,0x7000);
    half(packs+i*40+22,0x20);
  }
  captureSurfaceEffects(ram);
  for(unsigned i=0;i<6;++i) {
    auto p=packet();bindSurfaceEffect(p,packs+i*40+4);
    ASSERT_EQ(p.surfaceEffect.kind,SurfaceKind::Bubble);
    EXPECT_EQ(p.surfaceEffect.parameters[2][1],float(i%3+1));
  }
  // Single air bubbles use awa_s, not the plume's three cluster textures.
  named("bubble_s.c");captureSurfaceEffects(ram);
  auto p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.parameters[2][1],0.f);
}

TEST_F(MgsSurfaceEffectsTest, StunLensRequiresCompleteOwnedCameraArray) {
  named("stngrnd.c",5);allocation();
  const std::vector<uint8_t> base(ram.begin()+0x110000,ram.begin()+0x110054);
  constexpr std::array<unsigned,8> sizes{8,34,6,25,12,70,11,50};
  word(actor+0xe8,15);word(actor+0xf4,1);
  for(unsigned i=0;i<8;++i) {
    const auto p=prim+i*0x100,q=packs+i*80,rect=actor+0x20+i*8;
    std::copy(base.begin(),base.end(),ram.begin()+(p&0x1fffff));
    word(actor+0x60+i*4,p);word(p+36,0x612);
    word(p+56,actor+0xa0+i*8);word(p+60,rect);
    half(rect,sizes[i]/2);half(rect+2,sizes[i]/2);
    half(rect+4,sizes[i]);half(rect+6,sizes[i]);
    word(p+64,q);word(p+68,q+40);
    for(unsigned b=0;b<2;++b) {
      word(q+b*40+4,0x2e808080);half(q+b*40+14,0x7000);half(q+b*40+22,0x20);
    }
  }
  const auto original=ram;captureSurfaceEffects(ram);EXPECT_EQ(ram,original);
  for(unsigned i=0;i<16;++i) {
    auto p=packet();bindSurfaceEffect(p,packs+i*40+4);
    EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::Glow);
    EXPECT_EQ(p.surfaceEffect.parameters[2][2],1.f); // optical, no world-depth fade
    EXPECT_EQ(p.vertices[0].u,10.f); // original atlas coordinates retained
  }
  for(unsigned defect=0;defect<5;++defect) {
    ram=original;
    if(defect==0) word(actor+0xe8,0); // expired actor
    if(defect==1) word(actor+0xf4,0); // incomplete GetResources
    if(defect==2) word(prim+7*0x100+56,actor+0xa0); // wrong owner/index
    if(defect==3) half(actor+0x20+7*8+4,51); // different optical layout
    if(defect==4) word(actor+4,actor); // retired/cyclic list
    captureSurfaceEffects(ram);
    auto p=packet();bindSurfaceEffect(p,packs+4);
    EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None) << defect;
  }
}

TEST_F(MgsSurfaceEffectsTest, StaticEmitterDoesNotRequireAnActFunction) {
  named("emitter.c",5);allocation();word(actor+32,prim);
  word(actor+8,0);word(actor+12,0x80071000);
  captureSurfaceEffects(ram);auto p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,MgsSurfaceKind::Glow);
}

TEST_F(MgsSurfaceEffectsTest, OpticalMaterialsRequireLiveParentAndThermalDisablesCamo) {
  named("kogaku2.c",1);
  word(actor+32,0x80150000);word(0x80150000,0x80160000);
  word(actor+44,33);
  captureSurfaceEffects(ram);
  auto effect=surfaceEffectForObject(0x80160000);
  EXPECT_EQ(effect.kind,MgsSurfaceKind::Camo);
  EXPECT_EQ(effect.parameters[0][2],33.f);
  EXPECT_EQ(surfaceEffectForObject(0x80160004).kind,MgsSurfaceKind::None);
  word(0x800adb3c,8);captureSurfaceEffects(ram);
  EXPECT_EQ(surfaceEffectForObject(0x80160000).kind,MgsSurfaceKind::None);
  word(0x800adb3c,0);word(actor+4,actor);captureSurfaceEffects(ram);
  EXPECT_EQ(surfaceEffectForObject(0x80160000).kind,MgsSurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, SepiaAndMonotoneAreSelectedByActorResourcesNotGrayPixels) {
  named("sepia.c",7);word(actor+32,prim);
  word(prim+20,0x62002040);word(prim+24,0xfff1ff80);word(prim+28,0x001e0100);
  captureSurfaceEffects(ram);
  EXPECT_EQ(surfaceEffectForObject(0).parameters[3][3],1.f);
  word(actor+32,0);word(0x80070000,0x03e00008);word(0x80070004,0);
  captureSurfaceEffects(ram);
  EXPECT_EQ(surfaceEffectForObject(0).parameters[3][3],2.f);
  word(0x80070004,1);captureSurfaceEffects(ram);
  EXPECT_EQ(surfaceEffectForObject(0).parameters[3][3],0.f);
}

TEST_F(MgsSurfaceEffectsTest, PhotographRequiresObservedShutterTransitionAndIsConsumedOnce) {
  named("jpegcam.c",6);word(actor+32,0x80150000);word(actor+36,0x80160000);
  word(actor+0x70,1);word(actor+0x64,4);word(0x800adb3c,0x400);
  captureSurfaceEffects(ram);EXPECT_EQ(consumePhotoCaptureEvent(),0u);
  word(actor+0x64,3);captureSurfaceEffects(ram);
  const auto original=ram;
  word(actor+0x64,4);captureSurfaceEffects(ram);
  const auto event=consumePhotoCaptureEvent();EXPECT_GT(event,0u);
  EXPECT_EQ(consumePhotoCaptureEvent(),0u);
  captureSurfaceEffects(ram);EXPECT_EQ(consumePhotoCaptureEvent(),0u);
  word(actor+0x64,3);captureSurfaceEffects(ram);
  resetSceneCapture();
  word(actor+0x64,4);captureSurfaceEffects(ram);
  EXPECT_EQ(consumePhotoCaptureEvent(),0u); // restored shutter is not a new photograph
  word(actor+0x64,3);captureSurfaceEffects(ram);
  word(actor+0x64,4);word(actor+4,actor);captureSurfaceEffects(ram);
  EXPECT_EQ(consumePhotoCaptureEvent(),0u); // recycled/cyclic actor is not a shutter
}

TEST_F(MgsSurfaceEffectsTest, HideoRequiresOwnedRetailPlateAndKeepsLogoAspect) {
  configure(true,true);named("psyco.c",5);
  const uint32_t plate=actor+0x9d4,logo=actor+0x934;
  word(plate+4,0x28000000);word(plate+8,0);word(plate+12,320);
  word(plate+16,224u<<16);word(plate+20,(224u<<16)|320);
  word(logo+4,0x2c808080);
  const auto original=ram;captureSurfaceEffects(ram);EXPECT_EQ(ram,original);
  auto p=packet();p.opcode=0x28;p.textured=false;p.state.blend.enabled=false;
  for(auto &v:p.vertices) v.color={0,0,0};
  bindSurfaceEffect(p,plate+4);EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::SignalLoss);
  p=packet();p.opcode=0x2c;p.route=PacketRoute::Screen2D;
  p.vertices[0].x=200;bindInterfaceGeometry(p,logo+4);
  EXPECT_DOUBLE_EQ(p.vertices[0].x,230);
  word(plate+12,319);captureSurfaceEffects(ram);
  p=packet();p.opcode=0x28;p.textured=false;
  for(auto &v:p.vertices) v.color={0,0,0};
  bindSurfaceEffect(p,plate+4);EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, OriginalSightDirectoryControlsOnlyItsOwnGeometry) {
  configure(true,true);named("sight.c",6);
  constexpr uint32_t info=0x80150000,entries=0x80151000,source=0x80152000;
  word(actor+52,info);word(actor+56,packs);half(info,24);
  ram[(info&0x1fffff)+3]=1;word(info+4,entries);word(info+8,source);
  half(entries+2,0);word(source,5);word(source+4,0x280000ff);word(packs+4,0x280000ff);
  captureSurfaceEffects(ram);
  auto p=packet();p.opcode=0x28;p.route=PacketRoute::Screen2D;
  p.state.drawOffset.x=160;p.vertices[0].x=100;
  bindInterfaceGeometry(p,packs+4);EXPECT_DOUBLE_EQ(p.vertices[0].x,115);
  p.vertices[0].x=100;bindInterfaceGeometry(p,packs+8);
  EXPECT_DOUBLE_EQ(p.vertices[0].x,100);
  word(source,255);captureSurfaceEffects(ram);bindInterfaceGeometry(p,packs+4);
  EXPECT_DOUBLE_EQ(p.vertices[0].x,100); // text pseudo-record is not a primitive
}

TEST_F(MgsSurfaceEffectsTest, UsaMosaicOwnsBothBuffersAndRejectsIntegralStubOrWrongLayout) {
  named("mosaic.c",3);word(actor+72,prim);word(actor+80,2);word(actor+84,2);
  word(actor+88,8);word(actor+92,8);word(actor+96,1);
  half(actor+104,120);half(actor+106,90);
  for(unsigned b=0;b<2;++b) {
    word(prim+56+b*4,packs+b*160);word(prim+24+b*16+4,0x60000000);
    for(unsigned i=0;i<4;++i){word(packs+b*160+i*40+4,0x2c808080);half(packs+b*160+i*40+22,0x100);}
  }
  auto p=packet();p.opcode=0x2c;p.state.blend.enabled=false;p.state.clut.raw=0;
  p.state.texturePage.raw=0x100;
  const auto original=ram;captureSurfaceEffects(ram);EXPECT_EQ(original,ram);
  bindSurfaceEffect(p,packs+164);EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::Mosaic);
  EXPECT_EQ(p.surfaceEffect.parameters[2][0],8.f);
  auto tile=packet();tile.opcode=0x60;tile.textured=false;tile.state.blend.enabled=false;
  for(auto &v:tile.vertices){v.color={0,0,0};v.u=v.v=0;}
  bindSurfaceEffect(tile,prim+28);EXPECT_EQ(tile.surfaceEffect.kind,SurfaceKind::Mosaic);
  word(packs+4,0x2c808081);captureSurfaceEffects(ram);p.surfaceEffect={};
  bindSurfaceEffect(p,packs+4);EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
  word(actor+72,0);captureSurfaceEffects(ram);tile.surfaceEffect={};
  bindSurfaceEffect(tile,prim+28);EXPECT_EQ(tile.surfaceEffect.kind,SurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, CodecCrtRequiresOwnedPortraitAndExactVideoLayout) {
  named("menuman.c",8);word(actor+32,prim);
  word(prim,packs+40);word(prim+8,packs+400);word(prim+12,packs);
  word(packs,0x09000000);word(packs+4,0x2e808080);
  word(packs+8,31|(30<<16));word(packs+16,83|(30<<16));
  word(packs+24,31|(119<<16));word(packs+32,83|(119<<16));
  word(packs+12,(80<<8)|(0x4030u<<16));word(packs+20,52|(80<<8)|(0xbfu<<16));
  word(packs+28,169<<8);word(packs+36,52|(169<<8));
  const auto before=ram;
  captureSurfaceEffects(ram);
  auto p=packet();p.state.texturePage.raw=0xbf;p.state.clut.raw=0x4030;
  bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::CodecVideo);
  EXPECT_EQ(ram,before);
  // Same texture outside the menu allocation, or a different CLUT/UV shape,
  // must not turn subtitles, inventory or world materials into a CRT.
  auto other=packet();other.state.texturePage.raw=0xbf;other.state.clut.raw=0x4030;
  bindSurfaceEffect(other,packs+44);EXPECT_EQ(other.surfaceEffect.kind,SurfaceKind::None);
  half(packs+28,168<<8);captureSurfaceEffects(ram);
  p.surfaceEffect={};bindSurfaceEffect(p,packs+4);EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
  ram=before;word(prim,packs+36);captureSurfaceEffects(ram);
  p.surfaceEffect={};bindSurfaceEffect(p,packs+4);EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, RadarNoiseRequiresJammingStateAndOwnedMenuAllocation) {
  named("menuman.c",8);word(actor+32,prim);word(0x800adb3c,1);
  ram[(actor&0x1fffff)+0x1d8]=1;ram[(actor&0x1fffff)+0x1d9]=1;
  word(prim,packs+16);word(prim+8,packs+4096);word(prim+12,packs);
  word(packs,0x03ffffff);word(packs+4,0x6248a000);
  word(packs+8,0xffe6ffde);word(packs+12,0x00340045);
  auto p=packet();p.opcode=0x62;p.textured=false;
  for(auto &v:p.vertices){v.color={0,160,72};v.u=v.v=0;}
  captureSurfaceEffects(ram);bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::RadarJamming);
  word(0x800adb3c,0);captureSurfaceEffects(ram);p.surfaceEffect={};bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
  word(0x800adb3c,1);word(prim,packs+100000);captureSurfaceEffects(ram);bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
}

TEST_F(MgsSurfaceEffectsTest, SnowStormAllowsOnlyTheConstructedLifetimeOperand) {
  animation(animationSignatures[0]);
  constexpr std::array<uint8_t,21> bytes{0,16,1,0,5,1,254,12,0,9,1,255,8,0,0,0,2,0,1,13,15};
  uint16_t hash=0;for(unsigned char c:std::string_view("snow_ex1")) hash=uint16_t((hash<<5)|(hash>>11))+c;
  half(0x80140000,hash);half(actor+58,1);half(actor+60,1);half(actor+62,1);
  std::copy(bytes.begin(),bytes.end(),ram.begin()+0x130000);word(actor+96,script+5);word(actor+100,script+5);
  captureSurfaceEffects(ram);auto p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::SnowStorm);
  ram[0x130008]=23;captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::SnowStorm);
  ram[0x130010]=3;captureSurfaceEffects(ram);p=packet();bindSurfaceEffect(p,packs+4);
  EXPECT_EQ(p.surfaceEffect.kind,SurfaceKind::None);
}

TEST(MgsSurfaceEffectsSnapshotTest, OptInRetailFogFollowsRelocatedCodeAndZoomParameters) {
  const char *path=std::getenv("PS1_TEST_MGS_FOG_RAM");
  if(!path) GTEST_SKIP()<<"set PS1_TEST_MGS_FOG_RAM to a retail fog scene snapshot";
  std::ifstream input(path,std::ios::binary);
  std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});
  ASSERT_EQ(ram.size(),2u*1024*1024);
  struct Cleanup {~Cleanup(){configure(false,false);captureSurfaceEffects({});}} cleanup;
  configure(true,false);captureSurfaceEffects(ram);
  auto effect=surfaceEffectForObject(0);
  EXPECT_EQ(effect.parameters[3][2],1.f);
  EXPECT_GT(effect.parameters[3][0],0.f);
  EXPECT_GT(effect.parameters[3][1],effect.parameters[3][0]);
  EXPECT_LE(effect.parameters[3][1],255.f*256);
  // Invalid opcode in the relocated updater must disable attribution.
  // Find the updater by its distinctive first 16 bytes, not a USA address.
  const std::array<uint8_t,16> prefix{0x00,0x01,0x82,0x28,0x02,0x00,0x40,0x14,
                                     0,0,0,0,0xff,0x00,0x04,0x24};
  unsigned changes=0;
  for(size_t i=0x10000;i+prefix.size()<ram.size();i+=4)
    if(!std::memcmp(ram.data()+i,prefix.data(),prefix.size())){ram[i]^=1;changes++;}
  ASSERT_GT(changes,0u);captureSurfaceEffects(ram);
  EXPECT_EQ(surfaceEffectForObject(0).parameters[3][2],0.f);
}

TEST(MgsSurfaceEffectsSnapshotTest, OneReferencePerActorAndNewAfterInterleavedDraw) {
  ScreenReadTracker tracker;
  ScreenRead read{{0,0,640,224},{0,0,320,224},0x100000,true};
  EXPECT_TRUE(tracker.needsSnapshot(read));
  EXPECT_FALSE(tracker.needsSnapshot(read));
  read.group++;
  EXPECT_TRUE(tracker.needsSnapshot(read));
  tracker.invalidate();
  EXPECT_TRUE(tracker.needsSnapshot(read));
  read.group=0;
  EXPECT_TRUE(tracker.needsSnapshot(read));
}

TEST(MgsSurfaceEffectsSnapshotTest, OptInRetailRamProvesAnimationLayout) {
  const char *path=std::getenv("PS1_TEST_MGS_EFFECT_RAM");
  if(!path) GTEST_SKIP()<<"set PS1_TEST_MGS_EFFECT_RAM to a retail scene snapshot";
  std::ifstream input(path,std::ios::binary);
  std::vector<uint8_t> ram((std::istreambuf_iterator<char>(input)),{});
  ASSERT_EQ(ram.size(),2u*1024*1024);
  struct Cleanup {~Cleanup(){configure(false,false);captureSurfaceEffects({});metrics::reset();metrics::setEnabledForTesting(false);}} cleanup;
  configure(true,false);metrics::reset();metrics::setEnabledForTesting(true);
  captureSurfaceEffects(ram);
  EXPECT_GT(metrics::get("mgs.effects.animation.identified"),0u);
  EXPECT_EQ(metrics::get("mgs.effects.animation.unknown"),0u);
}
