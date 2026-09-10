#include "runtime/cpu_context.h"
#include "runtime/games/mgs_asset_cache.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/gpu/modern_replay.h"
#include "runtime/memory.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::games::mgs;
namespace {
void put(std::vector<uint8_t> &b, size_t p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) b.at(p + i) = v >> (i * 8);
}
void checksum(std::vector<uint8_t> &b) {
  uint32_t crc = ~0u;
  for (size_t i = 16; i < b.size(); ++i) {
    crc ^= b[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
  }
  put(b, 12, ~crc);
}
std::vector<uint8_t> sourceModel() {
  std::vector<uint8_t> b(178);
  put(b, 0, 1); put(b, 4, 1);
  put(b, 36, 1); put(b, 76, ~0u); put(b, 80, ~0u);
  put(b, 84, 4); put(b, 88, 120); put(b, 92, 152);
  put(b, 96, 1); put(b, 100, 156); put(b, 104, 164);
  put(b, 108, 168); put(b, 112, 176);
  for (unsigned v = 0; v < 4; ++v) {
    b[120 + v*8] = v*10;
    b[126 + v*8] = b[127 + v*8] = 255;
    b[152 + v] = v;
  }
  b[160] = 0; b[161] = 16; // normal Z = 4096
  return b;
}
std::vector<uint8_t> fixture() {
  std::array<std::vector<uint8_t>, 7> sections;
  sections[0].resize(64);
  put(sections[0],32,'k'); put(sections[0],40,1); put(sections[0],44,~0u);
  put(sections[0],52,178); put(sections[0],56,1);
  sections[1].resize(32);
  put(sections[1],4,~0u); put(sections[1],8,~0u); put(sections[1],16,4);
  put(sections[1],20,4); put(sections[1],28,1);
  sections[2].resize(4*48);
  for (unsigned v = 0; v < 4; ++v) {
    put(sections[2], v * 48, std::bit_cast<uint32_t>(float(v * 10)));
    put(sections[2], v * 48 + 12, std::bit_cast<uint32_t>(-1.0f));
  }
  sections[6] = sourceModel();
  constexpr const char *tags[] = {"ASST","MODL","VRTX","TEXR","TEXI","PLTE","SRCE"};
  constexpr uint32_t strides[] = {64,32,48,32,1,4,1};
  std::vector<uint8_t> b(128);
  std::memcpy(b.data(),"MGSAC001",8);
  for (size_t s = 0; s < 7; ++s) {
    b.resize((b.size()+15)&~size_t(15));
    std::memcpy(b.data()+16+s*16,tags[s],4);
    put(b,20+s*16,b.size()); put(b,24+s*16,sections[s].size()/strides[s]); put(b,28+s*16,strides[s]);
    b.insert(b.end(),sections[s].begin(),sections[s].end());
  }
  put(b,8,b.size()); checksum(b); return b;
}
void loadModel(Memory &memory, uint32_t address) {
  auto b=sourceModel();
  for (unsigned field : {56u,60u,68u,72u,76u,80u}) {
    const auto p=32+field;
    uint32_t v = uint32_t(b[p]) | uint32_t(b[p+1])<<8;
    put(b,p,address+v);
  }
  for (size_t i=0;i<b.size();++i) memory.write8(address+i,b[i]);
}
}
TEST(MgsAssetCacheTest, RejectsCorruptionAndOverlappingSections) {
  auto b=fixture();
  EXPECT_EQ(AssetCache::decode(b).section(AssetCache::Assets).count,1u);
  b.back()^=1;
  EXPECT_THROW(AssetCache::decode(b),std::runtime_error);
  b=fixture();put(b,20+16,128);checksum(b);
  EXPECT_THROW(AssetCache::decode(b),std::runtime_error);
  b=fixture();b.resize(64);
  EXPECT_THROW(AssetCache::decode(b),std::runtime_error);
}
TEST(MgsAssetCacheTest, BindingRequiresFullOriginalGeometryNotOnlyCounts) {
  auto cache=AssetCache::decode(fixture());
  Memory memory; loadModel(memory,0x80011000);
  EXPECT_EQ(matchOriginalDefinition(cache,memory,0x80011000),0u);
  memory.write8(0x80011000+120,99);
  EXPECT_EQ(matchOriginalDefinition(cache,memory,0x80011000),~0u);
}
TEST(MgsAssetCacheTest, CapturesBeforeCullingWithoutChangingGuestMemoryOrRegisters) {
  auto cache=AssetCache::decode(fixture());
  Memory memory;loadModel(memory,0x80011000);
  constexpr uint32_t channel=0x80010000, queue=0x80010100, object=0x80010200;
  memory.write16(channel+80,320);memory.write16(channel+86,1);memory.write32(channel+88,queue);
  memory.write32(queue,object);memory.write32(object+36,0x80011000);
  memory.write16(object+46,1);memory.write16(object+50,0); // old culling rejected it
  memory.write32(object+72+64,0x80011020);
  memory.write32(object+72+20,1234);
  recomp_context ctx{};ctx.mem=&memory;ctx.r4=channel;ctx.r5=1;
  std::array<uint32_t,32> registers{};
  std::copy_n(ctx.r,32,registers.begin());
  std::vector<uint8_t> before(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
  configure(true,true);setSceneAssetCache(&cache);
  captureSceneBeforeBounds(&ctx);
  const auto scene=latestSceneSnapshot();
  ASSERT_TRUE(scene);ASSERT_EQ(scene->instances.size(),1u);
  EXPECT_TRUE(scene->instances[0].visible);
  EXPECT_EQ(scene->instances[0].asset,0u);
  EXPECT_EQ(scene->instances[0].models[0].world[12],1234.0f);
  EXPECT_EQ(std::memcmp(before.data(),memory.ramPtr(),before.size()),0);
  for (size_t i=0;i<32;++i) EXPECT_EQ(ctx.r[i],registers[i]);
  // A partial or invalid next scene must never leave the old one renderable.
  memory.write32(queue,0x801fffff);
  captureSceneBeforeBounds(&ctx);
  EXPECT_FALSE(latestSceneSnapshot());
  setSceneAssetCache(nullptr);configure(false,false);
  EXPECT_FALSE(latestSceneSnapshot());
}
TEST(MgsAssetCacheTest, ValidatesLocallyExtractedUsaCache) {
  const char *p=std::getenv("PS1_TEST_MGS_ASSET_CACHE");
  if (!p) GTEST_SKIP()<<"optional original-disc fixture";
  const auto cache=AssetCache::load(p);
  EXPECT_GT(cache.section(AssetCache::Assets).count,1000u);
  EXPECT_GT(cache.section(AssetCache::Models).count,1000u);
}

TEST(MgsAssetCacheTest,
     OriginalMeshPacketUsesImmutableGpuOffsetsAndPreservesNativeState) {
  struct Mode {
    std::string previous;
    bool existed;
    Mode() : existed(std::getenv("PS1_MGS_MESH_REPLAY") != nullptr) {
      if (existed)
        previous = std::getenv("PS1_MGS_MESH_REPLAY");
      setenv("PS1_MGS_MESH_REPLAY", "1", 1);
    }
    ~Mode() {
      setSceneAssetCache(nullptr);
      configure(false, false);
      if (existed)
        setenv("PS1_MGS_MESH_REPLAY", previous.c_str(), 1);
      else
        unsetenv("PS1_MGS_MESH_REPLAY");
    }
  } mode;
  auto cache = AssetCache::decode(fixture());
  Memory memory;
  loadModel(memory, 0x80011000);
  constexpr uint32_t channel = 0x80010000, queue = 0x80010100,
                     object = 0x80010200, packs = 0x80013000;
  memory.write16(channel + 80, 320);
  memory.write16(channel + 86, 1);
  memory.write32(channel + 88, queue);
  memory.write32(queue, object);
  memory.write32(object + 36, 0x80011000);
  memory.write16(object + 46, 1);
  memory.write32(object + 72 + 64, 0x80011020);
  memory.write16(object + 72 + 82, 1);
  memory.write32(object + 72 + 84, packs);
  for (uint32_t offset : {0u, 8u, 16u}) {
    memory.write16(channel + 16 + offset, 4096);
    memory.write16(object + 72 + offset, 4096);
  }
  memory.write32(object + 72 + 28, 1000);
  memory.write32(object + 72 + 24, 100);
  gpu::replay::DrawPacket packet;
  packet.opcode = 0x3c;
  packet.vertexCount = 4;
  packet.indexCount = 6;
  packet.triangleIndices = {0, 1, 2, 1, 2, 3};
  packet.textured = packet.gouraud = true;
  packet.state.drawOffset = {160, 112};
  constexpr unsigned order[] = {0, 1, 3, 2};
  for (unsigned k = 0; k < 4; ++k) {
    const int x = int(order[k] * 3.2);
    memory.write32(packs + 4 + k * 12, 0x3c808080);
    memory.write32(packs + 8 + k * 12, x | (29u << 16));
    auto &v = packet.vertices[k];
    v.nativeX = 160 + x;
    v.nativeY = 141;
    v.color = {128, 128, 128};
    v.provenance.dmaNodeSource = (packs & 0x1fffffffu) + 1;
    v.provenance.gp0WordSource = (packs & 0x1fffffffu) + 8 + k * 12 + 1;
    v.provenance.evidence = gpu::replay::ReplayVertexDmaTraversalExact |
                            gpu::replay::ReplayVertexGp0WordSourceExact;
  }
  recomp_context ctx{};
  ctx.mem = &memory;
  ctx.r4 = channel;
  ctx.r5 = 0;
  configure(true, false);
  setSceneAssetCache(&cache);
  std::vector<uint8_t> before(memory.ramPtr(),
                              memory.ramPtr() + Memory::RAM_SIZE);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  auto wrong = packet;
  wrong.vertices[0].color.r = 0;
  const auto wrongBefore = wrong;
  EXPECT_FALSE(replaceMeshReplayPacket(wrong));
  EXPECT_EQ(wrong, wrongBefore);
  // A new float projection must not claim the native GTE event. Keeping that
  // evidence used to make the recorder reject mixed fallback frames.
  packet.vertices[0].provenance.projectionId = 42;
  packet.vertices[0].provenance.evidence |=
      gpu::replay::ReplayVertexProjectionEventExact;
  auto fallback = packet;
  fallback.opcode = 0x20;
  fallback.fallbackReason =
      gpu::replay::PacketFallbackReason::MissingPrecisionPacketAtomic;
  adaptMeshReplayPacket(fallback);
  EXPECT_EQ(fallback.vertices[0].provenance.projectionId, 0u);
  EXPECT_EQ(fallback.vertices[0].provenance.dmaNodeSource,
            packet.vertices[0].provenance.dmaNodeSource);
  EXPECT_FALSE(fallback.vertices[0].provenance.has(
      gpu::replay::ReplayVertexProjectionEventExact));
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  EXPECT_EQ(packet.vertices[0].provenance.projectionId, 0u);
  EXPECT_TRUE(packet.vertices[0].provenance.has(
      gpu::replay::ReplayVertexDmaTraversalExact));
  EXPECT_FALSE(packet.vertices[0].provenance.has(
      gpu::replay::ReplayVertexProjectionEventExact));
  ASSERT_TRUE(packet.originalMesh);
  EXPECT_DOUBLE_EQ(packet.vertices[1].x, 163.2);
  EXPECT_DOUBLE_EQ(packet.vertices[1].y, 141.0);
  EXPECT_DOUBLE_EQ(packet.vertices[2].x, 169.6);
  EXPECT_EQ(packet.originalMesh->pointBytes[2],
            cache.section(AssetCache::Vertices).offset + 3 * 48);
  EXPECT_FLOAT_EQ(packet.originalMesh->viewRows[2][3], 1000.0f);
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
  resetSceneCapture();
  EXPECT_FALSE(replaceMeshReplayPacket(packet));
}
