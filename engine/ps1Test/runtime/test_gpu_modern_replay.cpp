#include "runtime/gpu/modern_replay.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

DrawPacket trianglePacket(uint64_t sequence = 77) {
  DrawPacket packet;
  packet.opcode = 0x36;
  packet.route = PacketRoute::Projected3D;
  packet.semanticReason = PacketSemanticReason::PolygonDefault;
  packet.fallbackReason = PacketFallbackReason::None;
  packet.packetSequence = sequence;
  packet.submissionId = 900;
  packet.validMask = 0x7;
  packet.vertexCount = 3;
  packet.indexCount = 3;
  packet.triangleIndices = {0, 1, 2, 0, 0, 0};
  packet.textured = true;
  packet.gouraud = true;
  packet.nativeRead.required = true;
  packet.nativeRead.tileCount = 2;
  packet.nativeRead.versionHash = appendNativeReadDependencyHash(
      appendNativeReadDependencyHash(CRASH_REPLAY_READ_HASH_OFFSET, 17, 4), 18,
      9);

  packet.vertices[0].nativeX = 8;
  packet.vertices[0].nativeY = 15;
  packet.vertices[0].x = 8.25;
  packet.vertices[0].y = 15.5;
  packet.vertices[0].u = 4.0;
  packet.vertices[0].v = 5.0;
  packet.vertices[0].w = 16384.0;
  packet.vertices[0].color = Rgb8{10, 20, 30};
  packet.vertices[0].preciseXY = true;
  packet.vertices[0].preciseUV = true;

  packet.vertices[1].nativeX = 27;
  packet.vertices[1].nativeY = 15;
  packet.vertices[1].x = 27.0;
  packet.vertices[1].y = 15.0;
  packet.vertices[1].w = 24576.0;
  packet.vertices[2].nativeX = 8;
  packet.vertices[2].nativeY = 44;
  packet.vertices[2].x = 8.0;
  packet.vertices[2].y = 44.0;
  packet.vertices[2].w = 32767.0;

  packet.state.drawArea = DrawArea{10, 11, 500, 250};
  packet.state.drawOffset = DrawOffset{-3, 4};
  packet.state.texturePage = TexturePageState{
      0x19Au, 640, 256, TextureDepth::Indexed8, true, false, false};
  packet.state.clut = ClutState{0x4A1u, 16, 298};
  packet.state.textureWindow = TextureWindowState{0x000A531Fu, 248, 8, 40, 80};
  packet.state.textureUvDomain =
      TextureUvDomainState{4.0, 5.0, 63.0, 95.0, 1, 0, true, true, false};
  packet.state.blend =
      BlendState{BlendMode::BackgroundPlusForeground, true, true};
  packet.state.mask = MaskState{true, true};
  packet.state.depth = DepthState{true, true, DepthCompare::LessOrEqual,
                                  uint64_t{1} << 40, false};
  packet.state.dither = true;
  packet.state.drawToDisplayArea = true;
  return packet;
}

FrameLatch packetLatch(uint64_t begin, uint64_t end) {
  FrameLatch latch;
  latch.sequenceBegin = begin;
  latch.sequenceEnd = end;
  return latch;
}

void setCopyNativeReadProof(CopyRect &copy, uint32_t tileIndex = 0,
                            uint64_t revision = 1) {
  copy.nativeRead.required = true;
  copy.nativeRead.tileCount = 1;
  copy.nativeRead.versionHash = appendNativeReadDependencyHash(
      CRASH_REPLAY_READ_HASH_OFFSET, tileIndex, revision);
}

CrashReplayFramePtr makeFrame(uint64_t frameNumber) {
  CrashReplayFrameBuilder builder;
  builder.record(Reset{});
  FrameLatch latch;
  latch.frameNumber = frameNumber;
  latch.contentRevision = frameNumber + 100;
  return builder.seal(latch);
}

} // namespace

static_assert(CrashReplayFrame::schemaVersion() == 9);
static_assert(CrashReplayFrame::schemaName() == "CrashReplayV1");
static_assert(CRASH_REPLAY_FALLBACK_W == 32768.0);
static_assert(replayDrawFlagBit(ReplayDrawFlag::Textured) == (1u << 0));
static_assert(replayDrawFlagBit(ReplayDrawFlag::RawTexture) == (1u << 1));
static_assert(replayDrawFlagBit(ReplayDrawFlag::SemiTransparent) == (1u << 2));
static_assert(replayDrawFlagBit(ReplayDrawFlag::ForceMask) == (1u << 3));
static_assert(replayDrawFlagBit(ReplayDrawFlag::CheckMask) == (1u << 4));
static_assert(replayDrawFlagBit(ReplayDrawFlag::LimitUvDomain) == (1u << 5));
static_assert(replayDrawFlagBit(ReplayDrawFlag::SurfaceDepthValid) ==
              (1u << 6));
static_assert(std::is_const_v<CrashReplayFramePtr::element_type>,
              "mailbox frames must expose an immutable element type");
static_assert(!std::is_copy_constructible_v<CrashReplayFrame>);
static_assert(std::is_trivially_copyable_v<PaintRect>,
              "the coalesced GP0(68h) path must not own a heap payload");
static_assert(sizeof(ps1::gpu::replay::vulkan_abi::DrawUniforms) == 768);
static_assert(sizeof(ps1::gpu::replay::vulkan_abi::Vertex) == 32);

TEST(GpuModernReplayTest, SealedFrameContainsEveryMutationAndTerminalLatch) {
  CrashReplayFrameBuilder builder;

  NativeTileUpload tile;
  tile.destination = NativeRect{960, 500, 2, 2};
  tile.pixels = {1, 2, 3, 4};
  tile.nativeRevision = 9;
  builder.record(tile);

  SurfaceSeed seed;
  seed.destination = NativeRect{0, 0, 2, 1};
  seed.pixels = {0x1111, 0x8222};
  seed.nativeRevision = 10;
  seed.depthEpoch = uint64_t{1} << 41;
  builder.record(seed);

  const DrawPacket draw = trianglePacket();
  builder.record(draw);

  FillRect fill;
  fill.destination = NativeRect{0, 0, 16, 8};
  fill.color = Rgb8{1, 2, 3};
  fill.nativeColor = 0x0421;
  builder.record(fill);

  CopyRect copy;
  copy.source = NativePoint{8, 9};
  copy.destination = NativePoint{20, 21};
  copy.width = 10;
  copy.height = 11;
  setCopyNativeReadProof(copy);
  copy.mask = MaskState{true, false};
  builder.record(copy);

  CpuUpload cpu;
  cpu.transactionId = 77;
  cpu.phase = CpuUploadPhase::Data;
  cpu.transfer = NativeRect{32, 64, 3, 2};
  cpu.pixelOffset = 2;
  cpu.pixels = {0x1111, 0x2222, 0x8333};
  cpu.acceptedBits = {0b101};
  cpu.mask = MaskState{false, true};
  builder.record(cpu);

  PaintPixels paint;
  paint.pixels = {
      PaintedPixel{NativePoint{1, 2}, 0x0123, Rgb8{8, 9, 10}, false},
      PaintedPixel{NativePoint{3, 4}, 0x8456, Rgb8{11, 12, 13}, true},
  };
  paint.mask = MaskState{true, true};
  paint.depth.invalidateOnAccepted = true;
  builder.record(paint);

  PaintRect paintRect;
  paintRect.destination = NativeRect{50, 60, 4, 1};
  paintRect.nativeColor = 0x1234;
  paintRect.color = Rgb8{17, 18, 19};
  paintRect.depth.invalidateOnAccepted = true;
  paintRect.coalescedCommandCount = 4;
  builder.record(paintRect);

  ClearDepth clear;
  clear.nextEpoch = uint64_t{1} << 42;
  builder.record(clear);

  const Reset reset;
  builder.record(reset);

  ASSERT_EQ(builder.mutationCount(), 10u);
  FrameLatch latch = packetLatch(77, 78);
  latch.frameNumber = 1234;
  latch.contentRevision = 5678;
  latch.completedSubmissionId = 900;
  latch.display =
      DisplayLatchState{NativeRect{512, 0, 512, 256}, true, true, false, false};
  const auto frame = builder.seal(latch);

  ASSERT_TRUE(frame);
  EXPECT_TRUE(builder.isSealed());
  EXPECT_EQ(frame->mutationCount(), 10u);
  ASSERT_EQ(frame->operations().size(), 11u);
  EXPECT_EQ(std::get<NativeTileUpload>(frame->operations()[0]), tile);
  EXPECT_EQ(std::get<SurfaceSeed>(frame->operations()[1]), seed);
  EXPECT_EQ(std::get<DrawPacket>(frame->operations()[2]), draw);
  EXPECT_EQ(std::get<FillRect>(frame->operations()[3]), fill);
  EXPECT_EQ(std::get<CopyRect>(frame->operations()[4]), copy);
  EXPECT_EQ(std::get<CpuUpload>(frame->operations()[5]), cpu);
  EXPECT_EQ(std::get<PaintPixels>(frame->operations()[6]), paint);
  EXPECT_EQ(std::get<PaintRect>(frame->operations()[7]), paintRect);
  EXPECT_EQ(std::get<ClearDepth>(frame->operations()[8]), clear);
  EXPECT_EQ(std::get<Reset>(frame->operations()[9]), reset);
  EXPECT_EQ(std::get<FrameLatch>(frame->operations()[10]), latch);
  EXPECT_EQ(frame->latch(), latch);

  EXPECT_THROW(builder.record(Reset{}), std::logic_error);
  EXPECT_THROW(builder.seal(FrameLatch{}), std::logic_error);
}

TEST(GpuModernReplayTest, DrawPacketValidationIsPacketAtomic) {
  CrashReplayFrameBuilder builder;
  DrawPacket invalid = trianglePacket();
  invalid.triangleIndices[2] = 3;

  EXPECT_THROW(builder.record(invalid), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 0u)
      << "a rejected packet must not leave partial triangles behind";

  DrawPacket blendMismatch = trianglePacket();
  blendMismatch.state.blend.enabled = false;
  EXPECT_THROW(builder.record(blendMismatch), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 0u);

  const DrawPacket valid = trianglePacket();
  EXPECT_NO_THROW(builder.record(valid));
  ASSERT_EQ(builder.mutationCount(), 1u);
  const auto frame = builder.seal(packetLatch(77, 78));
  ASSERT_EQ(frame->mutationCount(), 1u);
  EXPECT_EQ(std::get<DrawPacket>(frame->operations().front()), valid);
}

TEST(GpuModernReplayTest, ShaderStateIsBakedAndVersionedBeforeThePacket) {
  const DrawPacket packet = trianglePacket();
  const uint32_t expectedFlags =
      replayDrawFlagBit(ReplayDrawFlag::Textured) |
      replayDrawFlagBit(ReplayDrawFlag::SemiTransparent) |
      replayDrawFlagBit(ReplayDrawFlag::ForceMask) |
      replayDrawFlagBit(ReplayDrawFlag::CheckMask) |
      replayDrawFlagBit(ReplayDrawFlag::LimitUvDomain) |
      replayDrawFlagBit(ReplayDrawFlag::SurfaceDepthValid);
  EXPECT_EQ(shaderDrawFlags(packet), expectedFlags);
  EXPECT_TRUE(packet.nativeRead.required);
  EXPECT_EQ(packet.nativeRead.tileCount, 2u);
  EXPECT_EQ(packet.nativeRead.versionHash, UINT64_C(0xEA12075AE3B57CEB));
  EXPECT_EQ(packet.state.textureWindow.maskX, 248u);
  EXPECT_TRUE(packet.state.textureUvDomain.flippedU);
  EXPECT_EQ(packet.state.textureUvDomain.offsetU, 1u);

  CrashReplayFrameBuilder builder;
  DrawPacket staleProof = packet;
  staleProof.nativeRead.required = false;
  EXPECT_THROW(builder.record(staleProof), std::invalid_argument);

  DrawPacket packedWindow = packet;
  packedWindow.state.textureWindow.maskX = 31;
  EXPECT_THROW(builder.record(packedWindow), std::invalid_argument);

  DrawPacket missingDomain = packet;
  missingDomain.state.textureUvDomain = {};
  EXPECT_THROW(builder.record(missingDomain), std::invalid_argument);

  DrawPacket invalidEffectiveW = packet;
  invalidEffectiveW.vertices[0].w = 0.0;
  EXPECT_THROW(builder.record(invalidEffectiveW), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 0u);
}

TEST(GpuModernReplayTest, QuadRemainsOnePacketWithTwoIndexedTriangles) {
  CrashReplayFrameBuilder builder;
  DrawPacket quad = trianglePacket();
  quad.opcode = 0x3E;
  quad.vertexCount = 4;
  quad.indexCount = 6;
  quad.validMask = 0xF;
  quad.vertices[3].nativeX = 27;
  quad.vertices[3].nativeY = 44;
  quad.vertices[3].x = 27.0;
  quad.vertices[3].y = 44.0;
  quad.vertices[3].w = 30000.0;
  quad.triangleIndices = {0, 1, 2, 1, 2, 3};

  builder.record(quad);
  const auto frame = builder.seal(packetLatch(77, 78));

  ASSERT_EQ(frame->mutationCount(), 1u);
  const auto &recorded = std::get<DrawPacket>(frame->operations().front());
  EXPECT_EQ(recorded.vertexCount, 4u);
  EXPECT_EQ(recorded.indexCount, 6u);
  EXPECT_EQ(recorded.triangleIndices, quad.triangleIndices);
}

TEST(GpuModernReplayTest, CpuUploadFlushCanStartMidRowAndRetainRejects) {
  CrashReplayFrameBuilder builder;
  CpuUpload flush;
  flush.transactionId = 12;
  flush.phase = CpuUploadPhase::Data;
  flush.transfer = NativeRect{100, 200, 3, 3};
  flush.pixelOffset = 2;
  flush.pixels = {0x1111, 0x2222, 0x3333, 0x4444};
  flush.acceptedBits = {0b0101};
  flush.mask = MaskState{false, true};

  EXPECT_TRUE(flush.acceptedAt(0));
  EXPECT_FALSE(flush.acceptedAt(1));
  EXPECT_TRUE(flush.acceptedAt(2));
  EXPECT_FALSE(flush.acceptedAt(3));
  EXPECT_EQ(flush.destinationAt(0), (NativePoint{102, 200}));
  EXPECT_EQ(flush.destinationAt(1), (NativePoint{100, 201}));
  EXPECT_EQ(flush.destinationAt(3), (NativePoint{102, 201}));

  builder.record(flush);
  const auto frame = builder.seal(FrameLatch{});
  EXPECT_EQ(std::get<CpuUpload>(frame->operations().front()), flush);
}

TEST(GpuModernReplayTest, CpuUploadDestinationsWrapAcrossBothNativeAxes) {
  CpuUpload upload;
  upload.transactionId = 12;
  upload.phase = CpuUploadPhase::Data;
  upload.transfer = NativeRect{1022, 510, 3, 3};
  upload.pixels.resize(9);
  upload.acceptedBits = {0x1FF};

  EXPECT_EQ(upload.destinationAt(0), (NativePoint{1022, 510}));
  EXPECT_EQ(upload.destinationAt(2), (NativePoint{0, 510}));
  EXPECT_EQ(upload.destinationAt(3), (NativePoint{1022, 511}));
  EXPECT_EQ(upload.destinationAt(5), (NativePoint{0, 511}));
  EXPECT_EQ(upload.destinationAt(6), (NativePoint{1022, 0}));
  EXPECT_EQ(upload.destinationAt(8), (NativePoint{0, 0}));
}

TEST(GpuModernReplayTest, NativeMutationExtentsCannotExceedPhysicalVram) {
  CrashReplayFrameBuilder valid;
  FillRect fullFill;
  fullFill.destination = NativeRect{0, 0, 1024, 512};
  EXPECT_NO_THROW(valid.record(fullFill));

  CopyRect fullCopy;
  fullCopy.width = 1024;
  fullCopy.height = 512;
  setCopyNativeReadProof(fullCopy);
  EXPECT_NO_THROW(valid.record(fullCopy));

  CopyRect missingCopyProof = fullCopy;
  missingCopyProof.nativeRead = {};
  CrashReplayFrameBuilder invalidMissingCopyProof;
  EXPECT_THROW(invalidMissingCopyProof.record(missingCopyProof),
               std::invalid_argument);

  CpuUpload fullUpload;
  fullUpload.transactionId = 12;
  fullUpload.phase = CpuUploadPhase::Data;
  fullUpload.transfer = NativeRect{0, 0, 1024, 512};
  fullUpload.pixelOffset = 1024u * 512u - 1u;
  fullUpload.pixels = {0x1234};
  fullUpload.acceptedBits = {1};
  EXPECT_NO_THROW(valid.record(fullUpload));

  FillRect wideFill = fullFill;
  wideFill.destination.width = 1025;
  CrashReplayFrameBuilder invalidWideFill;
  EXPECT_THROW(invalidWideFill.record(wideFill), std::invalid_argument);

  CopyRect tallCopy = fullCopy;
  tallCopy.height = 513;
  CrashReplayFrameBuilder invalidTallCopy;
  EXPECT_THROW(invalidTallCopy.record(tallCopy), std::invalid_argument);

  CpuUpload wideUpload = fullUpload;
  wideUpload.transfer.width = 1025;
  CrashReplayFrameBuilder invalidWideUpload;
  EXPECT_THROW(invalidWideUpload.record(wideUpload), std::invalid_argument);

  PaintRect tallPaint;
  tallPaint.destination = NativeRect{0, 0, 1, 513};
  tallPaint.coalescedCommandCount = 513;
  tallPaint.depth.invalidateOnAccepted = true;
  CrashReplayFrameBuilder invalidTallPaint;
  EXPECT_THROW(invalidTallPaint.record(tallPaint), std::invalid_argument);

  ClearDepth partialClear;
  partialClear.wholeSurface = false;
  partialClear.area = NativeRect{0, 0, 1, 1};
  CrashReplayFrameBuilder invalidPartialClear;
  EXPECT_THROW(invalidPartialClear.record(partialClear), std::invalid_argument);

  Reset depthPreservingReset;
  depthPreservingReset.clearDepth = false;
  CrashReplayFrameBuilder invalidDepthPreservingReset;
  EXPECT_THROW(invalidDepthPreservingReset.record(depthPreservingReset),
               std::invalid_argument);
}

TEST(GpuModernReplayTest, CpuUploadRejectsIncompleteOrAmbiguousFlushMetadata) {
  CrashReplayFrameBuilder builder;
  CpuUpload outside;
  outside.transactionId = 12;
  outside.phase = CpuUploadPhase::Data;
  outside.transfer = NativeRect{0, 0, 2, 2};
  outside.pixelOffset = 2;
  outside.pixels = {1, 2, 3};
  outside.acceptedBits = {0b111};
  EXPECT_THROW(builder.record(outside), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 0u);

  CpuUpload missingBits;
  missingBits.transactionId = 12;
  missingBits.phase = CpuUploadPhase::Data;
  missingBits.transfer = NativeRect{0, 0, 2, 2};
  missingBits.pixels = {1, 2};
  EXPECT_THROW(builder.record(missingBits), std::invalid_argument);

  CpuUpload strayBits = missingBits;
  strayBits.acceptedBits = {uint64_t{1} << 63};
  EXPECT_THROW(builder.record(strayBits), std::invalid_argument);

  NativeTileUpload empty;
  empty.destination = NativeRect{0, 0, 0, 1};
  EXPECT_THROW(builder.record(empty), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 0u);
}

TEST(GpuModernReplayTest, CpuUploadPhasesHaveExplicitTransactionalPayloads) {
  CpuUpload begin;
  begin.transactionId = 41;
  begin.phase = CpuUploadPhase::Begin;
  begin.transfer = NativeRect{20, 30, 2, 2};

  CpuUpload data = begin;
  data.phase = CpuUploadPhase::Data;
  data.pixelOffset = 0;
  data.pixels = {1, 2};
  data.acceptedBits = {0b01};

  CpuUpload complete = begin;
  complete.phase = CpuUploadPhase::Complete;
  complete.pixelOffset = 4;

  CpuUpload abort = begin;
  abort.phase = CpuUploadPhase::Abort;
  abort.pixelOffset = 2;

  CrashReplayFrameBuilder valid;
  EXPECT_NO_THROW(valid.record(begin));
  EXPECT_NO_THROW(valid.record(data));
  EXPECT_NO_THROW(valid.record(complete));
  EXPECT_NO_THROW(valid.record(abort));

  const auto rejected = [](CpuUpload upload) {
    CrashReplayFrameBuilder builder;
    EXPECT_THROW(builder.record(std::move(upload)), std::invalid_argument);
    EXPECT_EQ(builder.mutationCount(), 0u);
  };

  CpuUpload unassigned = begin;
  unassigned.transactionId = UNASSIGNED_PACKET_SEQUENCE;
  rejected(std::move(unassigned));

  CpuUpload beginWithPayload = data;
  beginWithPayload.phase = CpuUploadPhase::Begin;
  rejected(std::move(beginWithPayload));

  CpuUpload emptyData = begin;
  emptyData.phase = CpuUploadPhase::Data;
  rejected(std::move(emptyData));

  CpuUpload earlyComplete = complete;
  earlyComplete.pixelOffset = 3;
  rejected(std::move(earlyComplete));

  CpuUpload lateAbort = abort;
  lateAbort.pixelOffset = 5;
  rejected(std::move(lateAbort));

  CpuUpload invalidPhase = begin;
  invalidPhase.phase = static_cast<CpuUploadPhase>(0xFF);
  rejected(std::move(invalidPhase));
}

TEST(GpuModernReplayTest, VertexCoordinatesAreExplicitlyPostDrawOffset) {
  CrashReplayFrameBuilder builder;
  DrawPacket packet = trianglePacket();
  ASSERT_EQ(packet.state.drawOffset, (DrawOffset{-3, 4}));
  ASSERT_EQ(packet.vertices[0].nativeX, 8);
  ASSERT_DOUBLE_EQ(packet.vertices[0].x, 8.25);
  EXPECT_EQ(packet.vertices[0].coordinateSpace,
            ReplayVertex::CoordinateSpace::PostDrawOffsetNativePixels);
  builder.record(packet);

  DrawPacket wrongSpace = trianglePacket(78);
  wrongSpace.vertices[0].coordinateSpace =
      static_cast<ReplayVertex::CoordinateSpace>(0xFF);
  EXPECT_THROW(builder.record(wrongSpace), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 1u);
}

TEST(GpuModernReplayTest,
     TriangleProvenanceUsesOnlyExactCategoricalVertexEvidence) {
  DrawPacket packet = trianglePacket();
  for (std::size_t index = 0; index < packet.vertexCount; ++index) {
    ReplayVertex &vertex = packet.vertices[index];
    vertex.homogeneousQ = true;
    vertex.provenance.projectionId = 100u + index;
    vertex.provenance.projectionContext = 0;
    vertex.provenance.transformValueFingerprint = 0x123456789ABCDEF0u;
    vertex.provenance.transformLineage = 0xAABBCCDDu;
    vertex.provenance.modelVertexSource = 0x80010001u + index * 8u;
    vertex.provenance.modelValueFlowSourceFingerprint = 0x1000u + index;
    vertex.provenance.modelValueFlowProgramFingerprint = 0xCAFEBABEu;
    vertex.provenance.gp0WordSource = 0x80020001u + index * 4u;
    vertex.provenance.dmaListSource = 0x1001u;
    vertex.provenance.dmaNodeSource = 0x2001u;
    vertex.provenance.dmaOrderingTableAnchorSource = 0x1801u;
    vertex.provenance.dmaTraversalOrdinal = 3u;
    vertex.provenance.evidence = REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE;
  }

  const ReplayTriangleProvenance provenance =
      captureReplayTriangleProvenance(packet, 0);
  ASSERT_TRUE(provenance.validTopology);
  EXPECT_EQ(provenance.completeEvidence(),
            REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE);
  EXPECT_TRUE(provenance.coherentProjectionContext());
  EXPECT_TRUE(provenance.coherentTransformValue());
  EXPECT_TRUE(provenance.coherentTransformLineage());
  EXPECT_TRUE(provenance.coherentOrderingTableAnchor());
  EXPECT_EQ(provenance.vertexIndices, (std::array<uint8_t, 3>{0, 1, 2}));

  DrawPacket visuallyDifferent = packet;
  visuallyDifferent.vertices[0].x = -900.0;
  visuallyDifferent.vertices[1].y = 700.0;
  visuallyDifferent.vertices[2].w = 1234.0;
  visuallyDifferent.vertices[2].color = {255, 0, 255};
  EXPECT_EQ(captureReplayTriangleProvenance(visuallyDifferent, 0), provenance)
      << "screen/depth/colour observations must never enter owner identity";

  packet.vertices[2].provenance.transformLineage = 0x11223344u;
  packet.vertices[2].provenance.transformValueFingerprint = 0x0FEDCBA987654321u;
  const ReplayTriangleProvenance mismatch =
      captureReplayTriangleProvenance(packet, 0);
  EXPECT_EQ(mismatch.completeEvidence(),
            REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE);
  EXPECT_FALSE(mismatch.coherentTransformLineage())
      << "an exact disagreement stays visible instead of being merged";
  EXPECT_FALSE(mismatch.coherentTransformValue())
      << "same-frame numeric equality is observed, never guessed";
  EXPECT_FALSE(captureReplayTriangleProvenance(packet, 1).validTopology);
}

TEST(GpuModernReplayTest, VertexProvenanceValidationFailsClosed) {
  CrashReplayFrameBuilder builder;
  DrawPacket projected = trianglePacket();
  projected.vertices[0].homogeneousQ = true;
  projected.vertices[0].provenance.projectionId = 7;
  projected.vertices[0].provenance.evidence = ReplayVertexProjectionEventExact;
  EXPECT_NO_THROW(builder.record(projected));

  DrawPacket valueWithoutEvidence = trianglePacket(78);
  valueWithoutEvidence.vertices[0].provenance.transformLineage = 9;
  EXPECT_THROW(builder.record(valueWithoutEvidence), std::invalid_argument);

  DrawPacket transformValueWithoutEvidence = trianglePacket(78);
  transformValueWithoutEvidence.vertices[0]
      .provenance.transformValueFingerprint = 9;
  EXPECT_THROW(builder.record(transformValueWithoutEvidence),
               std::invalid_argument);

  DrawPacket modelFlowValueWithoutEvidence = trianglePacket(78);
  modelFlowValueWithoutEvidence.vertices[0]
      .provenance.modelValueFlowProgramFingerprint = 9;
  EXPECT_THROW(builder.record(modelFlowValueWithoutEvidence),
               std::invalid_argument);

  DrawPacket modelFlowWithoutProjection = trianglePacket(78);
  modelFlowWithoutProjection.vertices[0].provenance.evidence =
      ReplayVertexModelValueFlowExact;
  EXPECT_THROW(builder.record(modelFlowWithoutProjection),
               std::invalid_argument);

  DrawPacket dmaValueWithoutEvidence = trianglePacket(78);
  dmaValueWithoutEvidence.vertices[0].provenance.dmaListSource = 0x1001u;
  EXPECT_THROW(builder.record(dmaValueWithoutEvidence), std::invalid_argument);

  DrawPacket dmaWithoutGp0 = trianglePacket(78);
  dmaWithoutGp0.vertices[0].provenance.dmaListSource = 0x1001u;
  dmaWithoutGp0.vertices[0].provenance.dmaNodeSource = 0x2001u;
  dmaWithoutGp0.vertices[0].provenance.evidence = ReplayVertexDmaTraversalExact;
  EXPECT_THROW(builder.record(dmaWithoutGp0), std::invalid_argument);

  DrawPacket incompleteDma = trianglePacket(78);
  incompleteDma.vertices[0].provenance.gp0WordSource = 0x3001u;
  incompleteDma.vertices[0].provenance.dmaListSource = 0x1001u;
  incompleteDma.vertices[0].provenance.evidence =
      ReplayVertexGp0WordSourceExact | ReplayVertexDmaTraversalExact;
  EXPECT_THROW(builder.record(incompleteDma), std::invalid_argument);

  DrawPacket exactDma = trianglePacket(78);
  exactDma.vertices[0].provenance.gp0WordSource = 0x3001u;
  exactDma.vertices[0].provenance.dmaListSource = 0x1001u;
  exactDma.vertices[0].provenance.dmaNodeSource = 0x2001u;
  exactDma.vertices[0].provenance.evidence =
      ReplayVertexGp0WordSourceExact | ReplayVertexDmaTraversalExact;
  EXPECT_NO_THROW(builder.record(exactDma));

  DrawPacket screen = trianglePacket(79);
  screen.opcode = 0x2A;
  screen.route = PacketRoute::Screen2D;
  screen.semanticReason = PacketSemanticReason::DisplayAnchoredComposite;
  screen.validMask = 0;
  screen.vertexCount = 4;
  screen.indexCount = 6;
  screen.triangleIndices = {0, 1, 2, 1, 2, 3};
  screen.textured = false;
  screen.gouraud = false;
  screen.nativeRead = {};
  screen.state.textureUvDomain = {};
  screen.state.depth = DepthState{};
  screen.state.depth.epoch = 1;
  screen.state.depth.invalidateOnAccepted = true;
  for (std::size_t index = 0; index < screen.vertexCount; ++index) {
    screen.vertices[index].nativeX = static_cast<int32_t>(index);
    screen.vertices[index].nativeY = static_cast<int32_t>(index);
    screen.vertices[index].x = static_cast<double>(index);
    screen.vertices[index].y = static_cast<double>(index);
    screen.vertices[index].w = CRASH_REPLAY_FALLBACK_W;
  }
  screen.vertices[0].homogeneousQ = true;
  screen.vertices[0].provenance.projectionId = 8;
  screen.vertices[0].provenance.evidence = ReplayVertexProjectionEventExact;
  EXPECT_THROW(builder.record(screen), std::invalid_argument)
      << "Screen2D may retain a GP0 address, never stale GTE identity";
  EXPECT_EQ(builder.mutationCount(), 2u);
}

TEST(GpuModernReplayTest, InvalidWUsesRawGteFallbackForWholePacket) {
  CrashReplayFrameBuilder builder;
  DrawPacket fallback = trianglePacket();
  fallback.validMask = 0x3;
  fallback.fallbackReason = PacketFallbackReason::InvalidWPacketAtomic;
  for (std::size_t i = 0; i < fallback.vertexCount; ++i)
    fallback.vertices[i].w = CRASH_REPLAY_FALLBACK_W;
  EXPECT_NO_THROW(builder.record(fallback));

  DrawPacket mixedW = trianglePacket(78);
  mixedW.validMask = 0x3;
  mixedW.fallbackReason = PacketFallbackReason::InvalidWPacketAtomic;
  mixedW.vertices[0].w = CRASH_REPLAY_FALLBACK_W;
  mixedW.vertices[1].w = 12000.0;
  mixedW.vertices[2].w = CRASH_REPLAY_FALLBACK_W;
  EXPECT_THROW(builder.record(mixedW), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 1u);
}

TEST(GpuModernReplayTest, Screen2DRouteRequiresSemanticAndDepthInvalidation) {
  CrashReplayFrameBuilder builder;
  DrawPacket screen = trianglePacket();
  screen.opcode = 0x2A;
  screen.route = PacketRoute::Screen2D;
  screen.semanticReason = PacketSemanticReason::DisplayAnchoredComposite;
  screen.validMask = 0;
  screen.vertexCount = 4;
  screen.indexCount = 6;
  screen.triangleIndices = {0, 1, 2, 1, 2, 3};
  screen.textured = false;
  screen.gouraud = false;
  screen.nativeRead = {};
  screen.state.textureUvDomain = {};
  screen.vertices[3].nativeX = 27;
  screen.vertices[3].nativeY = 44;
  screen.vertices[3].x = 27.0;
  screen.vertices[3].y = 44.0;
  screen.state.depth = DepthState{};
  screen.state.depth.epoch = 1;
  screen.state.depth.invalidateOnAccepted = true;
  for (std::size_t i = 0; i < screen.vertexCount; ++i)
    screen.vertices[i].w = CRASH_REPLAY_FALLBACK_W;
  EXPECT_NO_THROW(builder.record(screen));

  DrawPacket noInvalidate = screen;
  noInvalidate.packetSequence = 78;
  noInvalidate.state.depth.invalidateOnAccepted = false;
  EXPECT_THROW(builder.record(noInvalidate), std::invalid_argument);

  DrawPacket noSemantic = screen;
  noSemantic.packetSequence = 78;
  noSemantic.semanticReason = PacketSemanticReason::PolygonDefault;
  EXPECT_THROW(builder.record(noSemantic), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 1u);
}

TEST(GpuModernReplayTest, PacketSequenceAndFrameCompletenessAreValidated) {
  CrashReplayFrameBuilder builder;
  const DrawPacket first = trianglePacket(100);
  builder.record(first);

  DrawPacket repeated = trianglePacket(100);
  EXPECT_THROW(builder.record(repeated), std::invalid_argument);
  EXPECT_EQ(builder.mutationCount(), 1u);

  FrameLatch tooNarrow = packetLatch(101, 102);
  EXPECT_THROW(builder.seal(tooNarrow), std::invalid_argument);
  EXPECT_FALSE(builder.isSealed());

  FrameLatch contradiction = packetLatch(100, 101);
  contradiction.unsupportedCount = 1;
  contradiction.complete = true;
  EXPECT_THROW(builder.seal(contradiction), std::invalid_argument);

  contradiction.complete = false;
  const auto frame = builder.seal(contradiction);
  EXPECT_FALSE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 1u);
}

TEST(GpuModernReplayTest, ResetNeverDoublesAsSurfaceSeed) {
  CrashReplayFrameBuilder builder;
  Reset reset;
  reset.nextDepthEpoch = uint64_t{1} << 48;
  builder.record(reset);

  SurfaceSeed seed;
  seed.destination = NativeRect{0, 0, 2, 2};
  seed.pixels = {1, 2, 3, 4};
  seed.depthEpoch = uint64_t{1} << 49;
  builder.record(seed);

  const auto frame = builder.seal(FrameLatch{});
  EXPECT_TRUE(std::holds_alternative<Reset>(frame->operations()[0]));
  EXPECT_TRUE(std::holds_alternative<SurfaceSeed>(frame->operations()[1]));
  EXPECT_EQ(std::get<Reset>(frame->operations()[0]).nextDepthEpoch,
            uint64_t{1} << 48);
}

TEST(GpuModernReplayTest, PaintRectCoalescesPointCommandsWithoutHeapPayload) {
  CrashReplayFrameBuilder builder;
  PaintRect points;
  points.destination = NativeRect{40, 50, 8, 1};
  points.nativeColor = 0x1234;
  points.color = Rgb8{16, 32, 64};
  points.depth.invalidateOnAccepted = true;
  points.coalescedCommandCount = 8;
  builder.record(points);

  const auto frame = builder.seal(FrameLatch{});
  EXPECT_EQ(std::get<PaintRect>(frame->operations().front()), points);

  CrashReplayFrameBuilder invalidBuilder;
  points.depth.invalidateOnAccepted = false;
  EXPECT_THROW(invalidBuilder.record(points), std::invalid_argument);
}

TEST(GpuModernReplayTest, MailboxPreservesEveryFrameInProducerOrder) {
  CrashReplayMailbox mailbox;
  constexpr uint64_t frameCount = 256;

  std::thread producer([&] {
    for (uint64_t frame = 0; frame < frameCount; ++frame)
      mailbox.publish(makeFrame(frame));
    mailbox.close();
  });

  std::vector<uint64_t> consumed;
  consumed.reserve(frameCount);
  while (auto frame = mailbox.waitConsumeFor(std::chrono::seconds(2)))
    consumed.push_back(frame->latch().frameNumber);
  producer.join();

  ASSERT_EQ(consumed.size(), frameCount);
  for (uint64_t frame = 0; frame < frameCount; ++frame)
    EXPECT_EQ(consumed[frame], frame);
  EXPECT_TRUE(mailbox.isClosed());
  EXPECT_EQ(mailbox.pendingFrameCount(), 0u);
}

TEST(GpuModernReplayTest, ClosingMailboxDoesNotDiscardQueuedMutations) {
  CrashReplayMailbox mailbox;
  mailbox.publish(makeFrame(10));
  mailbox.publish(makeFrame(11));
  mailbox.close();

  EXPECT_EQ(mailbox.pendingFrameCount(), 2u);
  ASSERT_TRUE(mailbox.tryConsume());
  ASSERT_TRUE(mailbox.waitConsume());
  EXPECT_FALSE(mailbox.tryConsume());
  EXPECT_FALSE(mailbox.waitConsume());
  EXPECT_THROW(mailbox.publish(makeFrame(12)), std::logic_error);
}
