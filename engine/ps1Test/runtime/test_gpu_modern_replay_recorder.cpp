#include "runtime/gpu/modern_replay_recorder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

ReplayRecorderConfig enabledConfig() {
  ReplayRecorderConfig config;
  config.enabled = true;
  config.internalScale = 4;
  config.pgxp = true;
  config.trueColor = true;
  config.trueDepthMode = ReplayDepthMode::Strict;
  return config;
}

std::vector<ps1::gpu::Color16> nativeVram(uint16_t value = 0) {
  return std::vector<ps1::gpu::Color16>(CrashReplayRecorder::NATIVE_PIXEL_COUNT,
                                        ps1::gpu::Color16{value});
}

DrawPacket polygonPacket(uint8_t opcode, uint64_t sequence,
                         TextureDepth depth = TextureDepth::Indexed4,
                         uint16_t textureX = 0, uint16_t textureY = 0,
                         uint16_t clutX = 512, uint16_t clutY = 400) {
  DrawPacket packet;
  packet.opcode = opcode;
  packet.packetSequence = sequence;
  packet.route = PacketRoute::Projected3D;
  packet.semanticReason = PacketSemanticReason::PolygonDefault;
  packet.fallbackReason = PacketFallbackReason::None;
  packet.textured = (opcode & 0x04u) != 0;
  packet.gouraud = (opcode & 0x10u) != 0;
  packet.rawTexture = packet.textured && (opcode & 0x01u) != 0;
  packet.vertexCount = (opcode & 0x08u) != 0 ? 4 : 3;
  packet.indexCount = packet.vertexCount == 4 ? 6 : 3;
  packet.validMask = static_cast<uint8_t>((1u << packet.vertexCount) - 1u);
  packet.triangleIndices = packet.vertexCount == 4
                               ? std::array<uint8_t, 6>{0, 1, 2, 1, 2, 3}
                               : std::array<uint8_t, 6>{0, 1, 2, 0, 0, 0};

  packet.state.texturePage.baseX = textureX;
  packet.state.texturePage.baseY = textureY;
  packet.state.texturePage.depth = depth;
  packet.state.clut.x = clutX;
  packet.state.clut.y = clutY;
  packet.state.textureUvDomain.enabled = packet.textured;
  packet.state.blend.enabled = (opcode & 0x02u) != 0;
  packet.state.blend.texturedStpOnly =
      packet.textured && packet.state.blend.enabled;
  packet.state.depth.epoch = 1;

  for (uint8_t index = 0; index < packet.vertexCount; ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.nativeX = 10 + (index & 1u) * 20;
    vertex.nativeY = 20 + (index >> 1u) * 20;
    vertex.x = static_cast<double>(vertex.nativeX) + 0.25;
    vertex.y = static_cast<double>(vertex.nativeY) + 0.5;
    vertex.u = static_cast<double>(index * 3u);
    vertex.v = static_cast<double>(index * 5u);
    vertex.w = 16000.0 + index;
    vertex.preciseXY = true;
    vertex.preciseUV = packet.textured;
  }
  return packet;
}

PaintRect pointPaint(int32_t x, MaskState mask = {}) {
  PaintRect operation;
  operation.destination = {x, 30, 1, 1};
  operation.nativeColor = 0x1234;
  operation.color = {40, 80, 120};
  operation.mask = mask;
  operation.depth.epoch = 1;
  operation.depth.invalidateOnAccepted = true;
  operation.coalescedCommandCount = 1;
  return operation;
}

template <typename T>
std::vector<const T *> operationsOf(const CrashReplayFrame &frame) {
  std::vector<const T *> result;
  for (const Operation &operation : frame.operations()) {
    if (const auto *typed = std::get_if<T>(&operation))
      result.push_back(typed);
  }
  return result;
}

} // namespace

TEST(GpuModernReplayRecorderTest,
     ConfigurationIsIndependentAndOpcodeFamiliesAreComplete) {
  CrashReplayRecorder disabled;
  EXPECT_FALSE(disabled.enabled());
  EXPECT_EQ(disabled.observeOpcode(0x24), UNASSIGNED_PACKET_SEQUENCE);
  EXPECT_EQ(disabled.opcodeCoverage(0x24), OpcodeCoverage{});

  const ReplayRecorderConfig config = enabledConfig();
  CrashReplayRecorder recorder(config);
  EXPECT_TRUE(recorder.enabled());
  EXPECT_EQ(recorder.config(), config);
  EXPECT_EQ(recorder.config().internalScale, 4u);
  EXPECT_EQ(recorder.config().trueDepthMode, ReplayDepthMode::Strict);
  const ReplayPolicyState expectedPolicy{4, true, true,
                                         ReplayDepthMode::Strict};
  EXPECT_EQ(recorder.policy(), expectedPolicy);

  for (uint16_t opcode = 0x20; opcode <= 0x7F; ++opcode) {
    EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(opcode));
    EXPECT_TRUE(CrashReplayRecorder::opcodeRequiresReplayOperation(opcode));
  }
  for (uint16_t opcode = 0x80; opcode <= 0xBF; ++opcode) {
    EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(opcode));
    EXPECT_TRUE(CrashReplayRecorder::opcodeRequiresReplayOperation(opcode));
  }
  for (uint16_t opcode = 0xC0; opcode <= 0xDF; ++opcode) {
    EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(opcode));
    EXPECT_FALSE(CrashReplayRecorder::opcodeRequiresReplayOperation(opcode));
  }
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0x02));
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0x68));
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0x40));
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0x60));
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0x69));
  EXPECT_FALSE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0xE0));
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0xE1));
  EXPECT_TRUE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0xE6));
  EXPECT_FALSE(CrashReplayRecorder::isCrashReplayV1OpcodeSupported(0xE7));
}

TEST(GpuModernReplayRecorderTest,
     ConfigurationIsStickyAfterFirstPublishedLatchEvenWhenConsumed) {
  CrashReplayRecorder recorder;
  const ReplayRecorderConfig enabled = enabledConfig();

  // Constructor configuration and replacement before publication remain
  // legal. This is the only window in which the capture policy may change.
  recorder.configure(enabled);
  recorder.configure(enabled);
  ASSERT_TRUE(recorder.sealAndPublish({}));
  ASSERT_TRUE(recorder.tryConsume());
  ASSERT_FALSE(recorder.tryConsume());

  EXPECT_THROW(recorder.configure(enabled), std::logic_error);
  ReplayRecorderConfig disabled = enabled;
  disabled.enabled = false;
  EXPECT_THROW(recorder.configure(disabled), std::logic_error);
}

TEST(GpuModernReplayRecorderTest,
     CoverageSequencesAndMailboxRepresentCompletePacketFamilies) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  EXPECT_EQ(recorder.observeOpcode(0x00), 0u);
  EXPECT_EQ(recorder.observeOpcode(0xC3), 1u);

  const uint64_t drawSequence = recorder.observeOpcode(0x3F);
  recorder.recordDraw(polygonPacket(0x3F, drawSequence), vram);

  EXPECT_EQ(recorder.observeOpcode(0x9F), 3u);
  CopyRect copy;
  copy.source = {5, 6};
  copy.destination = {10, 11};
  copy.width = 8;
  copy.height = 4;
  recorder.recordCopy(0x9F, copy, vram);

  const uint64_t uploadSequence = recorder.observeOpcode(0xBF);
  EXPECT_EQ(uploadSequence, 4u);
  recorder.beginCpuUpload(0xBF, uploadSequence, {100, 200, 1, 1}, {});
  recorder.appendCpuUploadPixel(0x4321, true);
  recorder.finishCpuUpload();

  EXPECT_EQ(recorder.observeOpcode(0x68), 5u);
  recorder.recordPaintRect(0x68, pointPaint(10));

  FrameLatch requested;
  requested.frameNumber = 9;
  const CrashReplayFramePtr frame = recorder.sealAndPublish(requested);
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);
  EXPECT_EQ(frame->latch().sequenceBegin, 0u);
  EXPECT_EQ(frame->latch().sequenceEnd, 6u);

  EXPECT_EQ(recorder.opcodeCoverage(0x00), (OpcodeCoverage{1, 1, 0, 0}));
  EXPECT_EQ(recorder.opcodeCoverage(0xC3), (OpcodeCoverage{1, 1, 0, 0}));
  EXPECT_EQ(recorder.opcodeCoverage(0x3F), (OpcodeCoverage{1, 1, 1, 0}));
  EXPECT_EQ(recorder.opcodeCoverage(0x9F), (OpcodeCoverage{1, 1, 1, 0}));
  EXPECT_EQ(recorder.opcodeCoverage(0xBF), (OpcodeCoverage{1, 1, 1, 0}));
  EXPECT_EQ(recorder.opcodeCoverage(0x68), (OpcodeCoverage{1, 1, 1, 0}));

  ASSERT_EQ(operationsOf<DrawPacket>(*frame).size(), 1u);
  EXPECT_EQ(operationsOf<DrawPacket>(*frame).front()->packetSequence,
            drawSequence);
  const auto uploads = operationsOf<CpuUpload>(*frame);
  ASSERT_EQ(uploads.size(), 3u);
  EXPECT_EQ(uploads[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(uploads[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(uploads[1]->pixels, (std::vector<uint16_t>{0x4321}));
  EXPECT_EQ(uploads[2]->phase, CpuUploadPhase::Complete);
  EXPECT_EQ(uploads[2]->pixelOffset, 1u);
  for (const CpuUpload *upload : uploads)
    EXPECT_EQ(upload->transactionId, uploadSequence);

  const CrashReplayFramePtr consumed = recorder.tryConsume();
  ASSERT_TRUE(consumed);
  EXPECT_EQ(consumed.get(), frame.get());
  EXPECT_FALSE(recorder.tryConsume());
}

TEST(GpuModernReplayRecorderTest,
     MissingRequiredAndUnsupportedPacketsFailOnlyTheirFrameLatch) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  EXPECT_EQ(recorder.observeOpcode(0x20), 0u);
  EXPECT_EQ(recorder.observeOpcode(0x40), 1u);
  EXPECT_EQ(recorder.observeOpcode(0x02), 2u);
  recorder.markPacketUnsupported(0x02);
  const CrashReplayFramePtr incomplete = recorder.sealAndPublish({});
  ASSERT_TRUE(incomplete);
  EXPECT_FALSE(incomplete->latch().complete);
  EXPECT_EQ(incomplete->latch().unsupportedCount, 3u);
  EXPECT_EQ(incomplete->latch().unrecoverableCount, 0u)
      << "native visual writes are dominated by the exact latch snapshot";
  EXPECT_EQ(incomplete->latch().sequenceBegin, 0u);
  EXPECT_EQ(incomplete->latch().sequenceEnd, 3u);
  EXPECT_EQ(recorder.opcodeCoverage(0x20), (OpcodeCoverage{1, 1, 0, 1}));
  EXPECT_EQ(recorder.opcodeCoverage(0x40), (OpcodeCoverage{1, 1, 0, 1}));
  EXPECT_EQ(recorder.opcodeCoverage(0x02), (OpcodeCoverage{1, 1, 0, 1}));

  const CrashReplayFramePtr next = recorder.sealAndPublish({});
  ASSERT_TRUE(next);
  EXPECT_TRUE(next->latch().complete);
  EXPECT_EQ(next->latch().unsupportedCount, 0u);
  EXPECT_EQ(next->latch().unrecoverableCount, 0u);
  EXPECT_EQ(next->latch().sequenceBegin, 3u);
  EXPECT_EQ(next->latch().sequenceEnd, 3u);
}

TEST(GpuModernReplayRecorderTest,
     UnknownStateAndTransferFramingRemainHardReanchorBarriers) {
  CrashReplayRecorder recorder(enabledConfig());

  EXPECT_EQ(recorder.observeOpcode(0xE7), 0u);
  EXPECT_EQ(recorder.observeOpcode(0xA0), 1u);
  recorder.markPacketUnsupported(0xA0);
  const CrashReplayFramePtr frame = recorder.sealAndPublish({});

  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 2u);
  EXPECT_EQ(frame->latch().unrecoverableCount, 2u)
      << "a color snapshot cannot reconstruct unknown GPU state or an A0 "
         "continuation";
}

TEST(GpuModernReplayRecorderTest,
     SurfaceSeedIsImmutableAndSynchronizesNativeAndQueuedRevisions) {
  auto vram = nativeVram(0x1111);
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram, 7);
  ASSERT_TRUE(recorder.surfaceSeeded());
  ASSERT_EQ(recorder.nativeRevision(), 1u);
  EXPECT_EQ(recorder.nativeTileRevision(0), 1u);
  EXPECT_EQ(recorder.queuedTileRevision(0), 1u);
  EXPECT_EQ(recorder.nativeTileRevision(CrashReplayRecorder::TILE_COUNT - 1),
            1u);
  EXPECT_EQ(recorder.queuedTileRevision(CrashReplayRecorder::TILE_COUNT - 1),
            1u);

  Reset reset;
  reset.nextDepthEpoch = 8;
  recorder.recordReset(reset);
  EXPECT_EQ(recorder.nativeRevision(), 1u)
      << "GP1 reset preserves native VRAM and its tile timeline";

  vram.front().raw = 0x7777;
  const CrashReplayFramePtr frame = recorder.sealAndPublish({});
  const auto seeds = operationsOf<SurfaceSeed>(*frame);
  ASSERT_EQ(seeds.size(), 1u);
  ASSERT_EQ(seeds.front()->pixels.size(),
            CrashReplayRecorder::NATIVE_PIXEL_COUNT);
  EXPECT_EQ(seeds.front()->pixels.front(), 0x1111);
  EXPECT_EQ(seeds.front()->nativeRevision, 1u);
  EXPECT_EQ(seeds.front()->depthEpoch, 7u);
  const auto resets = operationsOf<Reset>(*frame);
  ASSERT_EQ(resets.size(), 1u);
  EXPECT_EQ(*resets.front(), reset);
}

TEST(GpuModernReplayRecorderTest,
     AcceptedPaintPixelsAdvanceOnlyTouchedTilesOncePerCommand) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  EXPECT_EQ(recorder.observeOpcode(0x40), 0u);
  PaintPixels paint;
  paint.pixels = {
      PaintedPixel{NativePoint{1, 1}, 0x001F, Rgb8{255, 0, 0}, false},
      PaintedPixel{NativePoint{7, 7}, 0x03E0, Rgb8{0, 255, 0}, false},
      PaintedPixel{NativePoint{16, 8}, 0x7C00, Rgb8{0, 0, 255}, false},
  };
  paint.depth.epoch = 1;
  paint.depth.invalidateOnAccepted = true;
  recorder.recordPaintPixels(0x40, std::move(paint));

  EXPECT_EQ(recorder.nativeRevision(), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(0), 2u);
  constexpr uint32_t distantTile =
      (8u / CrashReplayRecorder::TILE_HEIGHT) *
          CrashReplayRecorder::TILES_X +
      16u / CrashReplayRecorder::TILE_WIDTH;
  EXPECT_EQ(recorder.nativeTileRevision(distantTile), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(1), 1u)
      << "an untouched tile inside the sparse bounding box must stay clean";

  const CrashReplayFramePtr frame = recorder.sealAndPublish({});
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->latch().complete);
  ASSERT_EQ(operationsOf<PaintPixels>(*frame).size(), 1u);
  EXPECT_EQ(recorder.opcodeCoverage(0x40), (OpcodeCoverage{1, 1, 1, 0}));
}

TEST(GpuModernReplayRecorderTest,
     NativeReadSetsCoverIndexedPagesClutAndWrappedDirectPages) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  TexturePageState indexed4;
  indexed4.baseX = 0;
  indexed4.baseY = 0;
  indexed4.depth = TextureDepth::Indexed4;
  ClutState clut4;
  clut4.x = 512;
  clut4.y = 400;
  const NativeReadDependency read4 =
      recorder.prepareNativeRead(indexed4, clut4, vram);
  EXPECT_TRUE(read4.required);
  EXPECT_EQ(read4.tileCount, 258u); // 8x32 page + 2 CLUT tiles.

  TexturePageState indexed8 = indexed4;
  indexed8.depth = TextureDepth::Indexed8;
  ClutState clut8;
  clut8.x = 1008;
  clut8.y = 400;
  const NativeReadDependency read8 =
      recorder.prepareNativeRead(indexed8, clut8, vram);
  EXPECT_TRUE(read8.required);
  EXPECT_EQ(read8.tileCount, 544u); // 16x32 page + 32 wrapped CLUT tiles.

  TexturePageState direct;
  direct.baseX = 960;
  direct.baseY = 256;
  direct.depth = TextureDepth::Direct15;
  const NativeReadDependency wrapped =
      recorder.prepareNativeRead(direct, clut4, vram);
  const NativeReadDependency wrappedWithOtherClut =
      recorder.prepareNativeRead(direct, clut8, vram);
  EXPECT_TRUE(wrapped.required);
  EXPECT_EQ(wrapped.tileCount, 1024u); // 32x32 tiles across the X wrap.
  EXPECT_EQ(wrapped, wrappedWithOtherClut)
      << "direct-color pages must not depend on CLUT state";

  direct.textureDisable = true;
  const NativeReadDependency disabled =
      recorder.prepareNativeRead(direct, clut4, vram);
  EXPECT_FALSE(disabled.required);
  EXPECT_EQ(disabled.tileCount, 0u);
  EXPECT_EQ(disabled.versionHash, CRASH_REPLAY_READ_HASH_OFFSET);
}

TEST(GpuModernReplayRecorderTest,
     DirtyTileUploadPrecedesDrawAndOwnsAnImmutableNativeSnapshot) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  TexturePageState page;
  page.depth = TextureDepth::Indexed4;
  ClutState clut;
  clut.x = 512;
  clut.y = 400;
  const NativeReadDependency baseline =
      recorder.prepareNativeRead(page, clut, vram);

  vram[0].raw = 0x1234;
  EXPECT_EQ(recorder.markNativeWrite(NativePoint{0, 0}), 2u);
  const uint64_t sequence = recorder.observeOpcode(0x25);
  recorder.recordDraw(polygonPacket(0x25, sequence), vram);
  vram[0].raw = 0x7777;

  const CrashReplayFramePtr frame = recorder.sealAndPublish({});
  ASSERT_EQ(frame->operations().size(), 4u);
  EXPECT_TRUE(std::holds_alternative<SurfaceSeed>(frame->operations()[0]));
  ASSERT_TRUE(std::holds_alternative<NativeTileUpload>(frame->operations()[1]));
  ASSERT_TRUE(std::holds_alternative<DrawPacket>(frame->operations()[2]));
  EXPECT_TRUE(std::holds_alternative<FrameLatch>(frame->operations()[3]));

  const auto &seed = std::get<SurfaceSeed>(frame->operations()[0]);
  const auto &upload = std::get<NativeTileUpload>(frame->operations()[1]);
  const auto &draw = std::get<DrawPacket>(frame->operations()[2]);
  EXPECT_EQ(seed.pixels.front(), 0u);
  EXPECT_EQ(upload.destination, (NativeRect{0, 0, 8, 8}));
  ASSERT_EQ(upload.pixels.size(), 64u);
  EXPECT_EQ(upload.pixels.front(), 0x1234);
  EXPECT_EQ(upload.nativeRevision, 2u);
  EXPECT_TRUE(draw.nativeRead.required);
  EXPECT_EQ(draw.nativeRead.tileCount, 258u);
  EXPECT_NE(draw.nativeRead.versionHash, baseline.versionHash);
  EXPECT_EQ(recorder.nativeTileRevision(0), 2u);
  EXPECT_EQ(recorder.queuedTileRevision(0), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(1), 1u);
  EXPECT_EQ(recorder.queuedTileRevision(1), 1u);

  const NativeReadDependency repeated =
      recorder.prepareNativeRead(page, clut, vram);
  EXPECT_EQ(repeated, draw.nativeRead);
  const CrashReplayFramePtr clean = recorder.sealAndPublish({});
  EXPECT_EQ(clean->mutationCount(), 0u)
      << "a pre-read at the queued revision must not duplicate an upload";
}

TEST(GpuModernReplayRecorderTest,
     DirtyWrappedCopySourceUploadsAfterPriorDrawAndBeforeCopy) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  const uint64_t drawSequence = recorder.observeOpcode(0x20);
  recorder.recordDraw(polygonPacket(0x20, drawSequence), vram);

  constexpr std::array<uint32_t, 4> sourceTiles{0, 127, 8064, 8191};
  for (const uint32_t tile : sourceTiles) {
    const uint32_t tileX = tile % CrashReplayRecorder::TILES_X;
    const uint32_t tileY = tile / CrashReplayRecorder::TILES_X;
    vram[static_cast<std::size_t>(tileY * CrashReplayRecorder::TILE_HEIGHT) *
             CrashReplayRecorder::NATIVE_WIDTH +
         tileX * CrashReplayRecorder::TILE_WIDTH]
        .raw = static_cast<uint16_t>(0x1000u + tileX + tileY);
  }
  EXPECT_EQ(recorder.markNativeWrite({1022, 510, 4, 4}), 2u);

  const uint64_t copySequence = recorder.observeOpcode(0x80);
  EXPECT_EQ(copySequence, drawSequence + 1u);
  CopyRect copy;
  copy.source = {1022, 510};
  copy.destination = {100, 100};
  copy.width = 4;
  copy.height = 4;
  recorder.recordCopy(0x80, copy, vram);

  for (const uint32_t tile : sourceTiles)
    EXPECT_EQ(recorder.queuedTileRevision(tile), 2u);
  vram.front().raw = 0x7777;

  const CrashReplayFramePtr frame = recorder.sealAndPublish({});
  ASSERT_EQ(frame->operations().size(), 8u);
  EXPECT_TRUE(std::holds_alternative<SurfaceSeed>(frame->operations()[0]));
  EXPECT_TRUE(std::holds_alternative<DrawPacket>(frame->operations()[1]));
  for (std::size_t index = 0; index < sourceTiles.size(); ++index) {
    ASSERT_TRUE(std::holds_alternative<NativeTileUpload>(
        frame->operations()[2 + index]));
    const NativeTileUpload &upload =
        std::get<NativeTileUpload>(frame->operations()[2 + index]);
    const uint32_t tile = sourceTiles[index];
    EXPECT_EQ(upload.destination,
              (NativeRect{static_cast<int32_t>(
                              (tile % CrashReplayRecorder::TILES_X) *
                              CrashReplayRecorder::TILE_WIDTH),
                          static_cast<int32_t>(
                              (tile / CrashReplayRecorder::TILES_X) *
                              CrashReplayRecorder::TILE_HEIGHT),
                          CrashReplayRecorder::TILE_WIDTH,
                          CrashReplayRecorder::TILE_HEIGHT}));
    EXPECT_EQ(upload.nativeRevision, 2u);
  }
  ASSERT_TRUE(std::holds_alternative<CopyRect>(frame->operations()[6]));
  const CopyRect &recorded = std::get<CopyRect>(frame->operations()[6]);
  EXPECT_TRUE(recorded.nativeRead.required);
  EXPECT_EQ(recorded.nativeRead.tileCount, sourceTiles.size());
  uint64_t expectedHash = CRASH_REPLAY_READ_HASH_OFFSET;
  for (const uint32_t tile : sourceTiles) {
    expectedHash =
        appendNativeReadDependencyHash(expectedHash, tile, uint64_t{2});
  }
  EXPECT_EQ(recorded.nativeRead.versionHash, expectedHash);
  EXPECT_EQ(std::get<NativeTileUpload>(frame->operations()[2]).pixels.front(),
            0x1000u)
      << "the queued copy source must own its pre-copy native snapshot";
  EXPECT_TRUE(std::holds_alternative<FrameLatch>(frame->operations()[7]));
}

TEST(GpuModernReplayRecorderTest,
     NativeReadThroughCacheUploadsLatestCopyPaintAndCpuUploadTileOnce) {
  auto vram = nativeVram();
  vram[0].raw = 0x1111;
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  recorder.observeOpcode(0x80);
  CopyRect firstCopy;
  firstCopy.source = {0, 0};
  firstCopy.destination = {16, 16};
  firstCopy.width = 1;
  firstCopy.height = 1;
  recorder.recordCopy(0x80, firstCopy, vram);
  vram[16u * CrashReplayRecorder::NATIVE_WIDTH + 16u].raw = vram[0].raw;
  EXPECT_EQ(recorder.markNativeWrite(firstCopy.destination), 2u);

  recorder.observeOpcode(0x68);
  PaintRect paint = pointPaint(17);
  paint.destination.y = 17;
  paint.nativeColor = 0x2222;
  recorder.recordPaintRect(0x68, paint);
  vram[17u * CrashReplayRecorder::NATIVE_WIDTH + 17u].raw =
      paint.nativeColor;
  EXPECT_EQ(recorder.markNativeWrite(paint.destination), 3u);

  const uint64_t uploadTransaction = recorder.observeOpcode(0xA0);
  recorder.beginCpuUpload(0xA0, uploadTransaction, {18, 18, 1, 1}, {});
  recorder.appendCpuUploadPixel(0x3333, true);
  vram[18u * CrashReplayRecorder::NATIVE_WIDTH + 18u].raw = 0x3333;
  recorder.finishCpuUpload();
  EXPECT_EQ(recorder.nativeRevision(), 4u);

  recorder.observeOpcode(0x80);
  CopyRect secondCopy;
  secondCopy.source = {16, 16};
  secondCopy.destination = {64, 64};
  secondCopy.width = 8;
  secondCopy.height = 8;
  recorder.recordCopy(0x80, secondCopy, vram);

  constexpr uint32_t destinationTile = 2u + 2u * 128u;
  EXPECT_EQ(recorder.nativeTileRevision(destinationTile), 4u);
  EXPECT_EQ(recorder.queuedTileRevision(destinationTile), 4u);

  const CrashReplayFramePtr frame = recorder.sealAndPublish({});
  ASSERT_EQ(frame->operations().size(), 9u);
  EXPECT_TRUE(std::holds_alternative<SurfaceSeed>(frame->operations()[0]));
  EXPECT_TRUE(std::holds_alternative<CopyRect>(frame->operations()[1]));
  EXPECT_TRUE(std::holds_alternative<PaintRect>(frame->operations()[2]));
  EXPECT_EQ(std::get<CpuUpload>(frame->operations()[3]).phase,
            CpuUploadPhase::Begin);
  EXPECT_EQ(std::get<CpuUpload>(frame->operations()[4]).phase,
            CpuUploadPhase::Data);
  EXPECT_EQ(std::get<CpuUpload>(frame->operations()[5]).phase,
            CpuUploadPhase::Complete);
  ASSERT_TRUE(
      std::holds_alternative<NativeTileUpload>(frame->operations()[6]));
  ASSERT_TRUE(std::holds_alternative<CopyRect>(frame->operations()[7]));
  EXPECT_TRUE(std::holds_alternative<FrameLatch>(frame->operations()[8]));

  const auto uploads = operationsOf<NativeTileUpload>(*frame);
  ASSERT_EQ(uploads.size(), 1u)
      << "multiple mutations may leave nativeShadow stale, but the next "
         "reader needs only the latest authoritative whole-tile snapshot";
  const NativeTileUpload &upload = *uploads.front();
  EXPECT_EQ(upload.destination, (NativeRect{16, 16, 8, 8}));
  EXPECT_EQ(upload.nativeRevision, 4u);
  ASSERT_EQ(upload.pixels.size(), 64u);
  EXPECT_EQ(upload.pixels[0], 0x1111u);
  EXPECT_EQ(upload.pixels[1u * 8u + 1u], 0x2222u);
  EXPECT_EQ(upload.pixels[2u * 8u + 2u], 0x3333u);

  const CopyRect &recordedSecond =
      std::get<CopyRect>(frame->operations()[7]);
  EXPECT_TRUE(recordedSecond.nativeRead.required);
  EXPECT_EQ(recordedSecond.nativeRead.tileCount, 1u);
  const uint64_t expectedHash = appendNativeReadDependencyHash(
      CRASH_REPLAY_READ_HASH_OFFSET, destinationTile, uint64_t{4});
  EXPECT_EQ(recordedSecond.nativeRead.versionHash, expectedHash);
}

TEST(GpuModernReplayRecorderTest,
     OneWrappedNativeWriteAssignsOneRevisionToEveryTouchedTile) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  EXPECT_EQ(recorder.markNativeWrite({1022, 510, 4, 4}), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(0), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(127), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(63 * 128), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(63 * 128 + 127), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(1), 1u);
  EXPECT_EQ(recorder.markNativeWrite({10, 10, 0, 4}), 2u);
}

TEST(GpuModernReplayRecorderTest,
     CpuUploadFlushesPartialRowsAtVBlankWithoutMakingLatchIncomplete) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  const uint64_t firstTransaction = recorder.observeOpcode(0xA0);
  EXPECT_EQ(firstTransaction, 0u);
  recorder.beginCpuUpload(0xA0, firstTransaction, {1022, 510, 3, 3},
                          {true, true});
  recorder.appendCpuUploadPixel(0x0001, true);
  recorder.appendCpuUploadPixel(0x0002, false);
  recorder.appendCpuUploadPixel(0x0003, true);
  recorder.appendCpuUploadPixel(0x0004, true);

  const CrashReplayFramePtr first = recorder.sealAndPublish({});
  EXPECT_TRUE(recorder.cpuUploadActive());
  EXPECT_EQ(recorder.cpuUploadConsumedPixels(), 4u);
  EXPECT_TRUE(first->latch().complete);
  EXPECT_EQ(first->latch().unsupportedCount, 0u);
  EXPECT_EQ(first->latch().sequenceBegin, 0u);
  EXPECT_EQ(first->latch().sequenceEnd, 1u);
  const auto firstChunks = operationsOf<CpuUpload>(*first);
  ASSERT_EQ(firstChunks.size(), 2u);
  EXPECT_EQ(firstChunks[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(firstChunks[0]->transactionId, firstTransaction);
  EXPECT_EQ(firstChunks[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(firstChunks[1]->transactionId, firstTransaction);
  EXPECT_EQ(firstChunks[1]->pixelOffset, 0u);
  EXPECT_EQ(firstChunks[1]->pixels, (std::vector<uint16_t>{1, 2, 3, 4}));
  EXPECT_EQ(firstChunks[1]->acceptedBits, (std::vector<uint64_t>{0b1101}));
  EXPECT_EQ(firstChunks[1]->transfer, (NativeRect{1022, 510, 3, 3}));
  EXPECT_EQ(recorder.nativeRevision(), 2u)
      << "all accepted pixels in one VBlank chunk share one revision";
  EXPECT_EQ(recorder.nativeTileRevision(63 * 128), 2u);
  EXPECT_EQ(recorder.nativeTileRevision(63 * 128 + 127), 2u);

  recorder.appendCpuUploadPixel(0x0005, false);
  recorder.appendCpuUploadPixel(0x0006, false);
  recorder.appendCpuUploadPixel(0x0007, false);
  recorder.appendCpuUploadPixel(0x0008, false);
  recorder.appendCpuUploadPixel(0x0009, true);
  recorder.finishCpuUpload();
  const CrashReplayFramePtr second = recorder.sealAndPublish({});
  EXPECT_FALSE(recorder.cpuUploadActive());
  EXPECT_TRUE(second->latch().complete);
  EXPECT_EQ(second->latch().sequenceBegin, 1u);
  EXPECT_EQ(second->latch().sequenceEnd, 1u);
  const auto secondChunks = operationsOf<CpuUpload>(*second);
  ASSERT_EQ(secondChunks.size(), 2u);
  EXPECT_EQ(secondChunks[0]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(secondChunks[0]->transactionId, firstTransaction);
  EXPECT_EQ(secondChunks[0]->pixelOffset, 4u);
  EXPECT_EQ(secondChunks[0]->pixels.size(), 5u);
  EXPECT_EQ(secondChunks[0]->acceptedBits, (std::vector<uint64_t>{0b10000}));
  EXPECT_EQ(secondChunks[1]->phase, CpuUploadPhase::Complete);
  EXPECT_EQ(secondChunks[1]->transactionId, firstTransaction);
  EXPECT_EQ(secondChunks[1]->pixelOffset, 9u);
  EXPECT_EQ(recorder.nativeRevision(), 3u);
  EXPECT_EQ(recorder.nativeTileRevision(0), 3u)
      << "the last transfer row wraps Y=512 to native row zero";

  const uint64_t rejectedTransaction = recorder.observeOpcode(0xA1);
  EXPECT_EQ(rejectedTransaction, 1u);
  recorder.beginCpuUpload(0xA1, rejectedTransaction, {30, 40, 2, 1}, {});
  recorder.appendCpuUploadPixel(0xAAAA, false);
  recorder.appendCpuUploadPixel(0xBBBB, false);
  recorder.finishCpuUpload();
  const CrashReplayFramePtr rejected = recorder.sealAndPublish({});
  EXPECT_TRUE(rejected->latch().complete);
  EXPECT_EQ(recorder.nativeRevision(), 3u)
      << "mask-rejected source words must not dirty native tiles";
  const auto rejectedChunks = operationsOf<CpuUpload>(*rejected);
  ASSERT_EQ(rejectedChunks.size(), 3u);
  EXPECT_EQ(rejectedChunks[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(rejectedChunks[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(rejectedChunks[1]->acceptedBits, (std::vector<uint64_t>{0}));
  EXPECT_EQ(rejectedChunks[2]->phase, CpuUploadPhase::Complete);
  EXPECT_EQ(recorder.opcodeCoverage(0xA0), (OpcodeCoverage{1, 1, 1, 0}));
  EXPECT_EQ(recorder.opcodeCoverage(0xA1), (OpcodeCoverage{1, 1, 1, 0}));

  const uint64_t abortedTransaction = recorder.observeOpcode(0xA2);
  EXPECT_EQ(abortedTransaction, 2u);
  recorder.beginCpuUpload(0xA2, abortedTransaction, {50, 60, 4, 1}, {});
  recorder.appendCpuUploadPixel(0xCCCC, true);
  recorder.abortCpuUpload();
  const CrashReplayFramePtr aborted = recorder.sealAndPublish({});
  EXPECT_FALSE(recorder.cpuUploadActive());
  EXPECT_TRUE(aborted->latch().complete);
  const auto abortedChunks = operationsOf<CpuUpload>(*aborted);
  ASSERT_EQ(abortedChunks.size(), 3u);
  EXPECT_EQ(abortedChunks[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(abortedChunks[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(abortedChunks[1]->pixelOffset, 0u);
  EXPECT_EQ(abortedChunks[1]->pixels, (std::vector<uint16_t>{0xCCCC}));
  EXPECT_EQ(abortedChunks[1]->acceptedBits, (std::vector<uint64_t>{1}));
  EXPECT_EQ(abortedChunks[2]->phase, CpuUploadPhase::Abort);
  EXPECT_EQ(abortedChunks[2]->pixelOffset, 1u);
  EXPECT_EQ(recorder.opcodeCoverage(0xA2), (OpcodeCoverage{1, 1, 1, 0}));

  const uint64_t emptyAbortTransaction = recorder.observeOpcode(0xA3);
  recorder.beginCpuUpload(0xA3, emptyAbortTransaction, {70, 80, 2, 1}, {});
  recorder.abortCpuUpload();
  const CrashReplayFramePtr emptyAbort = recorder.sealAndPublish({});
  const auto emptyAbortChunks = operationsOf<CpuUpload>(*emptyAbort);
  ASSERT_EQ(emptyAbortChunks.size(), 2u);
  EXPECT_EQ(emptyAbortChunks[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(emptyAbortChunks[1]->phase, CpuUploadPhase::Abort);
  EXPECT_EQ(emptyAbortChunks[1]->pixelOffset, 0u);
  EXPECT_EQ(emptyAbortChunks[1]->transactionId, emptyAbortTransaction);
}

TEST(GpuModernReplayRecorderTest,
     AdjacentPointPaintsCoalesceWithoutLosingPacketCoverage) {
  auto vram = nativeVram();
  CrashReplayRecorder recorder(enabledConfig());
  recorder.recordSurfaceSeed(vram);

  for (int32_t x = 10; x < 13; ++x) {
    recorder.observeOpcode(0x68);
    recorder.recordPaintRect(0x68, pointPaint(x));
  }
  const CrashReplayFramePtr frame = recorder.sealAndPublish({});
  const auto paints = operationsOf<PaintRect>(*frame);
  ASSERT_EQ(paints.size(), 1u);
  EXPECT_EQ(paints.front()->destination, (NativeRect{10, 30, 3, 1}));
  EXPECT_EQ(paints.front()->coalescedCommandCount, 3u);
  EXPECT_EQ(recorder.opcodeCoverage(0x68), (OpcodeCoverage{3, 3, 3, 0}));
  EXPECT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().sequenceBegin, 0u);
  EXPECT_EQ(frame->latch().sequenceEnd, 3u);
}
