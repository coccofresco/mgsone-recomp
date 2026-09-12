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
std::vector<uint8_t>
packFixture(const std::array<std::vector<uint8_t>, 9> &sections, bool names) {
  constexpr const char *tags[] = {"ASST", "MODL", "VRTX", "TEXR", "TEXI",
                                  "PLTE", "SRCE", "NAME", "TMPL"};
  constexpr uint32_t strides[] = {64, 32, 48, 32, 1, 4, 1, 8, 16};
  const size_t count = names ? 9 : 7;
  std::vector<uint8_t> b(16 + count * 16);
  std::memcpy(b.data(), names ? "MGSAC002" : "MGSAC001", 8);
  for (size_t s = 0; s < count; ++s) {
    b.resize((b.size() + 15) & ~size_t(15));
    std::memcpy(b.data() + 16 + s * 16, tags[s], 4);
    put(b, 20 + s * 16, b.size());
    put(b, 24 + s * 16, sections[s].size() / strides[s]);
    put(b, 28 + s * 16, strides[s]);
    b.insert(b.end(), sections[s].begin(), sections[s].end());
  }
  put(b, 8, b.size());
  checksum(b);
  return b;
}
std::vector<uint8_t> fixture(bool names = false) {
  std::array<std::vector<uint8_t>, 9> sections;
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
  if (names) {
    sections[7].resize(8);
    put(sections[7], 0, 0x1234);
    sections[8].resize(64);
    for (unsigned k = 0; k < 4; ++k) {
      put(sections[8], k * 16, std::bit_cast<uint32_t>(float(k & 1)));
      put(sections[8], k * 16 + 4, std::bit_cast<uint32_t>(float(k >> 1)));
      put(sections[8], k * 16 + 12, 0x3f800000);
    }
  }
  return packFixture(sections, names);
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
  // A distinct presentation pose must not contaminate the native packet
  // oracle, including its derived subdivision faces. Use a deliberate large
  // difference so an accidental oracle comparison against it fails the test.
  for (unsigned offset = 0; offset < 32; offset += 4)
    memory.write32(object + offset, memory.read32(object + 72 + offset));
  memory.write32(object + 40, 0x40);
  memory.write32(object + 20, 100);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  auto modernPacket = packet;
  ASSERT_TRUE(replaceMeshReplayPacket(modernPacket));
  EXPECT_DOUBLE_EQ(modernPacket.vertices[1].x, 195.2);
  EXPECT_FLOAT_EQ(modernPacket.originalMesh->viewRows[0][3], 100);
  // Trace one subdivision cell back to the four immutable parent corners.
  beginMeshDivide(&ctx);
  memory.write32(0x1f800034, 0x1f800038);
  meshDivideGrid(&ctx);
  memory.write32(0x1f80001c, packs);
  memory.write32(0x1f800034, 0x1f800038 + 9 * 20);
  ctx.r4 = 0x1f800038;
  meshDivideCopy(&ctx);
  constexpr uint32_t child = 0x80014000;
  ctx.r7 = child;
  ctx.r16 = 0x1f800038;
  meshDividePacket(&ctx);
  auto childPacket = packet;
  childPacket.originalMesh.reset();
  constexpr int childX[] = {0, 1, 4, 4};
  for (unsigned k = 0; k < 4; ++k) {
    memory.write32(child + 4 + k * 12, 0x3c808080);
    memory.write32(child + 8 + k * 12, childX[k] | (29u << 16));
    auto &v = childPacket.vertices[k];
    v.nativeX = 160 + childX[k];
    v.provenance.dmaNodeSource = (child & 0x1fffff) + 1;
    v.provenance.gp0WordSource = (child & 0x1fffff) + 9 + k * 12;
  }
  ctx.r4 = channel;
  prepareMeshReplay(&ctx);
  ASSERT_TRUE(replaceMeshReplayPacket(childPacket));
  ASSERT_TRUE(childPacket.originalMesh);
  EXPECT_EQ(childPacket.originalMesh->pointBytes,
            packet.originalMesh->pointBytes);
  EXPECT_EQ(childPacket.originalMesh->weights[3],
            (std::array<float, 4>{.25f, .25f, .25f, .25f}));
  EXPECT_NEAR(childPacket.vertices[1].x, 193.6, 1e-5);
  // A source behind the eye remains a cache draw: the GPU clips its true
  // signed eye coordinates. Native GTE saturation is only the oracle.
  memory.write32(object + 40, 0);
  memory.write32(object + 72 + 28, uint32_t(-1000));
  for (unsigned k = 0; k < 4; ++k) {
    const int x = int(order[k] * 10 * (131071.0 / 65536.0));
    memory.write32(packs + 8 + k * 12, x | (181u << 16));
    packet.vertices[k].nativeX = 160 + x;
    packet.vertices[k].nativeY = 112 + 181;
  }
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  EXPECT_DOUBLE_EQ(packet.vertices[0].rawDepth, -1000);
  EXPECT_DOUBLE_EQ(packet.vertices[0].w, 1);
  EXPECT_TRUE(packet.vertices[0].nearSaturated);
  EXPECT_FALSE(packet.vertices[0].inventedW);
  EXPECT_FLOAT_EQ(packet.originalMesh->viewRows[2][3], -1000);
  resetSceneCapture();
  EXPECT_FALSE(replaceMeshReplayPacket(packet));
  EXPECT_FALSE(replaceMeshReplayPacket(childPacket));
}

TEST(MgsAssetCacheTest,
     GeometryBindingSeparatesMaterialChangesAndRejectsTopologyChanges) {
  auto cache = AssetCache::decode(fixture());
  Memory memory;
  constexpr uint32_t definition = 0x80011000;
  loadModel(memory, definition);
  memory.write32(definition + 32, 2);       // shading flag
  memory.write16(definition + 176, 0x9876); // runtime texture ID override
  bool deformed = false;
  EXPECT_EQ(matchOriginalDefinition(cache, memory, definition), ~0u);
  EXPECT_EQ(matchSceneDefinition(cache, memory, definition, deformed), 0u);
  EXPECT_FALSE(deformed);
  memory.write8(definition + 120, 99);
  EXPECT_EQ(matchSceneDefinition(cache, memory, definition, deformed), ~0u);
  memory.write8(definition + 120, 0);
  memory.write8(definition + 152, 1); // same counts, different face
  EXPECT_EQ(matchSceneDefinition(cache, memory, definition, deformed), ~0u);
}
TEST(MgsAssetCacheTest, DeformationRequiresLiveActorAndExactSavedOriginals) {
  auto cache = AssetCache::decode(fixture(true));
  EXPECT_EQ(cache.section(AssetCache::Names).count, 1u);
  Memory memory;
  constexpr uint32_t definition = 0x80011000, actor = 0x80012000,
                     name = 0x80012100, points = 0x80012200, head = 0x800AF4E4;
  loadModel(memory, definition);
  memory.write32(0x800AF668 + 0x34 * 8, 0xa1234);
  memory.write32(0x800AF668 + 0x34 * 8 + 4, definition);
  memory.write32(head + 4, actor);
  memory.write32(actor, head);
  memory.write32(actor + 20, name);
  memory.write32(actor + 44, 1);
  memory.write32(actor + 56, definition);
  memory.write32(actor + 124, points);
  const char filename[] = "shakemdl.c";
  for (uint32_t i = 0; i < sizeof(filename); ++i)
    memory.write8(name + i, filename[i]);
  memory.write16(definition + 122, 15);
  bool deformed = false;
  EXPECT_EQ(matchOriginalDefinition(cache, memory, definition), ~0u);
  EXPECT_EQ(matchSceneDefinition(cache, memory, definition, deformed), 0u);
  EXPECT_TRUE(deformed);
  memory.write16(points, 17); // actor baseline no longer matches original
  EXPECT_EQ(matchSceneDefinition(cache, memory, definition, deformed), ~0u);
  memory.write16(points, 0);
  memory.write32(head + 4,
                 0); // matching file ID alone cannot permit deformations
  EXPECT_EQ(matchSceneDefinition(cache, memory, definition, deformed), ~0u);
}

TEST(MgsAssetCacheTest, ProceduralBillboardUsesResidentTemplateAndWorldDepth) {
  setenv("PS1_MGS_MESH_REPLAY", "1", 1);
  struct Cleanup {
    ~Cleanup() {
      setSceneAssetCache(nullptr);
      configure(false, false);
      unsetenv("PS1_MGS_MESH_REPLAY");
    }
  } cleanup;
  const auto cache = AssetCache::decode(fixture(true));
  Memory memory;
  constexpr uint32_t channel = 0x80010000, queue = 0x80010100,
                     prim = 0x80010200, positions = 0x80010300,
                     rect = 0x80010400, packs = 0x80010500;
  memory.write16(channel + 80, 320);
  memory.write16(channel + 82, 1);
  memory.write32(channel + 88, queue);
  memory.write32(queue, prim);
  memory.write32(prim + 36, 0x612); // camera-space rectangular FT4
  memory.write16(prim + 42, 1);
  memory.write16(prim + 48, 40);
  memory.write16(prim + 50, 4);
  memory.write16(prim + 52, 8);
  memory.write16(prim + 54, 8);
  memory.write32(prim + 56, positions);
  memory.write32(prim + 60, rect);
  memory.write32(prim + 64, packs);
  memory.write16(positions, 100);
  memory.write16(positions + 2, 50);
  memory.write16(positions + 4, 1000);
  memory.write16(rect, 20);
  memory.write16(rect + 2, 10);
  memory.write16(rect + 4, 40);
  memory.write16(rect + 6, 20);
  memory.write32(packs + 4, 0x2c808080);
  gpu::replay::DrawPacket packet;
  packet.opcode = 0x2c;
  packet.textured = true;
  packet.vertexCount = 4;
  packet.indexCount = 6;
  packet.triangleIndices = {0, 1, 2, 1, 2, 3};
  packet.state.drawOffset = {160, 112};
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t x = (k & 1) ? 38 : 25, y = (k >> 1) ? 19 : 12;
    memory.write32(packs + 8 + k * 8, x | (y << 16));
    auto &v = packet.vertices[k];
    v.nativeX = 160 + x;
    v.nativeY = 112 + y;
    v.color = {128, 128, 128};
    v.provenance.dmaNodeSource = (packs & 0x1fffff) + 1;
    v.provenance.gp0WordSource = (packs & 0x1fffff) + 9 + k * 8;
    v.provenance.evidence = gpu::replay::ReplayVertexDmaTraversalExact |
                            gpu::replay::ReplayVertexGp0WordSourceExact;
  }
  recomp_context ctx{};
  ctx.mem = &memory;
  ctx.r4 = channel;
  configure(true, false);
  setSceneAssetCache(&cache);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  ASSERT_TRUE(packet.originalMesh);
  EXPECT_EQ(packet.originalMesh->pointBytes[3],
            cache.section(AssetCache::Templates).offset + 48);
  EXPECT_DOUBLE_EQ(packet.vertices[0].x, 185.6);
  EXPECT_DOUBLE_EQ(packet.vertices[0].y, 124.8);
  EXPECT_DOUBLE_EQ(packet.vertices[3].w, 1000);
  EXPECT_FLOAT_EQ(packet.originalMesh->viewRows[0][0], 40);
  EXPECT_FLOAT_EQ(packet.originalMesh->viewRows[1][1], 20);
  // World-space effects share the model's modern 16:9 projection, even
  // though the reference primitive routine bypasses native DG_AdjustOverscan.
  memory.write32(prim + 36, 0x412);
  for (uint32_t offset : {0u, 8u, 16u}) {
    memory.write16(prim + offset, 4096);
    memory.write16(channel + 16 + offset, 4096);
  }
  configure(true, true);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  EXPECT_NEAR(packet.vertices[0].x, 179.2, 1e-5);
  EXPECT_FLOAT_EQ(packet.originalMesh->viewRows[0][0], 30);
  // DG billboards clamp the anchor first, then add/project the rectangle and
  // clamp again. The modern position must not inherit either saturation.
  memory.write32(prim + 36, 0x612);
  configure(true, false);
  memory.write16(positions, 10000);
  memory.write16(positions + 2, 10000);
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t x = (k & 1) ? 1023 : 1016, y = (k >> 1) ? 1023 : 1019;
    memory.write32(packs + 8 + k * 8, x | (y << 16));
    packet.vertices[k].nativeX = 160 + x;
    packet.vertices[k].nativeY = 112 + y;
  }
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  EXPECT_DOUBLE_EQ(packet.vertices[0].x, 3353.6);
  EXPECT_DOUBLE_EQ(packet.vertices[0].y, 3308.8);
}

TEST(MgsAssetCacheTest,
     CodecRecoveryRequiresNamedMaterialAndIntactPixelsOutsideBorrowedArea) {
  const char *oldMode = std::getenv("PS1_MGS_MESH_REPLAY");
  const std::string previous = oldMode ? oldMode : "";
  const bool existed = oldMode != nullptr;
  struct Cleanup {
    std::string previous;
    bool existed;
    ~Cleanup() {
      setSceneAssetCache(nullptr);
      configure(false, false);
      if (existed)
        setenv("PS1_MGS_MESH_REPLAY", previous.c_str(), 1);
      else
        unsetenv("PS1_MGS_MESH_REPLAY");
    }
  } cleanup{previous, existed};
  setenv("PS1_MGS_MESH_REPLAY", "1", 1);
  std::array<std::vector<uint8_t>, 9> sections;
  sections[0].resize(64);
  put(sections[0], 32, 'p');
  put(sections[0], 52, 88);
  sections[3].resize(32);
  put(sections[3], 0, 8);
  put(sections[3], 4, 8);
  put(sections[3], 12, 64);
  put(sections[3], 20, 16);
  sections[4].resize(64);
  for (uint32_t i = 0; i < 64; ++i)
    sections[4][i] = (i * 3) % 16;
  sections[5].resize(64);
  for (uint32_t i = 0; i < 16; ++i)
    put(sections[5], i * 4, 0x1000 + i);
  sections[6].resize(88);
  put(sections[6], 78, 768 | (256u << 16));
  put(sections[6], 82, 960 | (249u << 16));
  sections[7].resize(8);
  put(sections[7], 0, 0x4567);
  sections[8].resize(64);
  for (unsigned k = 0; k < 4; ++k) {
    put(sections[8], k * 16, std::bit_cast<uint32_t>(float(k & 1)));
    put(sections[8], k * 16 + 4, std::bit_cast<uint32_t>(float(k >> 1)));
    put(sections[8], k * 16 + 12, 0x3f800000);
  }
  const auto cache = AssetCache::decode(packFixture(sections, true));
  configure(true, false);
  setSceneAssetCache(&cache);
  std::vector<gpu::Color16> vram(1024 * 512);
  for (uint32_t i = 0; i < 16; ++i)
    vram[249 * 1024 + 960 + i].raw = 0x1000 + i;
  for (uint32_t i = 0; i < 64; ++i)
    vram[(256 + i / 8) * 1024 + 768 + (i % 8) / 4].raw |=
        uint16_t(sections[4][i]) << ((i % 4) * 4);
  gpu::replay::DrawPacket packet;
  packet.textured = true;
  packet.vertexCount = 4;
  packet.state.texturePage.raw = 0x1c;
  packet.state.clut.raw = 249 * 64 + 60;
  for (uint32_t k = 0; k < 4; ++k) {
    packet.vertices[k].u = (k & 1) * 7;
    packet.vertices[k].v = (k >> 1) * 7;
  }
  bindOriginalTexture(packet, vram);
  ASSERT_TRUE(packet.originalTexture);
  vram[256 * 1024 + 768].raw = 0xbeef;
  bindOriginalTexture(packet, vram);
  EXPECT_FALSE(packet.originalTexture);
  packet.originalMesh.emplace();
  packet.originalMesh->textureId = 0x4567;
  bindOriginalTexture(packet, vram);
  ASSERT_TRUE(packet.originalTexture);
  packet.originalMesh->textureId = 0x9999;
  bindOriginalTexture(packet, vram);
  EXPECT_FALSE(packet.originalTexture);
  packet.originalMesh->textureId = 0x4567;
  vram[260 * 1024 + 768].raw = 0xbeef;
  bindOriginalTexture(packet, vram);
  EXPECT_FALSE(packet.originalTexture);
}
