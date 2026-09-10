#include "runtime/gpu/gpu.h"
#include "runtime/gpu/modern_replay_execution_plan.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <vector>

using namespace ps1::gpu;

class GpuDmaTest : public ::testing::Test {
protected:
  GPU gpu;
  uint8_t ram[1024 * 1024 * 2]; // 2MB dummy RAM

  void SetUp() override {
    gpu.reset();
    std::fill(std::begin(ram), std::end(ram), 0);
  }

  void writeWord(uint32_t addr, uint32_t val) {
    uint32_t offset = addr & 0x1FFFFC;
    ram[offset] = val & 0xFF;
    ram[offset + 1] = (val >> 8) & 0xFF;
    ram[offset + 2] = (val >> 16) & 0xFF;
    ram[offset + 3] = (val >> 24) & 0xFF;
  }
};

TEST_F(GpuDmaTest, ProcessLinkedListParsesOT) {
  // Construct a small OT with 2 nodes.

  // Node 1 at 0x1000 sends a Monochrome Rectangle
  // Header: 2 words payload, points to 0x2000
  writeWord(0x1000, (2 << 24) | 0x2000);
  // Word 1: 0x68 (1x1 Rect), Color Red
  writeWord(0x1004, 0x680000FF);
  // Word 2: X=10, Y=10
  writeWord(0x1008, (10 << 16) | 10);

  // Node 2 at 0x2000 sends a Fill Rect
  // Header: 3 words payload, points to 0xFFFFFF (End of Table)
  writeWord(0x2000, (3 << 24) | 0xFFFFFF);
  // Word 1: 0x02 (Fill Rect), Color Green
  writeWord(0x2004, 0x0200FF00);
  // Word 2: X=20, Y=20
  writeWord(0x2008, (20 << 16) | 20);
  // Word 3: W=5, H=5
  writeWord(0x200C, (5 << 16) | 5);

  gpu.processLinkedList(0x1000, ram);

  const auto *vram = gpu.getVRAM();

  // Verify 1x1 Rect from Node 1
  uint16_t red = 31 | (0 << 5) | (0 << 10);
  EXPECT_EQ(vram[10 * 1024 + 10].raw, red);

  // Verify Fill Rect from Node 2
  uint16_t green = 0 | (31 << 5) | (0 << 10);
  EXPECT_EQ(vram[20 * 1024 + 20].raw, green);
  EXPECT_EQ(vram[24 * 1024 + 24].raw, green);

  gpu.snapshotDisplayBuffer(9u);
  std::vector<Color16> copy(GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT);
  GPU::DisplayState state;
  gpu.copyDisplayVRAM(copy.data(), copy.size(), &state);
  EXPECT_EQ(state.completedSubmissionId, 1u)
      << "the latch must identify the fully completed DMA submission";
  EXPECT_EQ(state.latchFrame, 9u);
}

TEST_F(GpuDmaTest, ReplayPreservesCommandCrossingDmaNodeBoundary) {
  GPU::ReplayConfig replayConfig;
  replayConfig.enabled = true;
  replayConfig.internalScale = 4;
  replayConfig.trueColor = true;
  replayConfig.trueDepthMode = ps1::gpu::replay::ReplayDepthMode::Hybrid;
  gpu.configureReplay(replayConfig);

  // A valid flat triangle split across two linked-list nodes. Provenance is
  // per word, so the disagreement must remain observable at triangle level.
  writeWord(0x1000, (2u << 24) | 0x2000u);
  writeWord(0x1004, 0x200000FFu);
  writeWord(0x1008, (10u << 16) | 10u);
  writeWord(0x2000, (2u << 24) | 0xFFFFFFu);
  writeWord(0x2004, (10u << 16) | 20u);
  writeWord(0x2008, (20u << 16) | 10u);

  gpu.processLinkedList(0x1000, ram);
  gpu.snapshotDisplayBuffer(12);
  const ps1::gpu::replay::CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const ps1::gpu::replay::DrawPacket *draw = nullptr;
  for (const ps1::gpu::replay::Operation &operation : frame->operations()) {
    if (const auto *candidate =
            std::get_if<ps1::gpu::replay::DrawPacket>(&operation)) {
      draw = candidate;
      break;
    }
  }
  ASSERT_NE(draw, nullptr);
  ASSERT_EQ(draw->vertexCount, 3u);
  EXPECT_EQ(draw->vertices[0].provenance.dmaNodeSource, 0x1001u);
  EXPECT_EQ(draw->vertices[0].provenance.dmaTraversalOrdinal, 0u);
  EXPECT_EQ(draw->vertices[1].provenance.dmaNodeSource, 0x2001u);
  EXPECT_EQ(draw->vertices[1].provenance.dmaTraversalOrdinal, 1u);
  EXPECT_EQ(draw->vertices[2].provenance.dmaNodeSource, 0x2001u);
  EXPECT_EQ(draw->vertices[2].provenance.dmaTraversalOrdinal, 1u);

  const ps1::gpu::replay::ReplayTriangleProvenance owner =
      ps1::gpu::replay::captureReplayTriangleProvenance(*draw, 0);
  ASSERT_TRUE(owner.validTopology);
  EXPECT_EQ(owner.completeEvidence(),
            ps1::gpu::replay::ReplayVertexGp0WordSourceExact |
                ps1::gpu::replay::ReplayVertexDmaTraversalExact);
  EXPECT_FALSE(owner.coherentDmaTraversal())
      << "an exact cross-node disagreement must not be collapsed";
}

TEST_F(GpuDmaTest, ReplayCarriesExactLastZeroWordOtAnchor) {
  GPU::ReplayConfig replayConfig;
  replayConfig.enabled = true;
  replayConfig.internalScale = 4;
  replayConfig.pgxp = true;
  replayConfig.trueColor = true;
  replayConfig.trueDepthMode = ps1::gpu::replay::ReplayDepthMode::Hybrid;
  gpu.configureReplay(replayConfig);

  constexpr uint32_t bucket = 0x1004u;
  constexpr uint32_t primitive = 0x2000u;
  constexpr uint32_t previousBucket = 0x1000u;
  writeWord(bucket, primitive);
  writeWord(primitive, (4u << 24) | previousBucket);
  writeWord(primitive + 4u, 0x200000FFu);
  writeWord(primitive + 8u, (10u << 16) | 10u);
  writeWord(primitive + 12u, (10u << 16) | 20u);
  writeWord(primitive + 16u, (20u << 16) | 10u);
  writeWord(previousBucket, 0x00FFFFFFu);

  gpu.processLinkedList(bucket, ram);
  gpu.snapshotDisplayBuffer(13);
  const ps1::gpu::replay::CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const ps1::gpu::replay::DrawPacket *draw = nullptr;
  for (const ps1::gpu::replay::Operation &operation : frame->operations()) {
    if (const auto *candidate =
            std::get_if<ps1::gpu::replay::DrawPacket>(&operation)) {
      draw = candidate;
      break;
    }
  }
  ASSERT_NE(draw, nullptr);
  for (std::size_t index = 0; index < draw->vertexCount; ++index) {
    EXPECT_EQ(draw->vertices[index]
                  .provenance.dmaOrderingTableAnchorSource,
              static_cast<uint64_t>(bucket) + 1u);
    EXPECT_NE(draw->vertices[index].provenance.evidence &
                  ps1::gpu::replay::ReplayVertexOrderingTableAnchorExact,
              0u);
  }
  EXPECT_TRUE(ps1::gpu::replay::captureReplayTriangleProvenance(*draw, 0)
                  .coherentOrderingTableAnchor());
}

TEST_F(GpuDmaTest, TraceRetainsDmaNodeAndProjectionJoinKeys) {
  const char *path = "/tmp/ps1_gpu_dma_provenance_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_GPU_WORDS", "1", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(30);
  GPU::ReplayConfig replayConfig;
  replayConfig.enabled = true;
  replayConfig.internalScale = 4;
  replayConfig.pgxp = true;
  replayConfig.trueColor = true;
  replayConfig.trueDepthMode = ps1::gpu::replay::ReplayDepthMode::Hybrid;
  gpu.configureReplay(replayConfig);

  // One opaque triangle in one linked-list node.
  writeWord(0x1000, (4u << 24) | 0xFFFFFFu);
  writeWord(0x1004, 0x200000FFu);
  writeWord(0x1008, (10u << 16) | 10u);
  writeWord(0x100C, (20u << 16) | 10u);
  writeWord(0x1010, (10u << 16) | 20u);

  ps1::Memory memory;
  for (uint32_t address : {0x1008u, 0x100Cu, 0x1010u}) {
    const uint32_t packed = static_cast<uint32_t>(ram[address]) |
                            (static_cast<uint32_t>(ram[address + 1]) << 8) |
                            (static_cast<uint32_t>(ram[address + 2]) << 16) |
                            (static_cast<uint32_t>(ram[address + 3]) << 24);
    ps1::PGXPValue precision;
    precision.value = packed;
    precision.x = static_cast<int16_t>(packed);
    precision.y = static_cast<int16_t>(packed >> 16);
    precision.z = 100.0;
    precision.qx = precision.x * precision.z;
    precision.qy = precision.y * precision.z;
    precision.nearDepth = 1.0;
    precision.projectionId = 77;
    precision.projectionContext = 88;
    precision.gameProfileCallsite = UINT64_C(0x800123408004567A);
    precision.projectionTransformValueFingerprint = 0x123456789ABCDEF0u;
    precision.projectionTransformLineage = 42;
    precision.modelVertexSource = 0x80030001u;
    precision.modelValueFlowSourceFingerprint = 0x1000u + address;
    precision.modelValueFlowProgramFingerprint = 0x9988776655443322u;
    precision.origin = ps1::PGXPOrigin::GteProjection;
    precision.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z |
                      ps1::PGXPValue::VALID_SIGNED_PROJECTION |
                      ps1::PGXPValue::VALID_TRANSFORM_LINEAGE |
                      ps1::PGXPValue::VALID_MODEL_VERTEX_SOURCE |
                      ps1::PGXPValue::VALID_MODEL_VALUE_FLOW;
    memory.write32Tracked(address, packed, &precision);
  }
  std::copy(std::begin(ram), std::end(ram), memory.ramPtr());

  gpu.processLinkedList(0x1000, memory.ramPtr(), &memory);
  gpu.snapshotDisplayBuffer(30);

  const ps1::gpu::replay::CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const ps1::gpu::replay::DrawPacket *draw = nullptr;
  for (const ps1::gpu::replay::Operation &operation : frame->operations()) {
    if (const auto *candidate =
            std::get_if<ps1::gpu::replay::DrawPacket>(&operation)) {
      draw = candidate;
      break;
    }
  }
  ASSERT_NE(draw, nullptr);
  ASSERT_EQ(draw->vertexCount, 3u);
  for (std::size_t index = 0; index < draw->vertexCount; ++index) {
    const ps1::gpu::replay::ReplayVertexProvenance &provenance =
        draw->vertices[index].provenance;
    EXPECT_EQ(provenance.evidence,
              ps1::gpu::replay::REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE &
                  ~ps1::gpu::replay::ReplayVertexOrderingTableAnchorExact);
    EXPECT_EQ(provenance.gp0WordSource,
              0x1009u + static_cast<uint64_t>(index) * 4u);
    EXPECT_EQ(provenance.dmaListSource, 0x1001u);
    EXPECT_EQ(provenance.dmaNodeSource, 0x1001u);
    EXPECT_EQ(provenance.dmaTraversalOrdinal, 0u);
    EXPECT_EQ(provenance.dmaOrderingTableAnchorSource, 0u);
    EXPECT_EQ(provenance.transformValueFingerprint,
              UINT64_C(0x123456789ABCDEF0));
    EXPECT_EQ(provenance.gameProfileCallsite,
              UINT64_C(0x800123408004567A));
  }
  const ps1::gpu::replay::ReplayTriangleProvenance owner =
      ps1::gpu::replay::captureReplayTriangleProvenance(*draw, 0);
  EXPECT_TRUE(owner.coherentDmaTraversal());
  EXPECT_TRUE(owner.coherentTransformValue());
  const ps1::gpu::replay::ReplayPrecisionCoverage coverage =
      ps1::gpu::replay::measureReplayPrecisionCoverage(std::array{frame});
  EXPECT_EQ(coverage.triangleOwners, 1u);
  EXPECT_EQ(coverage.computedModelFlowOwnerTriangles, 1u);
  EXPECT_EQ(coverage.dmaTraversalOwnerTriangles, 1u);
  EXPECT_EQ(coverage.coherentDmaTraversalOwnerTriangles, 1u);

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string body = buffer.str();
  EXPECT_NE(body.find("\"event\":\"gpu.dma_node\""), std::string::npos) << body;
  EXPECT_NE(body.find("\"node_address\":4096"), std::string::npos) << body;
  EXPECT_NE(body.find("\"traversal_ordinal\":0"), std::string::npos) << body;
  EXPECT_NE(body.find("\"projection_id\":77"), std::string::npos) << body;
  EXPECT_NE(body.find("\"intrinsic_provenance\":224"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"transform_lineage\":42"), std::string::npos) << body;
  EXPECT_NE(body.find("\"model_source\":2147680256"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"model_source_fingerprint\":8200"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"model_program_fingerprint\":"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"dma_provenance\":1"), std::string::npos) << body;
  EXPECT_NE(body.find("\"dma_list_start\":4096"), std::string::npos) << body;
  EXPECT_NE(body.find("\"dma_node_address\":4096"), std::string::npos) << body;
  EXPECT_NE(body.find("\"dma_traversal_ordinal\":0"), std::string::npos)
      << body;
  EXPECT_NE(body.find("\"signed_projection\":1"), std::string::npos) << body;

  ps1::metrics::resetTraceForTesting();
  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_GPU_WORDS");
  std::remove(path);
}
