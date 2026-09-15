#include "runtime/cpu_context.h"
#include "runtime/games/mgs_asset_cache.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/gpu/gpu.h"
#include "runtime/gpu/modern_replay.h"
#include "runtime/gpu/modern_replay_execution_plan.h"
#include "runtime/gpu/modern_replay_vulkan_bake.h"
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
std::vector<uint8_t> sourceModel(bool quad = false) {
  std::vector<uint8_t> b(178);
  put(b, 0, 1); put(b, 4, 1);
  put(b, 36, 1); put(b, 76, ~0u); put(b, 80, ~0u);
  put(b, 84, 4); put(b, 88, 120); put(b, 92, 152);
  put(b, 96, 1); put(b, 100, 156); put(b, 104, 164);
  put(b, 108, 168); put(b, 112, 176);
  for (unsigned v = 0; v < 4; ++v) {
    b[120 + v * 8] = quad ? (v == 1 || v == 2) : v * 10;
    b[122 + v * 8] = quad && v >= 2;
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
std::vector<uint8_t> fixture(bool names = false, bool quad = false,
                             bool texture = false) {
  std::array<std::vector<uint8_t>, 9> sections;
  sections[0].resize(64);
  put(sections[0],32,'k'); put(sections[0],40,1); put(sections[0],44,~0u);
  put(sections[0],52,178); put(sections[0],56,1);
  sections[1].resize(32);
  put(sections[1],4,~0u); put(sections[1],8,~0u); put(sections[1],16,4);
  put(sections[1],20,4); put(sections[1],28,1);
  sections[2].resize(4*48);
  for (unsigned v = 0; v < 4; ++v) {
    put(sections[2], v * 48,
        std::bit_cast<uint32_t>(float(quad ? (v == 1 || v == 2) : v * 10)));
    put(sections[2], v * 48 + 4,
        std::bit_cast<uint32_t>(float(quad && v >= 2)));
    put(sections[2], v * 48 + 12, std::bit_cast<uint32_t>(-1.0f));
  }
  sections[6] = sourceModel(quad);
  if (texture) {
    sections[0].resize(128);
    put(sections[0], 64 + 32, 'p');
    put(sections[0], 64 + 48, 178);
    put(sections[0], 64 + 52, 128);
    sections[6].resize(178 + 128);
    put(sections[6], 178 + 82, 512 | (400u << 16));
    sections[3].resize(32);
    put(sections[3], 0, 4);
    put(sections[3], 4, 4);
    put(sections[3], 12, 16);
    put(sections[3], 20, 16);
    put(sections[3], 28, 1);
    sections[4].assign(16, 1);
    sections[5].resize(16 * 4);
    put(sections[5], 4, 0x7fff);
  }
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
void loadModel(Memory &memory, uint32_t address, bool quad = false) {
  auto b = sourceModel(quad);
  for (unsigned field : {56u,60u,68u,72u,76u,80u}) {
    const auto p=32+field;
    uint32_t v = uint32_t(b[p]) | uint32_t(b[p+1])<<8;
    put(b,p,address+v);
  }
  for (size_t i=0;i<b.size();++i) memory.write8(address+i,b[i]);
}

class MgsSceneRecoveryTest : public ::testing::Test {
protected:
  static constexpr uint32_t channel = 0x80010000, queue = 0x80010100,
                            object = 0x80010200, definition = 0x80011000,
                            packs = 0x80013000, colors = 0x80014000,
                            table = 0x80015000,
                            tex = 0x800B46C8 + (0x1234 & 511) * 12;
  AssetCache cache = AssetCache::decode(fixture(false, true));
  Memory memory;
  recomp_context ctx{};
  std::array<std::string, 3> previous;
  std::array<bool, 3> existed{};
  static constexpr const char *vars[] = {
      "PS1_MGS_MESH_REPLAY", "PS1_MGS_SCENE_RECOVERY", "PS1_MGS_SOURCE_MESHES"};
  void SetUp() override {
    for (unsigned i = 0; i < 3; ++i) {
      existed[i] = std::getenv(vars[i]) != nullptr;
      if (existed[i])
        previous[i] = std::getenv(vars[i]);
      setenv(vars[i], i == 2 ? "0" : "1", 1);
    }
    configure(true, false);
    setSceneAssetCache(&cache);
    loadModel(memory, definition, true);
    memory.write32(channel, table);
    memory.write16(channel + 8, 8);
    memory.write16(channel + 80, 320);
    memory.write16(channel + 86, 1);
    memory.write32(channel + 88, queue);
    memory.write16(channel + 92, -160);
    memory.write16(channel + 94, -112);
    memory.write16(channel + 96, 320);
    memory.write16(channel + 98, 224);
    memory.write32(queue, object);
    memory.write32(object + 36, definition);
    memory.write32(object + 40, 0x57); // static, painted, opaque, one piece
    memory.write16(object + 46, 1);
    memory.write32(object + 72 + 64, definition + 32);
    memory.write32(object + 72 + 68, colors);
    memory.write16(object + 72 + 82, 1);
    for (uint32_t offset : {0u, 8u, 16u}) {
      memory.write16(channel + 16 + offset, 4096);
      memory.write16(object + offset, 4096);
      memory.write16(object + 72 + offset, 4096);
    }
    // All four projected SXY coordinates truncate to zero; the source quad
    // still covers a nonzero area. No native packet has ever been allocated.
    memory.write32(object + 28, 10000);
    memory.write32(object + 72 + 28, 10000);
    for (unsigned k = 0; k < 4; ++k)
      memory.write32(colors + k * 4, 0x3c808080);
    memory.write16(definition + 176, 0x1234);
    memory.write16(tex, 0x1234);
    memory.write16(tex + 6, 400 * 64 + 32);
    memory.write8(tex + 10, 15);
    memory.write8(tex + 11, 15);
    for (uint32_t i = 0; i <= 256; ++i)
      memory.write32(table + i * 4,
                     i ? ((table + (i - 1) * 4) & 0xffffff) : 0xffffff);
    ctx.mem = &memory;
    ctx.r4 = channel;
    ctx.r5 = 0;
  }
  void TearDown() override {
    setSceneAssetCache(nullptr);
    configure(false, false);
    for (unsigned i = 0; i < 3; ++i) {
      if (existed[i])
        setenv(vars[i], previous[i].c_str(), 1);
      else
        unsetenv(vars[i]);
    }
  }
  std::unique_ptr<SceneRecoveryReplay> recover(bool prepare = true) {
    if (prepare) {
      captureSceneBeforeBounds(&ctx);
      prepareMeshReplay(&ctx);
    }
    return recoverSceneReplay(table + 256 * 4,
                              {memory.ramPtr(), Memory::RAM_SIZE});
  }
};
} // namespace

TEST_F(MgsSceneRecoveryTest,
       RecoversSubpixelSourceWithoutNativePacketsOrMutation) {
  const std::vector<uint8_t> before(memory.ramPtr(),
                                    memory.ramPtr() + Memory::RAM_SIZE);
  const auto registers = std::to_array(ctx.r);
  auto result = recover();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->orderingTable, table & 0xffffff);
  ASSERT_EQ(result->buckets[39].size(), 1u);
  const auto &draw = result->buckets[39].front();
  ASSERT_TRUE(draw.originalMesh);
  EXPECT_TRUE(draw.originalMesh->sourceRecovery);
  EXPECT_EQ(draw.originalMesh->textureId, 0x1234u);
  EXPECT_DOUBLE_EQ(draw.vertices[1].x, .032);
  EXPECT_GT(draw.vertices[2].y, 0);
  for (const auto &v : draw.vertices) {
    EXPECT_EQ(v.provenance, gpu::replay::ReplayVertexProvenance{});
    EXPECT_EQ(v.rawDepth, 10000);
  }
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
  EXPECT_EQ(std::to_array(ctx.r), registers);
  resetMeshReplay();
  EXPECT_FALSE(recover(false));
}

TEST_F(MgsSceneRecoveryTest,
       RenderingReadsOriginalVectorsAndIgnoresExpandedCopies) {
  auto baseline = recover();
  ASSERT_TRUE(baseline);
  auto bytes = fixture(false, true);
  const auto expanded = cache.section(AssetCache::Vertices);
  for (uint32_t i = 0; i < expanded.count; ++i) {
    put(bytes, expanded.offset + i * expanded.stride, 0x7fc00000);
    put(bytes, expanded.offset + i * expanded.stride + 4, 0x7f800000);
    put(bytes, expanded.offset + i * expanded.stride + 8, 0xffffffff);
    put(bytes, expanded.offset + i * expanded.stride + 12, 0);
  }
  checksum(bytes);
  cache = AssetCache::decode(bytes);
  setSceneAssetCache(&cache);
  const auto original = cache.sourceModel(0);
  ASSERT_TRUE(original);
  const auto actual = recover();
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual->buckets[39], baseline->buckets[39]);
  for (uint32_t k = 0; k < 4; ++k) {
    constexpr uint32_t order[] = {0, 1, 3, 2};
    EXPECT_EQ(actual->buckets[39][0].originalMesh->pointBytes[k],
              (original->positionsByte + order[k] * 8) | 0x80000000u);
  }
}

TEST_F(MgsSceneRecoveryTest, SourceOnlyCacheRendersWithoutExpandedVertices) {
  const auto baseline = recover();
  ASSERT_TRUE(baseline);
  const auto legacy = AssetCache::decode(fixture(true, true));
  std::array<std::vector<uint8_t>, 9> sections;
  for (unsigned i = 0; i < sections.size(); ++i) {
    const auto section = legacy.section(static_cast<AssetCache::Section>(i));
    const auto bytes = legacy.bytes().subspan(section.offset, section.size());
    sections[i].assign(bytes.begin(), bytes.end());
  }
  auto expanded = packFixture(sections, true);
  std::memcpy(expanded.data(), "MGSAC003", 8);
  EXPECT_THROW(AssetCache::decode(expanded), std::runtime_error);
  sections[AssetCache::Vertices].clear();
  // Byte-distinct texture variants resolve to the same live geometry. This
  // exercises alias selection as well as projection with no VRTX section.
  const auto asset = sections[AssetCache::Assets];
  sections[AssetCache::Assets].insert(sections[AssetCache::Assets].end(),
                                      asset.begin(), asset.end());
  put(sections[AssetCache::Assets], 64 + 36, 1);
  put(sections[AssetCache::Assets], 64 + 48, 178);
  const auto model = sections[AssetCache::Models];
  sections[AssetCache::Models].insert(sections[AssetCache::Models].end(),
                                      model.begin(), model.end());
  put(sections[AssetCache::Models], 32 + 12, 4);
  put(sections[AssetCache::Models], 32 + 24, 1);
  const auto source = sections[AssetCache::Source];
  sections[AssetCache::Source].insert(sections[AssetCache::Source].end(),
                                      source.begin(), source.end());
  sections[AssetCache::Source][178 + 176] = 42;
  auto bytes = packFixture(sections, true);
  std::memcpy(bytes.data(), "MGSAC003", 8);
  cache = AssetCache::decode(bytes);
  ASSERT_EQ(cache.section(AssetCache::Vertices).count, 0u);
  const auto original = cache.sourceModel(0);
  ASSERT_TRUE(original);
  setSceneAssetCache(&cache);
  const auto result = recover();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->buckets[39].size(), 1u);
  // Container offsets change; the original projection and guest state do not.
  EXPECT_EQ(result->buckets[39][0].vertices, baseline->buckets[39][0].vertices);
  for (uint32_t k = 0; k < 4; ++k) {
    constexpr uint32_t order[] = {0, 1, 3, 2};
    EXPECT_EQ(result->buckets[39][0].originalMesh->pointBytes[k],
              (original->positionsByte + order[k] * 8) | 0x80000000u);
  }
  auto invalid = bytes;
  put(invalid, cache.section(AssetCache::Models).offset + 12, 4);
  checksum(invalid);
  EXPECT_THROW(AssetCache::decode(invalid), std::runtime_error);
  invalid = bytes;
  put(invalid, original->descriptorByte + 56, 0xffffffff);
  checksum(invalid);
  EXPECT_THROW(AssetCache::decode(invalid), std::runtime_error);
}

TEST_F(MgsSceneRecoveryTest, RespectsSemanticVisibilityAndSupportedMaterials) {
  ASSERT_TRUE(recover());
  for (uint32_t flags : {0xd7u, 0x5fu, 0x257u, 0x457u, 0x55u}) {
    memory.write32(object + 40, flags);
    EXPECT_FALSE(recover()) << flags;
  }
  memory.write32(object + 40, 0x57);
  memory.write16(object + 44, 2);
  memory.write32(0x800AE0E0, 1);
  EXPECT_FALSE(recover());
  memory.write16(object + 44, 0);
  memory.write32(definition + 32, 2); // semitransparent model
  EXPECT_FALSE(recover());
  memory.write32(definition + 32, 0);
  memory.write32(colors, 0x3e808080);
  EXPECT_FALSE(recover());
  memory.write32(colors, 0x3c808080);
  memory.write16(tex, 0);
  EXPECT_FALSE(recover());
  memory.write16(tex, 0x1234);
  memory.write32(object + 72 + 68, 0);
  EXPECT_FALSE(recover());
}

TEST_F(MgsSceneRecoveryTest, NativeScreenMatrixIsOnlyTheProjectionOracle) {
  const auto baseline = recover();
  ASSERT_TRUE(baseline);
  auto packet = baseline->buckets[39].front();
  packet.originalMesh.reset();
  const uint32_t model = object + 72;
  memory.write16(model + 76, 1);
  memory.write32(model + 84, packs);
  for (uint32_t offset : {0u, 8u, 16u})
    memory.write16(model + 32 + offset, 4096);
  // Deliberately distinguish the oracle from the original float scene. This
  // matrix must validate native SXY without becoming Vulkan's world matrix.
  memory.write32(model + 32 + 20, 1000);
  memory.write32(model + 32 + 28, 10000);
  for (unsigned k = 0; k < 4; ++k) {
    memory.write32(packs + 4 + k * 12, 0x3c808080);
    memory.write32(packs + 8 + k * 12, 32);
    memory.write16(packs + 12 + k * 12, 0);
    auto &v = packet.vertices[k];
    v.nativeX = packet.state.drawOffset.x + 32;
    v.nativeY = packet.state.drawOffset.y;
    v.u = v.v = 0;
    v.provenance.dmaNodeSource = (packs & 0x1fffffff) + 1;
    v.provenance.gp0WordSource = (packs & 0x1fffffff) + 8 + k * 12 + 1;
    v.provenance.evidence = gpu::replay::ReplayVertexDmaTraversalExact |
                            gpu::replay::ReplayVertexGp0WordSourceExact;
  }
  memory.write16(packs + 14, packet.state.clut.raw);
  memory.write16(packs + 26, packet.state.texturePage.raw);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  const auto before = packet;
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  EXPECT_EQ(packet.vertices[0].nativeX, before.vertices[0].nativeX);
  EXPECT_EQ(packet.vertices[0].x, baseline->buckets[39][0].vertices[0].x);
  EXPECT_EQ(packet.originalMesh->viewRows,
            baseline->buckets[39][0].originalMesh->viewRows);
  memory.write32(model + 32 + 20, 8000);
  prepareMeshReplay(&ctx);
  packet = before;
  EXPECT_FALSE(replaceMeshReplayPacket(packet));
  EXPECT_EQ(packet, before);
}

TEST_F(MgsSceneRecoveryTest, ArticulatedPaintUsesSameSourceProducerAsOnePiece) {
  const auto single = recover();
  ASSERT_TRUE(single);
  memory.write32(object + 40, 0x17); // articulated rather than ONEPIECE
  const auto articulated = recover();
  ASSERT_TRUE(articulated);
  EXPECT_EQ(single->buckets[39].front(), articulated->buckets[39].front());
}

TEST_F(MgsSceneRecoveryTest, NativeMaterialAdapterUsesIdenticalSourceGeometry) {
  const auto direct = recover();
  ASSERT_TRUE(direct);
  memory.write32(object + 72 + 84, packs);
  gpu::replay::DrawPacket packet;
  packet.opcode = 0x3c;
  packet.vertexCount = 4;
  packet.indexCount = 6;
  packet.textured = packet.gouraud = true;
  packet.triangleIndices = {0, 1, 2, 1, 2, 3};
  packet.state.clut.raw = 400 * 64 + 32;
  for (unsigned k = 0; k < 4; ++k) {
    memory.write32(packs + 4 + k * 12, 0x3c808080);
    auto &v = packet.vertices[k];
    v.color = {128, 128, 128};
    v.provenance.dmaNodeSource = (packs & 0xffffff) + 1;
    v.provenance.gp0WordSource = (packs & 0xffffff) + 9 + k * 12;
    v.provenance.evidence = gpu::replay::ReplayVertexDmaTraversalExact |
                            gpu::replay::ReplayVertexGp0WordSourceExact;
  }
  memory.write16(packs + 14, packet.state.clut.raw);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  ASSERT_TRUE(replaceMeshReplayPacket(packet));
  auto expected = direct->buckets[39].front();
  expected.originalMesh->sourceRecovery = false;
  EXPECT_EQ(packet.originalMesh, expected.originalMesh);
  for (unsigned k = 0; k < 4; ++k) {
    EXPECT_EQ(packet.vertices[k].x, expected.vertices[k].x);
    EXPECT_EQ(packet.vertices[k].y, expected.vertices[k].y);
    EXPECT_EQ(packet.vertices[k].rawDepth, expected.vertices[k].rawDepth);
  }
}

TEST_F(MgsSceneRecoveryTest,
       DmaAndHleObserverProduceSourceDrawWithoutTouchingNativeVram) {
  using namespace gpu::replay;
  cache = AssetCache::decode(fixture(false, true, true));
  setSceneAssetCache(&cache);
  for (const bool hle : {false, true}) {
    gpu::GPU gpu;
    gpu::GPU::ReplayConfig config;
    config.enabled = true;
    config.pgxp = true;
    config.internalScale = 12;
    config.trueDepthMode = ReplayDepthMode::Hybrid;
    gpu.configureReplay(config);
    gpu.writeGP0(0xe3000000);
    gpu.writeGP0(0xe4000000 | 319 | (223u << 10));
    gpu.writeGP0(0xe5000000 | 160 | (112u << 11));
    const auto upload = [&](uint32_t position, uint32_t extent,
                            std::span<const uint32_t> words) {
      gpu.writeGP0(0xa0000000);
      gpu.writeGP0(position);
      gpu.writeGP0(extent);
      for (auto word : words)
        gpu.writeGP0(word);
    };
    const std::array<uint32_t, 2> pixels{0x11111111, 0x11111111};
    upload(0, 1 | (4u << 16), pixels);
    const std::array<uint32_t, 8> palette{0x7fff0000};
    upload(512 | (400u << 16), 16 | (1u << 16), palette);
    // Seed the capture before the empty native list, then isolate its frame.
    gpu.snapshotDisplayBuffer(1);
    ASSERT_TRUE(gpu.tryConsumeReplay());
    const std::vector<gpu::Color16> before(gpu.getVRAM(),
                                           gpu.getVRAM() + 1024 * 512);
    captureSceneBeforeBounds(&ctx);
    prepareMeshReplay(&ctx);
    if (hle) {
      gpu.beginCommandBatch();
      auto observer =
          gpu.observePresentationList(table + 256 * 4, memory.ramPtr());
      ASSERT_TRUE(observer);
      for (int slot = 256; slot >= 0; --slot)
        observer((table & 0xffffff) + slot * 4);
      gpu.endCommandBatch();
    } else {
      gpu.processLinkedList(table + 256 * 4, memory.ramPtr(), &memory);
    }
    EXPECT_EQ(std::memcmp(before.data(), gpu.getVRAM(),
                          before.size() * sizeof(gpu::Color16)),
              0);
    gpu.snapshotDisplayBuffer(2);
    const auto frame = gpu.tryConsumeReplay();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->latch().complete);
    uint32_t draws = 0;
    for (const auto &operation : frame->operations()) {
      if (const auto *draw = std::get_if<DrawPacket>(&operation)) {
        ++draws;
        ASSERT_TRUE(draw->originalMesh);
        EXPECT_TRUE(draw->originalMesh->sourceRecovery);
        ASSERT_TRUE(draw->originalTexture);
        EXPECT_EQ(draw->originalTexture->data[2], 4u);
        EXPECT_NEAR(draw->vertices[1].x, 160.032, 1e-8);
        EXPECT_GT(draw->submissionId, 0u);
        EXPECT_EQ(draw->vertices[0].provenance, ReplayVertexProvenance{});
      }
    }
    EXPECT_EQ(draws, 1u) << "HLE observer: " << hle;
  }
}

TEST_F(MgsSceneRecoveryTest, UsesFloatingFrustumAndWinding) {
  ASSERT_TRUE(recover());
  memory.write32(object + 20, 100000);
  memory.write32(object + 72 + 20, 100000);
  EXPECT_FALSE(recover());
  memory.write32(object + 20, 0);
  memory.write32(object + 72 + 20, 0);
  memory.write32(object + 28, -10000);
  memory.write32(object + 72 + 28, -10000);
  EXPECT_FALSE(recover());
  memory.write32(object + 28, 10000);
  memory.write32(object + 72 + 28, 10000);
  memory.write16(channel + 16, -4096);
  EXPECT_FALSE(recover());
  memory.write32(definition + 32, 0x400); // explicit double-sided model
  auto bytes = fixture(false, true);
  put(bytes, cache.section(AssetCache::Source).offset + 32, 0x400);
  checksum(bytes);
  cache = AssetCache::decode(bytes);
  setSceneAssetCache(&cache);
  EXPECT_TRUE(recover());
}

TEST_F(MgsSceneRecoveryTest,
       RequiresCompleteAcyclicOrderingTableAndFreshCapture) {
  ASSERT_TRUE(recover());
  memory.write32(table + 7 * 4, (table + 5 * 4) & 0xffffff); // bypass a bucket
  EXPECT_FALSE(recover(false));
  memory.write32(table + 7 * 4, (table + 7 * 4) & 0xffffff); // cycle
  EXPECT_FALSE(recover(false));
  memory.write32(table + 7 * 4, (table + 6 * 4) & 0xffffff);
  ASSERT_TRUE(recover(false));
  memory.write16(channel + 86, 0);
  EXPECT_FALSE(recover());
  memory.write16(channel + 86, 1);
  ASSERT_TRUE(recover());
  ctx.r5 = 2;
  EXPECT_FALSE(recover());
}

TEST_F(MgsSceneRecoveryTest,
       RejectsAllocatedMaterialOverridesAndNativeDuplicates) {
  memory.write32(object + 72 + 84, packs);
  for (unsigned k = 0; k < 4; ++k)
    memory.write32(packs + 4 + k * 12, 0x3c808080);
  memory.write16(packs + 14, 400 * 64 + 32);
  ASSERT_TRUE(recover());
  memory.write32(packs + 4, 0x3c707070);
  EXPECT_FALSE(recover());
  memory.write32(packs + 4, 0x3c808080);
  ASSERT_TRUE(recover());
  // The entire actual list is checked before insertion, even if the native
  // packet comes later in the same bucket than the recovered source would.
  memory.write32(packs, 0x0c000000 | ((table + 39 * 4) & 0xffffff));
  memory.write32(table + 40 * 4, packs & 0xffffff);
  EXPECT_FALSE(recover(false));
  memory.write32(table + 40 * 4, (table + 39 * 4) & 0xffffff);
  memory.write16(object + 50, 2);
  memory.write16(object + 72 + 76, 2);
  // Accepted originals may have subdivision children: never redraw their
  // parent merely because the parent's own packet is absent from the list.
  memory.write32(packs, 10000);
  EXPECT_FALSE(recover());
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
TEST(MgsAssetCacheTest, SourceFormatViewsResolveKmdAndEmbeddedZmdStreams) {
  auto bytes = fixture(false, true);
  auto cache = AssetCache::decode(bytes);
  auto model = cache.sourceModel(0);
  ASSERT_TRUE(model);
  EXPECT_EQ(model->positionsByte,
            cache.section(AssetCache::Source).offset + 120);
  EXPECT_EQ(model->uvByte, cache.section(AssetCache::Source).offset + 168);
  EXPECT_EQ(model->normals, 1u);
  EXPECT_EQ(model->parent, -1);
  EXPECT_FALSE(cache.sourceModel(1));
  put(bytes, cache.section(AssetCache::Source).offset + 32 + 56, 0xfffffff0);
  checksum(bytes);
  EXPECT_FALSE(AssetCache::decode(bytes).sourceModel(0));
  bytes = fixture(false, true);
  cache = AssetCache::decode(bytes);
  std::array<std::vector<uint8_t>, 9> sections;
  for (size_t i = 0; i < 7; ++i) {
    const auto section = cache.section(AssetCache::Section(i));
    sections[i].assign(bytes.begin() + section.offset,
                       bytes.begin() + section.offset + section.size());
  }
  const auto kmd = sourceModel(true);
  auto &zmd = sections[6];
  zmd.assign(198, 0);
  put(zmd, 0, 0x61444d4b);
  put(zmd, 4, 1);
  put(zmd, 8, 124);
  put(zmd, 16, 0x1234);
  std::copy(kmd.begin(), kmd.begin() + 120, zmd.begin() + 20);
  std::copy(kmd.begin() + 120, kmd.end(), zmd.begin() + 140);
  for (uint32_t field : {56u, 60u, 68u, 72u, 76u, 80u}) {
    const uint32_t relative =
        kmd[32 + field] | (uint32_t(kmd[33 + field]) << 8);
    put(zmd, 52 + field, relative - 120);
  }
  put(sections[0], 52, zmd.size());
  put(sections[0], 60, 0x1234);
  cache = AssetCache::decode(packFixture(sections, false));
  model = cache.sourceModel(0);
  ASSERT_TRUE(model);
  EXPECT_EQ(model->descriptorByte,
            cache.section(AssetCache::Source).offset + 52);
  EXPECT_EQ(model->positionsByte,
            cache.section(AssetCache::Source).offset + 140);
  EXPECT_EQ(model->vertexIndicesByte,
            cache.section(AssetCache::Source).offset + 172);
  EXPECT_EQ(model->textureIdsByte,
            cache.section(AssetCache::Source).offset + 196);
  EXPECT_EQ(model->faces, 1u);
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
  for (uint32_t m = 0; m < cache.section(AssetCache::Models).count; ++m) {
    const auto original = cache.sourceModel(m);
    ASSERT_TRUE(original) << "original model " << m;
    EXPECT_EQ(original->faces * 4, cache.word(AssetCache::Models, m, 4));
  }
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
            (cache.section(AssetCache::Source).offset + 120 + 3 * 8) |
                0x80000000u);
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

TEST_F(MgsSceneRecoveryTest, OpaqueRecoveredMaterialRetainsRawAbrSelector) {
  for (uint32_t abr = 0; abr < 4; ++abr) {
    gpu::replay::DrawPacket packet;
    packet.originalMesh.emplace();
    packet.originalTexture.emplace();
    packet.state.texturePage.raw = uint16_t(abr << 5);
    gpu::replay::BakedDrawState environment;
    environment.depth.epoch = 1;
    environment.dither = true;
    environment.drawToDisplayArea = true;
    (void)bindRecoveredSceneDraw(packet, environment, {});
    EXPECT_EQ(packet.state.blend.mode, gpu::replay::BlendMode(abr));
    EXPECT_FALSE(packet.state.blend.enabled);
    EXPECT_EQ(packet.state.texturePage.raw, uint16_t(0x600 | (abr << 5)));
  }
}

class MgsSourceMeshTest : public MgsSceneRecoveryTest {
protected:
  void SetUp() override {
    MgsSceneRecoveryTest::SetUp();
    setenv("PS1_MGS_SOURCE_MESHES", "1", 1);
    cache = AssetCache::decode(fixture(false, true, true));
    setSceneAssetCache(&cache);
  }
  void packet(uint32_t address, uint32_t next, uint8_t opcode = 0x3c) {
    memory.write32(address, 0x0c000000 | (next & 0xffffff));
    for (unsigned k = 0; k < 4; ++k) {
      memory.write32(address + 4 + k * 12, uint32_t(opcode) << 24 | 0x808080);
      // Deliberately unrelated SXY: source identity, not GTE projection, owns
      // the modern geometry. The native oracle still draws these coordinates.
      memory.write32(address + 8 + k * 12,
                     (20u + (k & 1) * 8) | ((20u + (k >> 1) * 8) << 16));
      memory.write16(address + 12 + k * 12, 0);
    }
    memory.write16(address + 14, 400 * 64 + 32);
    memory.write16(address + 26, 0);
  }
  void link() {
    memory.write32(object + 72 + 84, packs);
    packet(packs, table + 39 * 4);
    memory.write32(table + 40 * 4, packs & 0xffffff);
  }
  void configureGpu(gpu::GPU &gpu, bool replay) {
    gpu::GPU::ReplayConfig config;
    config.enabled = replay;
    config.pgxp = true;
    config.internalScale = 4;
    config.trueDepthMode = gpu::replay::ReplayDepthMode::Hybrid;
    gpu.configureReplay(config);
    for (uint32_t w :
         {0xe3000000u, 0xe4000000u | 319u | (223u << 10),
          0xe5000000u | 160u | (112u << 11), 0xa0000000u, 0u, 1u | (4u << 16),
          0x11111111u, 0x11111111u, 0xa0000000u, 512u | (400u << 16),
          16u | (1u << 16), 0x7fff0000u, 0u, 0u, 0u, 0u, 0u, 0u, 0u})
      gpu.writeGP0(w);
    gpu.snapshotDisplayBuffer(1);
    if (replay)
      (void)gpu.tryConsumeReplay();
  }
};

TEST_F(MgsSourceMeshTest, CompleteFaceUsesSourceWithoutNativeProjectionGate) {
  link();
  auto result = recover();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->meshes.size(), 1u);
  ASSERT_EQ(result->meshNodes.size(), 1u);
  const auto &draw = result->meshes.front().draw;
  ASSERT_TRUE(draw.originalMesh);
  EXPECT_TRUE(draw.originalMesh->sourceTopology);
  EXPECT_DOUBLE_EQ(draw.vertices[1].x, .032);
  EXPECT_EQ(draw.originalMesh->weights,
            gpu::replay::OriginalMeshGeometry{}.weights);
  EXPECT_TRUE(result->meshes.front().visible);
  // Moving the source behind the camera culls it independently of native SXY.
  memory.write32(object + 28, uint32_t(-10000));
  memory.write32(object + 72 + 28, uint32_t(-10000));
  result = recover();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->meshes.size(), 1u);
  EXPECT_FALSE(result->meshes.front().visible);
}

TEST_F(MgsSourceMeshTest, SubdivisionChildrenShareOneOriginalFace) {
  link();
  captureSceneBeforeBounds(&ctx);
  beginMeshDivide(&ctx);
  memory.write32(0x1f800034, 0x1f800038);
  meshDivideGrid(&ctx);
  memory.write32(0x1f80001c, packs);
  ctx.r16 = 0x1f800038;
  constexpr uint32_t a = 0x80016000, b = 0x80016040;
  ctx.r7 = a;
  meshDividePacket(&ctx);
  ctx.r7 = b;
  meshDividePacket(&ctx);
  packet(a, b);
  packet(b, table + 39 * 4);
  memory.write32(table + 40 * 4, a & 0xffffff);
  auto result = recover();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->meshes.size(), 1u);
  EXPECT_EQ(result->meshNodes.at(a & 0xffffff),
            result->meshNodes.at(b & 0xffffff));
  EXPECT_EQ(result->meshes.front().draw.originalMesh->weights,
            gpu::replay::OriginalMeshGeometry{}.weights);
  EXPECT_DOUBLE_EQ(result->meshes.front().draw.vertices[1].x, .032);
  // A late mutation of either child invalidates the whole replacement. Do not
  // emit a full face alongside a native child whose material is now different.
  memory.write32(b + 4, 0x3c404040);
  EXPECT_FALSE(recover(false));
  result = recover();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->meshes.size(), 1u);
}

TEST_F(MgsSourceMeshTest, LatePacketMutationRequiresFreshSceneCapture) {
  link();
  ASSERT_TRUE(recover());
  memory.write16(packs + 12, 0x0101);
  EXPECT_FALSE(recover(false));
  const auto fresh = recover();
  ASSERT_TRUE(fresh);
  ASSERT_EQ(fresh->meshes.size(), 1u);
  EXPECT_TRUE(fresh->meshes.front().draw.originalMesh->sourceTopology);
}

TEST_F(MgsSourceMeshTest, AnimatedLightingUsesOriginalNormalsWithoutPackets) {
  constexpr uint32_t light = 0x80017000, angles = 0x80017100;
  memory.write32(object + 40, 0x1d); // TEXT, TRANS, SHADE, BOUND; articulated
  memory.write32(object + 52, light);
  memory.write32(object + 56, angles);
  memory.write16(light + 4, 4096);  // first light points down Z
  memory.write16(light + 32, 1024); // R=64, G=32, B=16 at normal incidence
  memory.write16(light + 38, 512);
  memory.write16(light + 44, 256);
  const std::vector<uint8_t> before(memory.ramPtr(),
                                    memory.ramPtr() + Memory::RAM_SIZE);
  const auto result = recover();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->buckets[39].size(), 1u);
  const auto &draw = result->buckets[39].front();
  EXPECT_TRUE(draw.originalMesh->sourceTopology);
  EXPECT_EQ(draw.vertices[0].color, (gpu::replay::Rgb8{64, 32, 16}));
  EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
  // A quarter turn changes the normal with the bone, without creating packs.
  memory.write16(angles + 2, 1024);
  captureSceneBeforeBounds(&ctx);
  const auto scene = latestSceneSnapshot();
  ASSERT_TRUE(scene);
  const auto material =
      sourceMeshMaterial(cache, memory, *scene, scene->instances[0], 0,
                         std::array<uint32_t, 1>{0}, 0, 0);
  ASSERT_TRUE(material);
  EXPECT_EQ(material->vertices[0].color, (gpu::replay::Rgb8{0, 0, 0}));
}

TEST_F(MgsSourceMeshTest, TransparencyDoesNotRequireNativePacketAllocation) {
  memory.write32(definition + 32, 2);
  for (unsigned k = 0; k < 4; ++k)
    memory.write32(colors + k * 4, 0x3e808080);
  const auto result = recover();
  ASSERT_TRUE(result);
  const auto &draw = result->buckets[39].front();
  EXPECT_EQ(draw.opcode, 0x3e);
  EXPECT_TRUE(draw.state.blend.enabled);
  EXPECT_TRUE(draw.state.blend.texturedStpOnly);
  EXPECT_TRUE(draw.originalMesh->sourceTopology);
}

TEST_F(MgsSourceMeshTest,
       ReusedOrderingTableDoesNotRestoreAbsentMeshesOverCodec) {
  ASSERT_TRUE(recover());
  EXPECT_FALSE(recover(false));
  EXPECT_TRUE(recover());
}

TEST_F(MgsSourceMeshTest, ThermalEligibilityDoesNotRequireNativePackets) {
  memory.write32(object + 40, 0x257); // thermal eligible, ordinary painted mesh
  const auto result = recover();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->buckets[39].size(), 1u);
  EXPECT_TRUE(result->buckets[39].front().originalMesh->sourceTopology);
  EXPECT_EQ(result->buckets[39].front().vertices[0].color,
            (gpu::replay::Rgb8{128, 128, 128}));
}

TEST_F(MgsSourceMeshTest, NullMaterialIsDefinedWithoutNativePackets) {
  auto bytes = fixture(true, true, true);
  const auto original = AssetCache::decode(bytes);
  const auto ids = original.sourceModel(0)->textureIdsByte;
  bytes[ids] = bytes[ids + 1] = 0;
  checksum(bytes);
  cache = AssetCache::decode(bytes);
  setSceneAssetCache(&cache);
  memory.write16(definition + 176, 0);
  const auto result = recover();
  ASSERT_TRUE(result);
  auto draw = result->buckets[39].front();
  ASSERT_TRUE(draw.originalMesh);
  EXPECT_TRUE(draw.originalMesh->sourceTopology);
  EXPECT_TRUE(draw.originalMesh->runtimeTexture);
  EXPECT_EQ(draw.state.texturePage.raw, 0);
  EXPECT_EQ(draw.state.clut.raw, 0);
  gpu::replay::BakedDrawState environment;
  environment.depth.epoch = 1;
  EXPECT_FALSE(bindRecoveredSceneDraw(draw, environment, {}));
  std::vector<gpu::Color16> vram(1024 * 512);
  EXPECT_TRUE(bindRecoveredSceneDraw(draw, environment, vram));
  EXPECT_FALSE(draw.originalTexture);
}

TEST_F(MgsSourceMeshTest, ReplacesDrawAtomicallyAndPreservesNativeVram) {
  link();
  gpu::GPU native, modern;
  configureGpu(native, false);
  configureGpu(modern, true);
  // MGS uses forced STP throughout the intro. Preserve it without vetoing
  // source geometry or losing the independent mask plane used by refraction.
  native.writeGP0(0xe6000001);
  modern.writeGP0(0xe6000001);
  captureSceneBeforeBounds(&ctx);
  prepareMeshReplay(&ctx);
  native.processLinkedList(table + 256 * 4, memory.ramPtr(), &memory);
  modern.processLinkedList(table + 256 * 4, memory.ramPtr(), &memory);
  EXPECT_EQ(std::memcmp(native.getVRAM(), modern.getVRAM(),
                        1024 * 512 * sizeof(gpu::Color16)),
            0);
  modern.snapshotDisplayBuffer(2);
  const auto frame = modern.tryConsumeReplay();
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->latch().complete);
  unsigned count = 0;
  for (const auto &op : frame->operations()) {
    if (const auto *draw = std::get_if<gpu::replay::DrawPacket>(&op)) {
      ++count;
      ASSERT_TRUE(draw->originalMesh);
      EXPECT_TRUE(draw->originalMesh->sourceTopology);
      EXPECT_TRUE(draw->state.depth.testEnabled);
      EXPECT_TRUE(draw->state.mask.forceMaskBit);
      EXPECT_NEAR(draw->vertices[1].x, 160.032, 1e-8);
      EXPECT_EQ(draw->vertices[0].provenance,
                gpu::replay::ReplayVertexProvenance{});
    }
  }
  EXPECT_EQ(count, 1u);
  const std::array<gpu::replay::CrashReplayFramePtr, 1> batch{frame};
  const auto execution = gpu::replay::planModernReplayExecution(batch);
  ASSERT_TRUE(execution.accepted()) << int(execution.reason);
  gpu::replay::VulkanReplayBakeFeatures features;
  features.allowTextured = features.allowDestinationMaskRead = true;
  const auto baked =
      gpu::replay::bakeModernReplayVulkan(execution.plan, 4, features);
  EXPECT_TRUE(baked.accepted()) << int(baked.reason);
}

TEST_F(MgsSourceMeshTest,
       ScrollingTextureSamplesOriginalPcxWithExactPhaseProof) {
  auto bytes = fixture(true, true, true);
  auto original = AssetCache::decode(bytes);
  // Logical name 0x1234 identifies the PCX; rows have distinguishable indices.
  put(bytes, original.section(AssetCache::Names).offset + 4, 1);
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x)
      bytes[original.section(AssetCache::Indices).offset + y * 4 + x] = y + 1;
  checksum(bytes);
  cache = AssetCache::decode(bytes);
  setSceneAssetCache(&cache);
  const auto source = recover();
  ASSERT_TRUE(source);
  auto draw = source->buckets[39].front();
  std::vector<gpu::Color16> vram(1024 * 512);
  vram[400 * 1024 + 513].raw = 0x7fff;
  vram[0].raw = 0x2222;
  vram[1024].raw = 0x3333;
  vram[2048].raw = 0x1111;
  vram[3072].raw = 0x4444;
  bindOriginalTexture(draw, vram);
  ASSERT_TRUE(draw.originalTexture);
  EXPECT_EQ(draw.originalTexture->origin[3], (3 << 16) | 1);
  EXPECT_EQ(draw.originalTexture->data[0],
            cache.section(AssetCache::Indices).offset);
  // tex_scrl rotates height-1 rows. The border and every interior texel must
  // still match; arbitrary texture writes do not authorize a cached substitute.
  vram[3072].raw = 0x5555;
  bindOriginalTexture(draw, vram);
  EXPECT_FALSE(draw.originalTexture);
  vram[3072].raw = 0x4444;
  vram[1024].raw ^= 1;
  bindOriginalTexture(draw, vram);
  EXPECT_FALSE(draw.originalTexture);
  vram[1024].raw ^= 1;
  draw.originalMesh->textureId = 0x5678;
  bindOriginalTexture(draw, vram);
  EXPECT_FALSE(draw.originalTexture);
}

TEST_F(MgsSourceMeshTest,
       AnimatedPaletteSupportsPcxWithFewerThanSixteenColors) {
  for (uint32_t colors : {1u, 11u, 15u}) {
    SCOPED_TRACE(colors);
    auto bytes = fixture(true, true, true);
    auto original = AssetCache::decode(bytes);
    put(bytes, original.section(AssetCache::Textures).offset + 20, colors);
    for (unsigned i = 0; i < 16; ++i)
      bytes[original.section(AssetCache::Indices).offset + i] = 0;
    checksum(bytes);
    cache = AssetCache::decode(bytes);
    setSceneAssetCache(&cache);
    const auto source = recover();
    ASSERT_TRUE(source);
    auto draw = source->buckets[39].front();
    std::vector<gpu::Color16> vram(1024 * 512);
    for (unsigned i = 0; i < 16; ++i)
      vram[400 * 1024 + 512 + i].raw = 0x1000 + i;
    bindOriginalTexture(draw, vram);
    ASSERT_TRUE(draw.originalTexture);
    EXPECT_EQ(draw.originalTexture->origin[2], 2);
    EXPECT_EQ(draw.originalTexture->palette[0], 0x10011000u);
    EXPECT_EQ(draw.originalTexture->palette[7], 0x100f100eu);
    // A palette animation does not permit changing the original index stream.
    vram[0].raw = 0x1111;
    bindOriginalTexture(draw, vram);
    EXPECT_FALSE(draw.originalTexture);
  }
}

TEST_F(MgsSourceMeshTest, TextScreenUsesSourceMeshWithVerifiedActorSurface) {
  auto bytes = fixture(true, true, true);
  auto original = AssetCache::decode(bytes);
  const auto ids = original.sourceModel(0)->textureIdsByte;
  bytes[ids] = 0x1d;
  bytes[ids + 1] = 0xa9;
  checksum(bytes);
  cache = AssetCache::decode(bytes);
  setSceneAssetCache(&cache);
  memory.write16(definition + 176, 0xa91d);
  const auto texture = [&](uint16_t id, uint16_t page, uint16_t clut, uint8_t w,
                           uint8_t h) {
    const uint32_t address = 0x800b46c8 + (id & 511) * 12;
    memory.write16(address, id);
    memory.write16(address + 4, page);
    memory.write16(address + 6, clut);
    memory.write8(address + 10, w);
    memory.write8(address + 11, h);
    return address;
  };
  texture(0xcc22, 17, 0, 255, 255);
  const auto font = texture(0xef71, 18, 15160, 111, 6);
  texture(0xa91d, 49, 15160, 127, 69);
  link();
  memory.write16(packs + 14, 15160);
  memory.write16(packs + 26, 49);
  gpu::GPU modern;
  configureGpu(modern, true);
  ASSERT_TRUE(recover());
  modern.processLinkedList(table + 256 * 4, memory.ramPtr(), &memory);
  modern.snapshotDisplayBuffer(2);
  const auto frame = modern.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  unsigned count = 0;
  for (const auto &op : frame->operations()) {
    const auto *draw = std::get_if<gpu::replay::DrawPacket>(&op);
    if (!draw)
      continue;
    ++count;
    ASSERT_TRUE(draw->originalMesh);
    EXPECT_TRUE(draw->originalMesh->sourceTopology);
    EXPECT_TRUE(draw->originalMesh->runtimeTexture);
    EXPECT_FALSE(draw->originalTexture);
    EXPECT_NEAR(draw->vertices[1].x, 160.032, 1e-8);
  }
  EXPECT_EQ(count, 1u);
  const std::array<gpu::replay::CrashReplayFramePtr, 1> batch{frame};
  const auto execution = gpu::replay::planModernReplayExecution(batch);
  ASSERT_TRUE(execution.accepted()) << int(execution.reason);
  gpu::replay::VulkanReplayBakeFeatures features;
  features.allowTextured = features.allowDestinationMaskRead = true;
  EXPECT_TRUE(gpu::replay::bakeModernReplayVulkan(execution.plan, 4, features)
                  .accepted());
  // A matching name alone cannot turn a material miss into a runtime surface.
  memory.write16(font + 6, 15161);
  const auto invalid = recover();
  ASSERT_TRUE(invalid);
  EXPECT_FALSE(invalid->meshes.front().draw.originalMesh->runtimeTexture);
}
