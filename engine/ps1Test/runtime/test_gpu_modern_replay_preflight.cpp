#include "runtime/gpu/modern_replay_preflight.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

constexpr NativeRect kAperture{512, 0, 512, 256};

ReplayPreflightState anchoredState(uint64_t revision = 1) {
  ReplayPreflightState state;
  state.timelineInitialized = true;
  state.surfaceInitialized = true;
  state.depthInitialized = true;
  state.depthEpoch = 1;
  state.lastFrameNumber = 9;
  state.contentRevision = 90;
  state.completedSubmissionId = 9;
  state.nextPacketSequence = 100;
  state.nativeTileRevisions.fill(revision);
  state.knownNativeTiles.set();
  return state;
}

FrameLatch latch(uint64_t frameNumber, uint64_t contentRevision,
                 uint64_t submission, uint64_t sequenceBegin,
                 uint64_t sequenceEnd) {
  FrameLatch value;
  value.frameNumber = frameNumber;
  value.contentRevision = contentRevision;
  value.completedSubmissionId = submission;
  value.sequenceBegin = sequenceBegin;
  value.sequenceEnd = sequenceEnd;
  value.display.aperture = kAperture;
  value.display.pal = true;
  return value;
}

DrawPacket projectedPacket(uint64_t sequence, uint64_t submission = 10,
                           uint64_t depthEpoch = 1) {
  DrawPacket packet;
  packet.opcode = 0x20;
  packet.packetSequence = sequence;
  packet.submissionId = submission;
  packet.validMask = 0x7;
  packet.vertexCount = 3;
  packet.indexCount = 3;
  packet.triangleIndices = {0, 1, 2, 0, 0, 0};
  packet.state.depth.epoch = depthEpoch;
  packet.vertices[0].nativeX = 0;
  packet.vertices[0].nativeY = 0;
  packet.vertices[0].x = 0.25;
  packet.vertices[0].y = 0.5;
  packet.vertices[0].preciseXY = true;
  packet.vertices[1].nativeX = 8;
  packet.vertices[1].nativeY = 0;
  packet.vertices[1].x = 8.0;
  packet.vertices[1].y = 0.0;
  packet.vertices[2].nativeX = 0;
  packet.vertices[2].nativeY = 8;
  packet.vertices[2].x = 0.0;
  packet.vertices[2].y = 8.0;
  return packet;
}

void setNativeReadProof(DrawPacket &packet, const ReplayPreflightState &state) {
  const std::vector<uint32_t> tiles = conservativeNativeReadTileIndices(packet);
  packet.nativeRead.required = true;
  packet.nativeRead.tileCount = static_cast<uint32_t>(tiles.size());
  packet.nativeRead.versionHash = CRASH_REPLAY_READ_HASH_OFFSET;
  for (const uint32_t tile : tiles) {
    packet.nativeRead.versionHash = appendNativeReadDependencyHash(
        packet.nativeRead.versionHash, tile, state.nativeTileRevisions[tile]);
  }
}

void setNativeReadProof(CopyRect &copy, const ReplayPreflightState &state) {
  const std::vector<uint32_t> tiles = conservativeNativeReadTileIndices(copy);
  copy.nativeRead.required = true;
  copy.nativeRead.tileCount = static_cast<uint32_t>(tiles.size());
  copy.nativeRead.versionHash = CRASH_REPLAY_READ_HASH_OFFSET;
  for (const uint32_t tile : tiles) {
    copy.nativeRead.versionHash = appendNativeReadDependencyHash(
        copy.nativeRead.versionHash, tile, state.nativeTileRevisions[tile]);
  }
}

DrawPacket texturedPacket(uint64_t sequence,
                          const ReplayPreflightState &readState,
                          uint64_t submission = 10) {
  DrawPacket packet = projectedPacket(sequence, submission);
  packet.state.depth.epoch = readState.depthEpoch;
  packet.opcode = 0x24;
  packet.textured = true;
  packet.state.textureUvDomain.enabled = true;
  packet.vertices[0].u = 1.0;
  packet.vertices[0].v = 2.0;
  packet.vertices[1].u = 20.0;
  packet.vertices[1].v = 2.0;
  packet.vertices[2].u = 1.0;
  packet.vertices[2].v = 20.0;
  setNativeReadProof(packet, readState);
  return packet;
}

DrawPacket screenPacket(uint64_t sequence) {
  DrawPacket packet = projectedPacket(sequence);
  packet.route = PacketRoute::Screen2D;
  packet.semanticReason = PacketSemanticReason::Intrinsic2DFamily;
  packet.validMask = 0;
  packet.state.depth.invalidateOnAccepted = true;
  return packet;
}

DrawPacket textureDisabledPacket(uint64_t sequence) {
  DrawPacket packet = projectedPacket(sequence);
  packet.opcode = 0x24;
  packet.textured = true;
  packet.state.textureUvDomain.enabled = true;
  packet.state.texturePage.raw = 1u << 11u;
  packet.state.texturePage.textureDisable = true;
  return packet;
}

DrawPacket effectiveUntexturedTextureDisabledPacket(uint64_t sequence) {
  DrawPacket packet = projectedPacket(sequence);
  packet.opcode = 0x24; // Encoded textured layout remains provenance.
  packet.textured = false;
  packet.rawTexture = false;
  packet.state.texturePage.raw = 1u << 11u;
  packet.state.texturePage.textureDisable = true;
  packet.state.textureUvDomain.enabled = false;
  return packet;
}

CpuUpload cpuUpload(uint64_t transactionId, CpuUploadPhase phase,
                    NativeRect transfer, uint64_t pixelOffset = 0,
                    std::vector<uint16_t> pixels = {}, MaskState mask = {}) {
  CpuUpload upload;
  upload.transactionId = transactionId;
  upload.phase = phase;
  upload.transfer = transfer;
  upload.pixelOffset = pixelOffset;
  upload.pixels = std::move(pixels);
  if (!upload.pixels.empty()) {
    upload.acceptedBits.resize(
        CpuUpload::acceptedWordCount(upload.pixels.size()), UINT64_MAX);
    const std::size_t tail = upload.pixels.size() % 64u;
    if (tail != 0)
      upload.acceptedBits.back() = (uint64_t{1} << tail) - 1u;
  }
  upload.mask = mask;
  return upload;
}

CrashReplayFramePtr makeFrame(FrameLatch frameLatch,
                              std::vector<Operation> operations = {}) {
  CrashReplayFrameBuilder builder;
  for (Operation &operation : operations) {
    std::visit(
        [&builder]<typename T>(T &&value) {
          using Value = std::remove_cvref_t<T>;
          if constexpr (!std::is_same_v<Value, FrameLatch>)
            builder.record(std::forward<T>(value));
        },
        std::move(operation));
  }
  return builder.seal(std::move(frameLatch));
}

ReplayPreflightRequest requestFor(const FrameLatch &expected) {
  ReplayPreflightRequest request;
  request.expectedLatch = expected;
  request.expectedPolicy = expected.policy;
  request.capabilities = CRASH_REPLAY_V1_ALL_CAPABILITIES;
  return request;
}

ReplayPreflightResult
runOne(const CrashReplayFramePtr &frame, const FrameLatch &expected,
       const ReplayPreflightState &state,
       ReplayCapabilityMask capabilities = CRASH_REPLAY_V1_ALL_CAPABILITIES,
       ReplayNearClipMode nearClipMode = ReplayNearClipMode::Saturate) {
  const std::vector<CrashReplayFramePtr> fifo{frame};
  ReplayPreflightRequest request = requestFor(expected);
  request.capabilities = capabilities;
  request.nearClipMode = nearClipMode;
  return preflightCrashReplay(fifo, request, state);
}

ReplayNativeSnapshotIdentity snapshotFor(const FrameLatch &frameLatch,
                                         CrashReplayFramePtr sealedFrame = {}) {
  ReplayNativeSnapshotIdentity snapshot;
  snapshot.frameNumber = frameLatch.frameNumber;
  snapshot.contentRevision = frameLatch.contentRevision;
  snapshot.completedSubmissionId = frameLatch.completedSubmissionId;
  snapshot.display = frameLatch.display;
  snapshot.sealedFrame = std::move(sealedFrame);
  return snapshot;
}

ReplayNativeAnchorRequest
anchorRequestFor(const FrameLatch &frameLatch,
                 CrashReplayFramePtr sealedFrame = {}) {
  ReplayNativeAnchorRequest request;
  request.snapshot = snapshotFor(frameLatch, std::move(sealedFrame));
  request.expectedPolicy = frameLatch.policy;
  return request;
}

ReplayNativeAnchorResult anchorOne(const CrashReplayFramePtr &frame,
                                   const FrameLatch &snapshotLatch,
                                   const ReplayPreflightState &state) {
  const std::array<CrashReplayFramePtr, 1> fifo{frame};
  return deriveNativeSnapshotAnchor(
      fifo, anchorRequestFor(snapshotLatch, frame), state);
}

} // namespace

static_assert(CRASH_REPLAY_NATIVE_TILE_COUNT == 8192);

TEST(GpuModernReplayPreflightTest,
     RetainsExactFifoPrefixAndReturnsCandidateOnlyForM1) {
  const ReplayPreflightState confirmed = anchoredState();
  ReplayPreflightState afterUpload = confirmed;
  afterUpload.nativeTileRevisions[0] = 2;

  NativeTileUpload upload;
  upload.destination = NativeRect{0, 0, 8, 8};
  upload.pixels.resize(64, 0x1234);
  upload.nativeRevision = 2;
  const FrameLatch firstLatch = latch(10, 100, 10, 100, 105);
  const auto first =
      makeFrame(firstLatch, {Operation{upload},
                             Operation{texturedPacket(103, afterUpload)}});

  FillRect fill;
  fill.destination = NativeRect{20, 30, 4, 5};
  const FrameLatch targetLatch = latch(11, 101, 11, 105, 108);
  const auto target = makeFrame(targetLatch, {Operation{fill}});
  const auto later = makeFrame(latch(12, 102, 12, 108, 109));
  const std::vector<CrashReplayFramePtr> fifo{first, target, later};

  const ReplayPreflightResult result =
      preflightCrashReplay(fifo, requestFor(targetLatch), confirmed);

  EXPECT_EQ(result.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(result.reason, ReplayPreflightReason::None);
  ASSERT_EQ(result.frames.size(), 2u);
  EXPECT_EQ(result.frames[0], first);
  EXPECT_EQ(result.frames[1], target);
  EXPECT_EQ(fifo.size(), 3u);
  EXPECT_EQ(result.candidateState.lastFrameNumber, 11u);
  EXPECT_EQ(result.candidateState.nextPacketSequence, 108u);
  EXPECT_EQ(result.candidateState.nativeTileRevisions[0], 2u);
  EXPECT_EQ(confirmed.nativeTileRevisions[0], 1u);
  EXPECT_EQ(result.counters.inputFrames, 3u);
  EXPECT_EQ(result.counters.retainedFrames, 2u);
  EXPECT_EQ(result.counters.examinedFrames, 2u);
  EXPECT_EQ(result.counters.operations, 5u);
  EXPECT_EQ(result.counters.mutations, 3u);
  EXPECT_EQ(result.counters.drawPackets, 1u);
  EXPECT_EQ(result.counters.nativeTileUploads, 1u);
  EXPECT_GT(result.counters.nativeReadTiles, 0u);
}

TEST(GpuModernReplayPreflightTest,
     DoesNotRetainFirstFrameNewerThanUnavailableTarget) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const auto future = makeFrame(latch(11, 101, 11, 101, 102));
  const std::vector<CrashReplayFramePtr> fifo{future};

  const ReplayPreflightResult result =
      preflightCrashReplay(fifo, requestFor(expected), confirmed);

  EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(result.reason, ReplayPreflightReason::TargetLatchUnavailable);
  EXPECT_TRUE(result.frames.empty());
  EXPECT_EQ(result.counters.inputFrames, 1u);
  EXPECT_EQ(result.counters.retainedFrames, 0u);
  EXPECT_EQ(result.counters.examinedFrames, 0u);
  EXPECT_EQ(result.candidateState, confirmed);
}

TEST(GpuModernReplayPreflightTest,
     CapturePolicyMustMatchConsumerConfigurationIndependently) {
  const ReplayPreflightState confirmed = anchoredState();
  FrameLatch actual = latch(10, 100, 10, 100, 101);
  actual.policy = ReplayPolicyState{4, true, true, ReplayDepthMode::Hybrid};
  const auto frame = makeFrame(actual);
  const std::array<CrashReplayFramePtr, 1> fifo{frame};

  ReplayPreflightRequest matching = requestFor(actual);
  EXPECT_EQ(preflightCrashReplay(fifo, matching, confirmed).route,
            ReplayPreflightRoute::M1Replay);

  for (const ReplayPolicyState mismatch : {
           ReplayPolicyState{1, true, true, ReplayDepthMode::Hybrid},
           ReplayPolicyState{4, false, true, ReplayDepthMode::Hybrid},
           ReplayPolicyState{4, true, false, ReplayDepthMode::Hybrid},
           ReplayPolicyState{4, true, true, ReplayDepthMode::Strict},
       }) {
    SCOPED_TRACE(mismatch.internalScale);
    ReplayPreflightRequest request = matching;
    request.expectedPolicy = mismatch;
    const ReplayPreflightResult result =
        preflightCrashReplay(fifo, request, confirmed);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason, ReplayPreflightReason::PolicyMismatch);
    EXPECT_EQ(result.candidateState, confirmed);
  }

  ReplayPreflightRequest invalid = matching;
  invalid.expectedPolicy.internalScale = 0;
  EXPECT_EQ(preflightCrashReplay(fifo, invalid, confirmed).reason,
            ReplayPreflightReason::InvalidExpectedPolicy);
}

TEST(GpuModernReplayPreflightTest,
     EveryOperationVariantIsGatedBeforeExecution) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  NativeTileUpload tile;
  tile.destination = NativeRect{0, 0, 8, 8};
  tile.pixels.resize(64);
  tile.nativeRevision = 2;

  SurfaceSeed seed;
  seed.destination = NativeRect{0, 0, 1, 1};
  seed.pixels = {0};

  FillRect fill;
  fill.destination = NativeRect{0, 0, 1, 1};
  CopyRect copy;
  copy.width = 1;
  copy.height = 1;
  setNativeReadProof(copy, confirmed);
  CpuUpload cpuUpload;
  cpuUpload.transactionId = 100;
  cpuUpload.phase = CpuUploadPhase::Begin;
  cpuUpload.transfer = NativeRect{0, 0, 1, 1};
  PaintPixels pixels;
  pixels.pixels.push_back(PaintedPixel{});
  pixels.depth.epoch = 1;
  pixels.depth.invalidateOnAccepted = true;
  PaintRect paintRect;
  paintRect.destination = NativeRect{0, 0, 1, 1};
  paintRect.depth.epoch = 1;
  paintRect.depth.invalidateOnAccepted = true;
  ClearDepth clearDepth;
  Reset reset;

  std::vector<std::pair<ReplayCapability, Operation>> cases;
  cases.emplace_back(ReplayCapability::NativeTileUpload, std::move(tile));
  cases.emplace_back(ReplayCapability::SurfaceSeed, std::move(seed));
  cases.emplace_back(ReplayCapability::DrawPacket, projectedPacket(100));
  cases.emplace_back(ReplayCapability::FillRect, fill);
  cases.emplace_back(ReplayCapability::CopyRect, copy);
  cases.emplace_back(ReplayCapability::CpuUpload, std::move(cpuUpload));
  cases.emplace_back(ReplayCapability::PaintPixels, std::move(pixels));
  cases.emplace_back(ReplayCapability::PaintRect, paintRect);
  cases.emplace_back(ReplayCapability::ClearDepth, clearDepth);
  cases.emplace_back(ReplayCapability::Reset, reset);

  for (auto &[capability, operation] : cases) {
    SCOPED_TRACE(std::string(replayCapabilityName(capability)));
    std::vector<Operation> oneOperation;
    oneOperation.push_back(std::move(operation));
    const auto frame = makeFrame(expected, std::move(oneOperation));
    const ReplayCapabilityMask capabilities =
        CRASH_REPLAY_V1_ALL_CAPABILITIES & ~replayCapabilityBit(capability);
    const ReplayPreflightResult result =
        runOne(frame, expected, confirmed, capabilities);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason, ReplayPreflightReason::MissingCapability);
    EXPECT_EQ(result.missingCapabilities, replayCapabilityBit(capability));
    EXPECT_EQ(result.candidateState, confirmed);
    ASSERT_EQ(result.frames.size(), 1u);
    EXPECT_EQ(result.frames.front(), frame);
  }

  const auto emptyFrame = makeFrame(expected);
  const ReplayCapabilityMask noLatch =
      CRASH_REPLAY_V1_ALL_CAPABILITIES &
      ~replayCapabilityBit(ReplayCapability::FrameLatch);
  const ReplayPreflightResult latchResult =
      runOne(emptyFrame, expected, confirmed, noLatch);
  EXPECT_EQ(latchResult.reason, ReplayPreflightReason::MissingCapability);
  EXPECT_EQ(latchResult.missingCapabilities,
            replayCapabilityBit(ReplayCapability::FrameLatch));
}

TEST(GpuModernReplayPreflightTest,
     DrawPacketComposableCapabilityMatrixReportsExactMissingBit) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  DrawPacket opaque = projectedPacket(100);
  DrawPacket semiTransparent = opaque;
  semiTransparent.opcode = 0x22;
  semiTransparent.state.blend.enabled = true;
  DrawPacket textured = texturedPacket(100, confirmed);
  DrawPacket destinationMaskRead = opaque;
  destinationMaskRead.state.mask.checkMaskBit = true;
  DrawPacket forceMaskWrite = opaque;
  forceMaskWrite.state.mask.forceMaskBit = true;
  DrawPacket depthTestWrite = opaque;
  depthTestWrite.state.depth.testEnabled = true;
  depthTestWrite.state.depth.writeEnabled = true;
  depthTestWrite.state.depth.compare = DepthCompare::LessOrEqual;
  DrawPacket depthPainterWrite = opaque;
  depthPainterWrite.state.depth.testEnabled = false;
  depthPainterWrite.state.depth.writeEnabled = true;
  depthPainterWrite.state.depth.compare = DepthCompare::Always;
  DrawPacket depthInvalidate = opaque;
  depthInvalidate.state.depth.invalidateOnAccepted = true;
  DrawPacket depthNonCanonical = opaque;
  depthNonCanonical.state.depth.testEnabled = true;
  depthNonCanonical.state.depth.writeEnabled = false;
  depthNonCanonical.state.depth.compare = DepthCompare::LessOrEqual;

  struct Case {
    ReplayCapability missing;
    DrawPacket packet;
  };
  const std::vector<Case> cases{
      {ReplayCapability::DrawOpaque, opaque},
      {ReplayCapability::DrawSemiTransparent, semiTransparent},
      {ReplayCapability::DrawUntextured, opaque},
      {ReplayCapability::DrawTextured, textured},
      {ReplayCapability::DestinationColorFeedback, semiTransparent},
      {ReplayCapability::DestinationMaskRead, destinationMaskRead},
      {ReplayCapability::ForceMaskWrite, forceMaskWrite},
      {ReplayCapability::DepthTestWriteLessEqual, depthTestWrite},
      {ReplayCapability::DepthPainterWrite, depthPainterWrite},
      {ReplayCapability::DepthPainterOrdered, opaque},
      {ReplayCapability::DepthInvalidate, depthInvalidate},
      {ReplayCapability::DepthNonCanonical, depthNonCanonical},
  };

  for (const Case &testCase : cases) {
    SCOPED_TRACE(std::string(replayCapabilityName(testCase.missing)));
    const auto frame = makeFrame(expected, {Operation{testCase.packet}});
    const ReplayCapabilityMask capabilities =
        CRASH_REPLAY_V1_ALL_CAPABILITIES &
        ~replayCapabilityBit(testCase.missing);
    const ReplayPreflightResult result =
        runOne(frame, expected, confirmed, capabilities);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason, ReplayPreflightReason::MissingCapability);
    EXPECT_EQ(result.missingCapabilities,
              replayCapabilityBit(testCase.missing));
    EXPECT_EQ(result.candidateState, confirmed);
  }
}

TEST(GpuModernReplayPreflightTest,
     MinimalOpaqueUntexturedDepthProfileRejectsEveryOtherDrawClass) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const ReplayCapabilityMask profile =
      replayCapabilityBit(ReplayCapability::DrawPacket) |
      replayCapabilityBit(ReplayCapability::FrameLatch) |
      replayCapabilityBit(ReplayCapability::Projected3D) |
      replayCapabilityBit(ReplayCapability::NearPlaneSaturate) |
      replayCapabilityBit(ReplayCapability::DrawOpaque) |
      replayCapabilityBit(ReplayCapability::DrawUntextured) |
      replayCapabilityBit(ReplayCapability::ForceMaskWrite) |
      replayCapabilityBit(ReplayCapability::DepthTestWriteLessEqual);

  DrawPacket acceptedPacket = projectedPacket(100);
  acceptedPacket.state.mask.forceMaskBit = true;
  acceptedPacket.state.depth.testEnabled = true;
  acceptedPacket.state.depth.writeEnabled = true;
  acceptedPacket.state.depth.compare = DepthCompare::LessOrEqual;
  const auto acceptedFrame = makeFrame(expected, {Operation{acceptedPacket}});
  const ReplayPreflightResult accepted =
      runOne(acceptedFrame, expected, confirmed, profile);
  ASSERT_EQ(accepted.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(accepted.reason, ReplayPreflightReason::None);

  struct RejectedCase {
    const char *name;
    DrawPacket packet;
    ReplayCapability firstMissing;
  };
  std::vector<RejectedCase> rejected;

  rejected.push_back({"textured", texturedPacket(100, confirmed),
                      ReplayCapability::DrawTextured});

  DrawPacket blended = acceptedPacket;
  blended.opcode = 0x22;
  blended.state.blend.enabled = true;
  rejected.push_back(
      {"blended", blended, ReplayCapability::DrawSemiTransparent});

  DrawPacket maskChecked = acceptedPacket;
  maskChecked.state.mask.checkMaskBit = true;
  rejected.push_back(
      {"mask_checked", maskChecked, ReplayCapability::DestinationMaskRead});

  rejected.push_back(
      {"screen_2d", screenPacket(100), ReplayCapability::Screen2D});

  DrawPacket painterOrdered = acceptedPacket;
  painterOrdered.state.depth.testEnabled = false;
  painterOrdered.state.depth.writeEnabled = false;
  painterOrdered.state.depth.compare = DepthCompare::Always;
  rejected.push_back({"painter_ordered", painterOrdered,
                      ReplayCapability::DepthPainterOrdered});

  DrawPacket invalidating = painterOrdered;
  invalidating.state.depth.invalidateOnAccepted = true;
  rejected.push_back(
      {"depth_invalidate", invalidating, ReplayCapability::DepthInvalidate});

  for (const RejectedCase &testCase : rejected) {
    SCOPED_TRACE(testCase.name);
    const auto frame = makeFrame(expected, {Operation{testCase.packet}});
    const ReplayPreflightResult result =
        runOne(frame, expected, confirmed, profile);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason, ReplayPreflightReason::MissingCapability);
    EXPECT_EQ(result.missingCapabilities,
              replayCapabilityBit(testCase.firstMissing));
    EXPECT_EQ(result.candidateState, confirmed);
  }
}

TEST(GpuModernReplayPreflightTest,
     SequenceRangesAreHalfOpenAndDrawPacketsMayHaveGp0Gaps) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch firstLatch = latch(10, 100, 10, 100, 110);
  const auto first = makeFrame(firstLatch, {Operation{projectedPacket(101)},
                                            Operation{projectedPacket(108)}});
  const FrameLatch secondLatch = latch(11, 101, 11, 110, 113);
  const auto second = makeFrame(
      secondLatch, {Operation{FillRect{NativeRect{0, 0, 1, 1}, Rgb8{}, 0}}});
  const std::vector<CrashReplayFramePtr> fifo{first, second};

  const ReplayPreflightResult accepted =
      preflightCrashReplay(fifo, requestFor(secondLatch), confirmed);
  EXPECT_EQ(accepted.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(accepted.candidateState.nextPacketSequence, 113u);

  const FrameLatch discontinuous = latch(11, 101, 11, 111, 113);
  const auto wrongSecond = makeFrame(discontinuous);
  const std::vector<CrashReplayFramePtr> wrongFifo{first, wrongSecond};
  const ReplayPreflightResult rejected =
      preflightCrashReplay(wrongFifo, requestFor(discontinuous), confirmed);
  EXPECT_EQ(rejected.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(rejected.reason, ReplayPreflightReason::SequenceRangeDiscontinuity);
}

TEST(GpuModernReplayPreflightTest,
     CpuUploadTransactionPersistsAtomicallyAcrossFrameLatches) {
  const ReplayPreflightState confirmed = anchoredState();
  constexpr NativeRect transfer{30, 40, 4, 1};
  constexpr MaskState mask{true, false};

  const FrameLatch firstLatch = latch(10, 100, 10, 100, 101);
  const auto first = makeFrame(
      firstLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer, 0, {}, mask)},
       Operation{
           cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1, 2}, mask)}});
  const ReplayPreflightResult partial = runOne(first, firstLatch, confirmed);
  ASSERT_EQ(partial.route, ReplayPreflightRoute::M1Replay);
  EXPECT_TRUE(partial.candidateState.cpuUpload.active);
  EXPECT_EQ(partial.candidateState.cpuUpload.transactionId, 100u);
  EXPECT_EQ(partial.candidateState.cpuUpload.transfer, transfer);
  EXPECT_EQ(partial.candidateState.cpuUpload.mask, mask);
  EXPECT_EQ(partial.candidateState.cpuUpload.nextOffset, 2u);

  // The transaction id belongs to the A0 header in the prior frame and is
  // deliberately outside this second latch's empty sequence range.
  const FrameLatch secondLatch = latch(11, 101, 11, 101, 101);
  const auto second =
      makeFrame(secondLatch, {Operation{cpuUpload(100, CpuUploadPhase::Data,
                                                  transfer, 2, {3, 4}, mask)},
                              Operation{cpuUpload(100, CpuUploadPhase::Complete,
                                                  transfer, 4, {}, mask)}});
  const std::vector<CrashReplayFramePtr> fifo{first, second};
  const ReplayPreflightResult complete =
      preflightCrashReplay(fifo, requestFor(secondLatch), confirmed);
  ASSERT_EQ(complete.route, ReplayPreflightRoute::M1Replay);
  EXPECT_FALSE(complete.candidateState.cpuUpload.active);
  EXPECT_EQ(complete.candidateState.nextPacketSequence, 101u);

  const auto aborted = makeFrame(
      firstLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer, 0, {}, mask)},
       Operation{
           cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1, 2}, mask)},
       Operation{
           cpuUpload(100, CpuUploadPhase::Abort, transfer, 2, {}, mask)}});
  const ReplayPreflightResult abortResult =
      runOne(aborted, firstLatch, confirmed);
  ASSERT_EQ(abortResult.route, ReplayPreflightRoute::M1Replay);
  EXPECT_FALSE(abortResult.candidateState.cpuUpload.active);
}

TEST(GpuModernReplayPreflightTest,
     CpuUploadHeaderSequenceCannotRegressOrCollideWithDrawPacket) {
  const ReplayPreflightState confirmed = anchoredState();
  constexpr NativeRect transfer{30, 40, 1, 1};
  const FrameLatch expected = latch(10, 100, 10, 100, 102);

  for (const uint64_t transactionId : {100u, 101u}) {
    SCOPED_TRACE(transactionId);
    const auto frame = makeFrame(
        expected,
        {Operation{projectedPacket(101)},
         Operation{cpuUpload(transactionId, CpuUploadPhase::Begin, transfer)},
         Operation{cpuUpload(transactionId, CpuUploadPhase::Abort, transfer)}});
    const ReplayPreflightResult result = runOne(frame, expected, confirmed);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason,
              ReplayPreflightReason::PacketSequenceDiscontinuity);
    EXPECT_EQ(result.candidateState, confirmed);
  }

  const auto ordered = makeFrame(
      expected, {Operation{projectedPacket(100)},
                 Operation{cpuUpload(101, CpuUploadPhase::Begin, transfer)},
                 Operation{cpuUpload(101, CpuUploadPhase::Abort, transfer)}});
  EXPECT_EQ(runOne(ordered, expected, confirmed).route,
            ReplayPreflightRoute::M1Replay);
}

TEST(GpuModernReplayPreflightTest,
     ActiveCpuUploadCannotAdvanceLatchSequenceUntilTerminal) {
  const ReplayPreflightState confirmed = anchoredState();
  constexpr NativeRect transfer{30, 40, 4, 1};
  const FrameLatch firstLatch = latch(10, 100, 10, 100, 101);
  const auto first = makeFrame(
      firstLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
       Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1})}});

  const FrameLatch impossibleLatch = latch(11, 101, 11, 101, 102);
  const auto impossible = makeFrame(
      impossibleLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 1, {2})}});
  const std::vector<CrashReplayFramePtr> fifo{first, impossible};
  const ReplayPreflightResult result =
      preflightCrashReplay(fifo, requestFor(impossibleLatch), confirmed);
  EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(result.reason, ReplayPreflightReason::PacketSequenceDiscontinuity);
  EXPECT_EQ(result.candidateState, confirmed);

  const FrameLatch validLatch = latch(11, 101, 11, 101, 101);
  const auto valid = makeFrame(
      validLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 1, {2})}});
  const std::vector<CrashReplayFramePtr> validFifo{first, valid};
  EXPECT_EQ(
      preflightCrashReplay(validFifo, requestFor(validLatch), confirmed).route,
      ReplayPreflightRoute::M1Replay);
}

TEST(GpuModernReplayPreflightTest,
     CpuUploadRejectsMissingDuplicateGapIdentityMetadataAndInterleave) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  constexpr NativeRect transfer{30, 40, 4, 1};

  struct Case {
    ReplayPreflightReason reason;
    std::vector<Operation> operations;
  };
  std::vector<Case> cases;
  cases.push_back(
      {ReplayPreflightReason::CpuUploadMissingBegin,
       {Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1})}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadMissingBegin,
       {Operation{cpuUpload(100, CpuUploadPhase::Complete, transfer, 4)}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadBeginOutOfRange,
       {Operation{cpuUpload(101, CpuUploadPhase::Begin, transfer)}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadDuplicateBegin,
       {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
        Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadOffsetDiscontinuity,
       {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
        Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 1, {1})}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadOffsetDiscontinuity,
       {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
        Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1})},
        Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {2})}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadTransactionMismatch,
       {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
        Operation{cpuUpload(101, CpuUploadPhase::Data, transfer, 0, {1})}}});
  cases.push_back({ReplayPreflightReason::CpuUploadMetadataMismatch,
                   {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
                    Operation{cpuUpload(100, CpuUploadPhase::Data,
                                        NativeRect{31, 40, 4, 1}, 0, {1})}}});
  cases.push_back({ReplayPreflightReason::CpuUploadMetadataMismatch,
                   {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
                    Operation{cpuUpload(100, CpuUploadPhase::Abort, transfer, 0,
                                        {}, MaskState{true, false})}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadOffsetDiscontinuity,
       {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
        Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1, 2})},
        Operation{cpuUpload(100, CpuUploadPhase::Complete, transfer, 4)}}});
  cases.push_back(
      {ReplayPreflightReason::CpuUploadOffsetDiscontinuity,
       {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
        Operation{cpuUpload(100, CpuUploadPhase::Abort, transfer, 1)}}});

  FillRect fill;
  fill.destination = NativeRect{0, 0, 1, 1};
  cases.push_back({ReplayPreflightReason::CpuUploadInterleavedOperation,
                   {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
                    Operation{fill}}});
  Reset reset;
  reset.nextDepthEpoch = 2;
  cases.push_back({ReplayPreflightReason::CpuUploadInterleavedOperation,
                   {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
                    Operation{reset}}});
  SurfaceSeed seed;
  seed.destination = NativeRect{0, 0, 1, 1};
  seed.pixels = {0};
  cases.push_back({ReplayPreflightReason::CpuUploadInterleavedOperation,
                   {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
                    Operation{std::move(seed)}}});

  for (Case &testCase : cases) {
    SCOPED_TRACE(std::string(replayPreflightReasonName(testCase.reason)));
    const auto frame = makeFrame(expected, std::move(testCase.operations));
    const ReplayPreflightResult result = runOne(frame, expected, confirmed);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason, testCase.reason);
    EXPECT_EQ(result.candidateState, confirmed);
  }
}

TEST(GpuModernReplayPreflightTest,
     RouteFallbackNearClipAndTextureDisableHaveIndependentGates) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  struct Case {
    DrawPacket packet;
    ReplayCapability missing;
    ReplayNearClipMode nearClip = ReplayNearClipMode::Saturate;
  };
  DrawPacket fallback = projectedPacket(100);
  fallback.fallbackReason = PacketFallbackReason::RasterSetupRejected;
  std::vector<Case> cases{
      {screenPacket(100), ReplayCapability::Screen2D},
      {fallback, ReplayCapability::FallbackRasterSetup},
      {textureDisabledPacket(100), ReplayCapability::TextureDisable},
  };

  for (const Case &testCase : cases) {
    SCOPED_TRACE(std::string(replayCapabilityName(testCase.missing)));
    const auto frame = makeFrame(expected, {Operation{testCase.packet}});
    const ReplayCapabilityMask capabilities =
        CRASH_REPLAY_V1_ALL_CAPABILITIES &
        ~replayCapabilityBit(testCase.missing);
    const ReplayPreflightResult result =
        runOne(frame, expected, confirmed, capabilities, testCase.nearClip);
    EXPECT_EQ(result.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(result.reason, ReplayPreflightReason::MissingCapability);
    EXPECT_EQ(result.missingCapabilities,
              replayCapabilityBit(testCase.missing));
  }

  ReplayPreflightRequest geometricRequest = requestFor(expected);
  geometricRequest.nearClipMode = ReplayNearClipMode::GeometricClip;
  geometricRequest.capabilities |=
      replayCapabilityBit(ReplayCapability::NearPlaneGeometricClip);
  const auto geometricFrame =
      makeFrame(expected, {Operation{projectedPacket(100)}});
  const std::array<CrashReplayFramePtr, 1> geometricBatch{geometricFrame};
  const ReplayPreflightResult geometric =
      preflightCrashReplay(geometricBatch, geometricRequest, confirmed);
  EXPECT_EQ(geometric.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(geometric.reason, ReplayPreflightReason::InvalidNearClipMode)
      << "schema 2 cannot advertise geometric clipping without homogeneous "
         "numerators";

  const std::vector<std::pair<TextureDepth, ReplayCapability>> depths{
      {TextureDepth::Indexed4, ReplayCapability::TextureIndexed4},
      {TextureDepth::Indexed8, ReplayCapability::TextureIndexed8},
      {TextureDepth::Direct15, ReplayCapability::TextureDirect15},
      {TextureDepth::Reserved, ReplayCapability::TextureReserved},
  };
  for (std::size_t index = 0; index < depths.size(); ++index) {
    const auto [depth, capability] = depths[index];
    SCOPED_TRACE(std::string(replayCapabilityName(capability)));
    DrawPacket packet = texturedPacket(100, confirmed);
    packet.state.texturePage.depth = depth;
    packet.state.texturePage.raw = static_cast<uint16_t>(index << 7u);
    setNativeReadProof(packet, confirmed);
    const auto frame = makeFrame(expected, {Operation{packet}});
    const ReplayCapabilityMask capabilities =
        CRASH_REPLAY_V1_ALL_CAPABILITIES & ~replayCapabilityBit(capability);
    const ReplayPreflightResult result =
        runOne(frame, expected, confirmed, capabilities);
    EXPECT_EQ(result.reason, ReplayPreflightReason::MissingCapability);
    EXPECT_EQ(result.missingCapabilities, replayCapabilityBit(capability));
  }
}

TEST(GpuModernReplayPreflightTest,
     EffectiveUntexturedTextureDisableNeedsNoTextureCapabilitiesOrReadProof) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const auto frame = makeFrame(
      expected,
      {Operation{effectiveUntexturedTextureDisabledPacket(100)}});
  const ReplayCapabilityMask capabilities =
      CRASH_REPLAY_V1_ALL_CAPABILITIES &
      ~replayCapabilityBit(ReplayCapability::DrawTextured) &
      ~replayCapabilityBit(ReplayCapability::TextureDisable) &
      ~replayCapabilityBit(ReplayCapability::NativeTileUpload);

  const ReplayPreflightResult result =
      runOne(frame, expected, confirmed, capabilities);
  EXPECT_EQ(result.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(result.reason, ReplayPreflightReason::None);
  EXPECT_EQ(result.missingCapabilities, 0u);
  EXPECT_EQ(result.counters.nativeReadPackets, 0u);
  EXPECT_EQ(result.counters.nativeReadTiles, 0u);
}

TEST(GpuModernReplayPreflightTest,
     NativeUploadRevisionAndReadProofUsePrePacketShadowAtomically) {
  ReplayPreflightState confirmed = anchoredState(5);
  NativeTileUpload upload;
  upload.destination = NativeRect{0, 0, 8, 8};
  upload.pixels.resize(64);

  upload.nativeRevision = 4;
  const FrameLatch uploadLatch = latch(10, 100, 10, 100, 101);
  const auto regressing = makeFrame(uploadLatch, {Operation{upload}});
  const ReplayPreflightResult regression =
      runOne(regressing, uploadLatch, confirmed);
  EXPECT_EQ(regression.reason,
            ReplayPreflightReason::NativeRevisionNotAdvanced);
  EXPECT_EQ(regression.candidateState, confirmed);

  upload.nativeRevision = 5;
  const auto repeated = makeFrame(uploadLatch, {Operation{upload}});
  const ReplayPreflightResult repeatedResult =
      runOne(repeated, uploadLatch, confirmed);
  EXPECT_EQ(repeatedResult.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(repeatedResult.reason,
            ReplayPreflightReason::NativeRevisionNotAdvanced)
      << "equal revision numbers cannot prove that a second payload is "
         "idempotent";

  upload.nativeRevision = 6;
  ReplayPreflightState afterUpload = confirmed;
  afterUpload.nativeTileRevisions[0] = 6;
  DrawPacket packet = texturedPacket(103, afterUpload);
  const FrameLatch drawLatch = latch(10, 100, 10, 100, 105);
  const auto correct =
      makeFrame(drawLatch, {Operation{upload}, Operation{packet}});
  const ReplayPreflightResult accepted = runOne(correct, drawLatch, confirmed);
  EXPECT_EQ(accepted.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(accepted.candidateState.nativeTileRevisions[0], 6u);

  packet.nativeRead.versionHash ^= UINT64_C(0x100);
  const auto badHash =
      makeFrame(drawLatch, {Operation{upload}, Operation{packet}});
  const ReplayPreflightResult rejected = runOne(badHash, drawLatch, confirmed);
  EXPECT_EQ(rejected.reason, ReplayPreflightReason::NativeReadHashMismatch);
  EXPECT_EQ(rejected.candidateState, confirmed)
      << "a failed proof must not expose the locally applied tile upload";
  ASSERT_EQ(rejected.frames.size(), 1u);
  EXPECT_EQ(rejected.frames.front(), badHash);
}

TEST(GpuModernReplayPreflightTest,
     NativeReadProofRejectsWrongCountAndUnknownTiles) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  DrawPacket wrongCount = texturedPacket(100, confirmed);
  --wrongCount.nativeRead.tileCount;
  const auto countFrame = makeFrame(expected, {Operation{wrongCount}});
  EXPECT_EQ(runOne(countFrame, expected, confirmed).reason,
            ReplayPreflightReason::NativeReadCountMismatch);

  ReplayPreflightState unknown = confirmed;
  const std::vector<uint32_t> readTiles =
      conservativeNativeReadTileIndices(wrongCount);
  ASSERT_FALSE(readTiles.empty());
  unknown.knownNativeTiles.reset(readTiles.front());
  DrawPacket unknownRead = texturedPacket(100, unknown);
  const auto unknownFrame = makeFrame(expected, {Operation{unknownRead}});
  EXPECT_EQ(runOne(unknownFrame, expected, unknown).reason,
            ReplayPreflightReason::NativeReadTileUnknown);
}

TEST(GpuModernReplayPreflightTest,
     CopySourceProofCoversWrappedTilesAndRejectsStaleState) {
  const ReplayPreflightState confirmed = anchoredState();
  ReplayPreflightState afterUploads = confirmed;

  CopyRect copy;
  copy.source = NativePoint{1020, 508};
  copy.destination = NativePoint{100, 100};
  copy.width = 8;
  copy.height = 8;
  const std::vector<uint32_t> sourceTiles =
      conservativeNativeReadTileIndices(copy);
  EXPECT_EQ(sourceTiles, (std::vector<uint32_t>{0, 127, 8064, 8191}));

  std::vector<Operation> operations;
  for (const uint32_t tile : sourceTiles) {
    NativeTileUpload upload;
    upload.destination = NativeRect{
        static_cast<int32_t>((tile % CRASH_REPLAY_NATIVE_TILE_COLUMNS) *
                             CRASH_REPLAY_NATIVE_TILE_WIDTH),
        static_cast<int32_t>((tile / CRASH_REPLAY_NATIVE_TILE_COLUMNS) *
                             CRASH_REPLAY_NATIVE_TILE_HEIGHT),
        CRASH_REPLAY_NATIVE_TILE_WIDTH, CRASH_REPLAY_NATIVE_TILE_HEIGHT};
    upload.pixels.resize(static_cast<std::size_t>(
        CRASH_REPLAY_NATIVE_TILE_WIDTH * CRASH_REPLAY_NATIVE_TILE_HEIGHT));
    upload.nativeRevision = 2;
    operations.emplace_back(std::move(upload));
    afterUploads.nativeTileRevisions[tile] = 2;
  }
  setNativeReadProof(copy, afterUploads);
  operations.emplace_back(copy);

  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const ReplayPreflightResult accepted =
      runOne(makeFrame(expected, operations), expected, confirmed);
  EXPECT_EQ(accepted.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(accepted.reason, ReplayPreflightReason::None);
  EXPECT_EQ(accepted.counters.nativeReadPackets, 1u);
  EXPECT_EQ(accepted.counters.nativeReadTiles, sourceTiles.size());

  CopyRect wrongCount = copy;
  --wrongCount.nativeRead.tileCount;
  EXPECT_EQ(runOne(makeFrame(expected, {Operation{wrongCount}}), expected,
                   afterUploads)
                .reason,
            ReplayPreflightReason::NativeReadCountMismatch);

  CopyRect wrongHash = copy;
  wrongHash.nativeRead.versionHash ^= UINT64_C(0x100);
  EXPECT_EQ(runOne(makeFrame(expected, {Operation{wrongHash}}), expected,
                   afterUploads)
                .reason,
            ReplayPreflightReason::NativeReadHashMismatch);

  ReplayPreflightState unknown = afterUploads;
  unknown.knownNativeTiles.reset(sourceTiles.front());
  EXPECT_EQ(runOne(makeFrame(expected, {Operation{copy}}), expected, unknown)
                .reason,
            ReplayPreflightReason::NativeReadTileUnknown);
}

TEST(GpuModernReplayPreflightTest,
     CopyPaintAndCpuUploadRequireFinalTileUploadBeforeNextNativeRead) {
  const ReplayPreflightState confirmed = anchoredState();

  CopyRect firstCopy;
  firstCopy.source = {0, 0};
  firstCopy.destination = {16, 16};
  firstCopy.width = 1;
  firstCopy.height = 1;
  setNativeReadProof(firstCopy, confirmed);

  PaintRect paint;
  paint.destination = {17, 17, 1, 1};
  paint.nativeColor = 0x2222;
  paint.depth.epoch = confirmed.depthEpoch;
  paint.depth.invalidateOnAccepted = true;
  paint.coalescedCommandCount = 1;

  const NativeRect transfer{18, 18, 1, 1};
  const CpuUpload begin =
      cpuUpload(100, CpuUploadPhase::Begin, transfer);
  const CpuUpload data =
      cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {0x3333});
  const CpuUpload complete =
      cpuUpload(100, CpuUploadPhase::Complete, transfer, 1);

  constexpr uint32_t destinationTile = 2u + 2u * 128u;
  NativeTileUpload latestTile;
  latestTile.destination = {16, 16, 8, 8};
  latestTile.pixels.resize(64);
  latestTile.nativeRevision = 4;

  ReplayPreflightState afterLatestTile = confirmed;
  afterLatestTile.nativeTileRevisions[destinationTile] = 4;
  CopyRect secondCopy;
  secondCopy.source = {16, 16};
  secondCopy.destination = {64, 64};
  secondCopy.width = 8;
  secondCopy.height = 8;
  setNativeReadProof(secondCopy, afterLatestTile);

  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const std::vector<Operation> correctOperations{
      Operation{firstCopy}, Operation{paint},      Operation{begin},
      Operation{data},      Operation{complete},   Operation{latestTile},
      Operation{secondCopy}};
  const ReplayPreflightResult accepted =
      runOne(makeFrame(expected, correctOperations), expected, confirmed);
  EXPECT_EQ(accepted.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(accepted.reason, ReplayPreflightReason::None);
  EXPECT_EQ(accepted.candidateState.nativeTileRevisions[destinationTile], 4u);
  EXPECT_EQ(accepted.counters.nativeReadPackets, 2u);
  EXPECT_EQ(accepted.counters.nativeReadTiles, 2u);

  const std::vector<Operation> staleOperations{
      Operation{firstCopy}, Operation{paint},    Operation{begin},
      Operation{data},      Operation{complete}, Operation{secondCopy}};
  const ReplayPreflightResult rejected =
      runOne(makeFrame(expected, staleOperations), expected, confirmed);
  EXPECT_EQ(rejected.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(rejected.reason, ReplayPreflightReason::NativeReadHashMismatch);
  EXPECT_EQ(rejected.candidateState, confirmed)
      << "semantic mutations do not silently advance the read-through cache; "
         "only an ordered NativeTileUpload may satisfy the later proof";
}

TEST(GpuModernReplayPreflightTest,
     ConservativeNativeReadTilesStayOrderedAndDeduplicateWrappedClut) {
  DrawPacket packet = projectedPacket(100);
  packet.opcode = 0x24;
  packet.textured = true;
  packet.state.texturePage.baseX = 960;
  packet.state.texturePage.baseY = 0;
  packet.state.texturePage.depth = TextureDepth::Indexed8;
  packet.state.clut.x = 1008;
  packet.state.clut.y = 0;

  const std::vector<uint32_t> tiles = conservativeNativeReadTileIndices(packet);

  EXPECT_EQ(tiles.size(), 534u);
  EXPECT_TRUE(std::is_sorted(tiles.begin(), tiles.end()));
  EXPECT_EQ(std::adjacent_find(tiles.begin(), tiles.end()), tiles.end());
}

TEST(GpuModernReplayPreflightTest,
     DecodedReadSetMatchesProducerAndRawMismatchNeedsExplicitImportPolicy) {
  const ReplayPreflightState confirmed = anchoredState();
  DrawPacket packet = projectedPacket(100);
  packet.opcode = 0x24;
  packet.textured = true;
  packet.state.textureUvDomain.enabled = true;
  packet.state.texturePage.raw = 0x009A;
  packet.state.texturePage.baseX = 640;
  packet.state.texturePage.baseY = 256;
  packet.state.texturePage.depth = TextureDepth::Indexed8;
  packet.state.clut.raw = 0x643F;
  packet.state.clut.x = 1008;
  packet.state.clut.y = 400;
  setNativeReadProof(packet, confirmed);
  EXPECT_EQ(packet.nativeRead.tileCount, 544u);

  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const auto canonical = makeFrame(expected, {Operation{packet}});
  EXPECT_EQ(runOne(canonical, expected, confirmed).route,
            ReplayPreflightRoute::M1Replay);

  packet.state.texturePage.raw ^= 1u;
  const auto mismatch = makeFrame(expected, {Operation{packet}});
  EXPECT_EQ(runOne(mismatch, expected, confirmed).reason,
            ReplayPreflightReason::DecodedTextureStateMismatch);

  const std::vector<CrashReplayFramePtr> fifo{mismatch};
  ReplayPreflightRequest importRequest = requestFor(expected);
  importRequest.requireCanonicalRawState = false;
  EXPECT_EQ(preflightCrashReplay(fifo, importRequest, confirmed).route,
            ReplayPreflightRoute::M1Replay);
}

TEST(GpuModernReplayPreflightTest,
     CanonicalRawStateCoversBlendDitherDisplayAndTexturedStpPolicy) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const auto reasonFor = [&](const DrawPacket &packet) {
    return runOne(makeFrame(expected, {Operation{packet}}), expected, confirmed)
        .reason;
  };

  DrawPacket canonical = projectedPacket(100);
  canonical.state.texturePage.raw =
      static_cast<uint16_t>((2u << 5u) | (1u << 9u) | (1u << 10u));
  canonical.state.blend.mode = BlendMode::BackgroundMinusForeground;
  canonical.state.dither = true;
  canonical.state.drawToDisplayArea = true;
  EXPECT_EQ(reasonFor(canonical), ReplayPreflightReason::None);

  DrawPacket blendMode = canonical;
  blendMode.state.blend.mode = BlendMode::BackgroundPlusForeground;
  EXPECT_EQ(reasonFor(blendMode),
            ReplayPreflightReason::DecodedTextureStateMismatch);

  DrawPacket invalidBlendMode = canonical;
  invalidBlendMode.state.blend.mode = static_cast<BlendMode>(0xFF);
  EXPECT_EQ(reasonFor(invalidBlendMode),
            ReplayPreflightReason::DecodedTextureStateMismatch);

  DrawPacket dither = canonical;
  dither.state.dither = false;
  EXPECT_EQ(reasonFor(dither),
            ReplayPreflightReason::DecodedTextureStateMismatch);

  DrawPacket display = canonical;
  display.state.drawToDisplayArea = false;
  EXPECT_EQ(reasonFor(display),
            ReplayPreflightReason::DecodedTextureStateMismatch);

  DrawPacket stp = texturedPacket(100, confirmed);
  stp.state.blend.texturedStpOnly = true;
  EXPECT_EQ(reasonFor(stp), ReplayPreflightReason::DecodedTextureStateMismatch);
}

TEST(GpuModernReplayPreflightTest, SurfaceSeedReanchorsEveryNativeTile) {
  ReplayPreflightState unseeded = anchoredState();
  unseeded.surfaceInitialized = false;
  unseeded.depthInitialized = true;
  unseeded.depthEpoch = 99;
  unseeded.nativeTileRevisions.fill(99);
  unseeded.knownNativeTiles.reset();

  const FrameLatch emptyLatch = latch(10, 100, 10, 100, 100);
  const auto emptyFrame = makeFrame(emptyLatch);
  EXPECT_EQ(runOne(emptyFrame, emptyLatch, unseeded).reason,
            ReplayPreflightReason::SurfaceNotInitialized);

  SurfaceSeed seed;
  seed.destination =
      NativeRect{0, 0, CRASH_REPLAY_NATIVE_WIDTH, CRASH_REPLAY_NATIVE_HEIGHT};
  seed.pixels.resize(static_cast<std::size_t>(CRASH_REPLAY_NATIVE_WIDTH) *
                     CRASH_REPLAY_NATIVE_HEIGHT);
  seed.nativeRevision = 7;
  seed.depthEpoch = 7;

  ReplayPreflightState seeded = unseeded;
  seeded.surfaceInitialized = true;
  seeded.depthInitialized = true;
  seeded.depthEpoch = 7;
  seeded.nativeTileRevisions.fill(7);
  seeded.knownNativeTiles.set();
  const DrawPacket packet = texturedPacket(103, seeded);
  const FrameLatch expected = latch(10, 100, 10, 100, 105);
  const auto frame =
      makeFrame(expected, {Operation{std::move(seed)}, Operation{packet}});

  const ReplayPreflightResult result = runOne(frame, expected, unseeded);
  ASSERT_EQ(result.route, ReplayPreflightRoute::M1Replay);
  EXPECT_TRUE(result.candidateState.surfaceInitialized);
  EXPECT_TRUE(result.candidateState.depthInitialized);
  EXPECT_EQ(result.candidateState.depthEpoch, 7u);
  EXPECT_TRUE(result.candidateState.knownNativeTiles.all());
  EXPECT_EQ(result.candidateState.nativeTileRevisions.front(), 7u);
  EXPECT_EQ(result.candidateState.nativeTileRevisions.back(), 7u);
}

TEST(GpuModernReplayPreflightTest,
     DepthEpochAdvancesExactlyAndAllRasterWritesReferenceIt) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  ClearDepth clear;
  clear.nextEpoch = 2;
  DrawPacket draw = projectedPacket(100, 10, 2);
  Reset reset;
  reset.nextDepthEpoch = 3;
  PaintPixels pixels;
  pixels.pixels.push_back(PaintedPixel{});
  pixels.depth.epoch = 3;
  pixels.depth.invalidateOnAccepted = true;
  PaintRect rect;
  rect.destination = NativeRect{2, 3, 1, 1};
  rect.depth.epoch = 3;
  rect.depth.invalidateOnAccepted = true;

  const auto frame =
      makeFrame(expected, {Operation{clear}, Operation{draw}, Operation{reset},
                           Operation{std::move(pixels)}, Operation{rect}});
  const ReplayPreflightResult result = runOne(frame, expected, confirmed);
  ASSERT_EQ(result.route, ReplayPreflightRoute::M1Replay);
  EXPECT_TRUE(result.candidateState.depthInitialized);
  EXPECT_EQ(result.candidateState.depthEpoch, 3u);
}

TEST(GpuModernReplayPreflightTest,
     DepthEpochRejectsDiscontinuityOverflowAndStaleRasterReferences) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  ClearDepth skipped;
  skipped.nextEpoch = 3;
  EXPECT_EQ(
      runOne(makeFrame(expected, {Operation{skipped}}), expected, confirmed)
          .reason,
      ReplayPreflightReason::DepthEpochDiscontinuity);

  Reset reset;
  reset.nextDepthEpoch = 3;
  EXPECT_EQ(runOne(makeFrame(expected, {Operation{reset}}), expected, confirmed)
                .reason,
            ReplayPreflightReason::DepthEpochDiscontinuity);

  DrawPacket staleDraw = projectedPacket(100, 10, 2);
  EXPECT_EQ(
      runOne(makeFrame(expected, {Operation{staleDraw}}), expected, confirmed)
          .reason,
      ReplayPreflightReason::DepthEpochMismatch);

  PaintPixels stalePixels;
  stalePixels.pixels.push_back(PaintedPixel{});
  stalePixels.depth.epoch = 2;
  stalePixels.depth.invalidateOnAccepted = true;
  EXPECT_EQ(
      runOne(makeFrame(expected, {Operation{stalePixels}}), expected, confirmed)
          .reason,
      ReplayPreflightReason::DepthEpochMismatch);

  PaintRect staleRect;
  staleRect.destination = NativeRect{0, 0, 1, 1};
  staleRect.depth.epoch = 2;
  staleRect.depth.invalidateOnAccepted = true;
  EXPECT_EQ(
      runOne(makeFrame(expected, {Operation{staleRect}}), expected, confirmed)
          .reason,
      ReplayPreflightReason::DepthEpochMismatch);

  ReplayPreflightState maximum = confirmed;
  maximum.depthEpoch = UINT64_MAX;
  ClearDepth overflow;
  overflow.nextEpoch = UINT64_MAX;
  const ReplayPreflightResult overflowResult =
      runOne(makeFrame(expected, {Operation{overflow}}), expected, maximum);
  EXPECT_EQ(overflowResult.reason, ReplayPreflightReason::DepthEpochOverflow);
  EXPECT_EQ(overflowResult.candidateState, maximum);

  DrawPacket invalidCompare = projectedPacket(100);
  invalidCompare.state.depth.compare = static_cast<DepthCompare>(0xFF);
  EXPECT_EQ(runOne(makeFrame(expected, {Operation{invalidCompare}}), expected,
                   confirmed)
                .reason,
            ReplayPreflightReason::InvalidDepthCompare);

  ReplayPreflightState noDepth = confirmed;
  noDepth.depthInitialized = false;
  noDepth.depthEpoch = 0;
  EXPECT_EQ(runOne(makeFrame(expected), expected, noDepth).reason,
            ReplayPreflightReason::DepthNotInitialized);
}

TEST(GpuModernReplayPreflightTest,
     SubmissionZeroIsDirectWhileBatchIdsStayInsideTheirLatch) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);

  const auto direct = makeFrame(expected, {Operation{projectedPacket(100, 0)}});
  EXPECT_EQ(runOne(direct, expected, confirmed).route,
            ReplayPreflightRoute::M1Replay);

  const auto stale = makeFrame(expected, {Operation{projectedPacket(100, 9)}});
  EXPECT_EQ(runOne(stale, expected, confirmed).reason,
            ReplayPreflightReason::PacketSubmissionOutOfRange);

  const auto future =
      makeFrame(expected, {Operation{projectedPacket(100, 11)}});
  EXPECT_EQ(runOne(future, expected, confirmed).reason,
            ReplayPreflightReason::PacketSubmissionOutOfRange);
}

TEST(GpuModernReplayPreflightTest,
     TimelineRegressionsAndInvalidDisplayStateCannotReachM1) {
  const ReplayPreflightState confirmed = anchoredState();

  const FrameLatch skipped = latch(12, 100, 10, 100, 101);
  EXPECT_EQ(runOne(makeFrame(skipped), skipped, confirmed).reason,
            ReplayPreflightReason::FrameNumberDiscontinuity);

  const FrameLatch oldContent = latch(10, 89, 10, 100, 101);
  EXPECT_EQ(runOne(makeFrame(oldContent), oldContent, confirmed).reason,
            ReplayPreflightReason::ContentRevisionRegression);

  const FrameLatch oldSubmission = latch(10, 100, 8, 100, 101);
  EXPECT_EQ(runOne(makeFrame(oldSubmission), oldSubmission, confirmed).reason,
            ReplayPreflightReason::SubmissionRegression);

  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  FrameLatch invalidAperture = expected;
  invalidAperture.display.aperture.width = 513;
  EXPECT_EQ(runOne(makeFrame(invalidAperture), expected, confirmed).reason,
            ReplayPreflightReason::InvalidAperture);

  FrameLatch wrongField = expected;
  wrongField.display.oddField = true;
  EXPECT_EQ(runOne(makeFrame(wrongField), expected, confirmed).reason,
            ReplayPreflightReason::DisplayStateMismatch);
}

TEST(GpuModernReplayPreflightTest,
     LatchIdentityAndCompletenessSelectWholeFrameFallback) {
  const ReplayPreflightState confirmed = anchoredState();
  const FrameLatch actual = latch(10, 100, 10, 100, 101);
  const auto frame = makeFrame(actual);

  FrameLatch wrong = actual;
  wrong.contentRevision = 101;
  EXPECT_EQ(runOne(frame, wrong, confirmed).reason,
            ReplayPreflightReason::ContentRevisionMismatch);
  wrong = actual;
  wrong.completedSubmissionId = 11;
  EXPECT_EQ(runOne(frame, wrong, confirmed).reason,
            ReplayPreflightReason::SubmissionMismatch);
  wrong = actual;
  wrong.display.aperture.width = 511;
  EXPECT_EQ(runOne(frame, wrong, confirmed).reason,
            ReplayPreflightReason::ApertureMismatch);

  ReplayPreflightRequest wrongSchema = requestFor(actual);
  wrongSchema.schemaVersion = 1;
  const std::vector<CrashReplayFramePtr> fifo{frame};
  EXPECT_EQ(preflightCrashReplay(fifo, wrongSchema, confirmed).reason,
            ReplayPreflightReason::SchemaVersionMismatch);

  FrameLatch unavailable = latch(11, 101, 11, 101, 102);
  EXPECT_EQ(runOne(frame, unavailable, confirmed).reason,
            ReplayPreflightReason::TargetLatchUnavailable);

  FrameLatch incomplete = actual;
  incomplete.complete = false;
  const auto incompleteFrame = makeFrame(incomplete);
  EXPECT_EQ(runOne(incompleteFrame, actual, confirmed).reason,
            ReplayPreflightReason::FrameIncomplete);

  FrameLatch unsupported = actual;
  unsupported.complete = false;
  unsupported.unsupportedCount = 2;
  const auto unsupportedFrame = makeFrame(unsupported);
  const ReplayPreflightResult unsupportedResult =
      runOne(unsupportedFrame, actual, confirmed);
  EXPECT_EQ(unsupportedResult.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(unsupportedResult.reason,
            ReplayPreflightReason::UnsupportedMutationCount);
  EXPECT_EQ(unsupportedResult.candidateState, confirmed);
}

TEST(GpuModernReplayPreflightAnchorTest,
     CompleteCapabilityMissReanchorsAndFollowingFrameCanReplayAtM1) {
  const ReplayPreflightState scheduled = anchoredState();

  NativeTileUpload upload;
  upload.destination = NativeRect{0, 0, 8, 8};
  upload.pixels.resize(64, 0x1234);
  upload.nativeRevision = 2;
  ClearDepth clear;
  clear.nextEpoch = 2;

  const FrameLatch fallbackLatch = latch(10, 100, 10, 100, 103);
  const auto fallbackFrame =
      makeFrame(fallbackLatch, {Operation{upload}, Operation{clear}});

  const ReplayCapabilityMask withoutClearDepth =
      CRASH_REPLAY_V1_ALL_CAPABILITIES &
      ~replayCapabilityBit(ReplayCapability::ClearDepth);
  const ReplayPreflightResult strict =
      runOne(fallbackFrame, fallbackLatch, scheduled, withoutClearDepth);
  ASSERT_EQ(strict.route, ReplayPreflightRoute::M0Fallback);
  ASSERT_EQ(strict.reason, ReplayPreflightReason::MissingCapability);
  ASSERT_EQ(strict.candidateState, scheduled);

  const ReplayNativeAnchorResult anchor =
      anchorOne(fallbackFrame, fallbackLatch, scheduled);
  ASSERT_EQ(anchor.route, ReplayNativeAnchorRoute::M0Reanchor);
  ASSERT_EQ(anchor.reason, ReplayPreflightReason::None);
  ASSERT_EQ(anchor.frames.size(), 1u);
  EXPECT_EQ(anchor.frames.front(), fallbackFrame);
  EXPECT_EQ(anchor.candidateState.lastFrameNumber, 10u);
  EXPECT_EQ(anchor.candidateState.contentRevision, 100u);
  EXPECT_EQ(anchor.candidateState.completedSubmissionId, 10u);
  EXPECT_EQ(anchor.candidateState.nextPacketSequence, 103u);
  EXPECT_EQ(anchor.candidateState.depthEpoch, 2u);
  EXPECT_EQ(anchor.candidateState.nativeTileRevisions[0], 2u);

  const FrameLatch followingLatch = latch(11, 101, 11, 103, 104);
  const DrawPacket followingPacket = projectedPacket(103, 11, 2);
  const auto following =
      makeFrame(followingLatch, {Operation{followingPacket}});
  const ReplayPreflightResult replayed =
      runOne(following, followingLatch, anchor.candidateState);
  ASSERT_EQ(replayed.route, ReplayPreflightRoute::M1Replay);
  EXPECT_EQ(replayed.reason, ReplayPreflightReason::None);
  EXPECT_EQ(replayed.candidateState.lastFrameNumber, 11u);
  EXPECT_EQ(replayed.candidateState.nextPacketSequence, 104u);
}

TEST(GpuModernReplayPreflightAnchorTest,
     CpuUploadTransactionSurvivesReanchorAcrossFrameLatch) {
  const ReplayPreflightState scheduled = anchoredState();
  constexpr NativeRect transfer{30, 40, 4, 1};

  FrameLatch partialLatch = latch(10, 100, 10, 100, 101);
  const auto partial = makeFrame(
      partialLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
       Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 0, {1, 2})}});

  const ReplayNativeAnchorResult anchor =
      anchorOne(partial, partialLatch, scheduled);
  ASSERT_EQ(anchor.route, ReplayNativeAnchorRoute::M0Reanchor);
  ASSERT_TRUE(anchor.candidateState.cpuUpload.active);
  EXPECT_EQ(anchor.candidateState.cpuUpload.transactionId, 100u);
  EXPECT_EQ(anchor.candidateState.cpuUpload.transfer, transfer);
  EXPECT_EQ(anchor.candidateState.cpuUpload.nextOffset, 2u);
  EXPECT_EQ(anchor.candidateState.nextPacketSequence, 101u);

  const FrameLatch completeLatch = latch(11, 101, 11, 101, 102);
  const auto complete = makeFrame(
      completeLatch,
      {Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 2, {3, 4})},
       Operation{cpuUpload(100, CpuUploadPhase::Complete, transfer, 4)},
       Operation{projectedPacket(101, 11)}});
  const ReplayPreflightResult replayed =
      runOne(complete, completeLatch, anchor.candidateState);
  ASSERT_EQ(replayed.route, ReplayPreflightRoute::M1Replay);
  EXPECT_FALSE(replayed.candidateState.cpuUpload.active);
  EXPECT_EQ(replayed.candidateState.nextPacketSequence, 102u);
}

TEST(GpuModernReplayPreflightAnchorTest,
     SnapshotIdentitySelectsExactPrefixAndNeverRetainsFutureFrames) {
  const ReplayPreflightState scheduled = anchoredState();
  const FrameLatch firstLatch = latch(10, 100, 10, 100, 101);
  const auto first = makeFrame(firstLatch);
  const FrameLatch targetLatch = latch(11, 101, 11, 101, 103);
  const auto target = makeFrame(targetLatch);
  const auto future = makeFrame(latch(12, 102, 12, 103, 104));
  const std::array<CrashReplayFramePtr, 3> fifo{first, target, future};

  const ReplayNativeAnchorResult result = deriveNativeSnapshotAnchor(
      fifo, anchorRequestFor(targetLatch, target), scheduled);

  ASSERT_EQ(result.route, ReplayNativeAnchorRoute::M0Reanchor);
  ASSERT_EQ(result.frames.size(), 2u);
  EXPECT_EQ(result.frames[0], first);
  EXPECT_EQ(result.frames[1], target);
  EXPECT_EQ(result.counters.inputFrames, 3u);
  EXPECT_EQ(result.counters.retainedFrames, 2u);
  EXPECT_EQ(result.counters.examinedFrames, 2u);
  EXPECT_EQ(result.candidateState.lastFrameNumber, 11u);
  EXPECT_EQ(result.candidateState.nextPacketSequence, 103u);
}

TEST(GpuModernReplayPreflightAnchorTest,
     ExactTokenSelectsSecondEmptyVblankWithUnchangedRevisions) {
  const ReplayPreflightState scheduled = anchoredState();
  const FrameLatch firstLatch = latch(10, 90, 9, 100, 100);
  const FrameLatch secondLatch = latch(11, 90, 9, 100, 100);
  const auto first = makeFrame(firstLatch);
  const auto second = makeFrame(secondLatch);
  const std::array<CrashReplayFramePtr, 2> fifo{first, second};

  const ReplayNativeAnchorResult result = deriveNativeSnapshotAnchor(
      fifo, anchorRequestFor(secondLatch, second), scheduled);

  ASSERT_EQ(result.route, ReplayNativeAnchorRoute::M0Reanchor);
  ASSERT_EQ(result.frames.size(), 2u);
  EXPECT_EQ(result.frames[0].get(), first.get());
  EXPECT_EQ(result.frames[1].get(), second.get());
  EXPECT_EQ(result.candidateState.lastFrameNumber, 11u);
  EXPECT_EQ(result.candidateState.contentRevision, 90u);
  EXPECT_EQ(result.candidateState.completedSubmissionId, 9u);
  EXPECT_EQ(result.candidateState.nextPacketSequence, 100u);
}

TEST(GpuModernReplayPreflightAnchorTest,
     ExactTokenNeverAliasesAnOlderSameValueEmptyLatch) {
  const ReplayPreflightState scheduled = anchoredState();
  const FrameLatch duplicateLatch = latch(10, 90, 9, 100, 100);
  const auto older = makeFrame(duplicateLatch);
  const auto snapshotFrame = makeFrame(duplicateLatch);
  const std::array<CrashReplayFramePtr, 2> fifo{older, snapshotFrame};

  const ReplayNativeAnchorResult result = deriveNativeSnapshotAnchor(
      fifo, anchorRequestFor(duplicateLatch, snapshotFrame), scheduled);

  EXPECT_EQ(result.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(result.reason, ReplayPreflightReason::FrameNumberDiscontinuity);
  ASSERT_EQ(result.frames.size(), 2u);
  EXPECT_EQ(result.frames[0].get(), older.get());
  EXPECT_EQ(result.frames[1].get(), snapshotFrame.get());
  EXPECT_EQ(result.candidateState, scheduled);
}

TEST(GpuModernReplayPreflightAnchorTest,
     SnapshotIdentityMismatchNeverExposesCandidateState) {
  const ReplayPreflightState scheduled = anchoredState();
  FrameLatch actual = latch(10, 100, 10, 100, 101);
  const auto frame = makeFrame(actual);
  const std::array<CrashReplayFramePtr, 1> fifo{frame};
  const ReplayNativeAnchorRequest matching = anchorRequestFor(actual, frame);

  const auto expectFailure = [&](ReplayNativeAnchorRequest request,
                                 ReplayPreflightReason reason) {
    const ReplayNativeAnchorResult result =
        deriveNativeSnapshotAnchor(fifo, request, scheduled);
    EXPECT_EQ(result.route, ReplayNativeAnchorRoute::M0PresentOnly);
    EXPECT_EQ(result.reason, reason);
    EXPECT_EQ(result.candidateState, scheduled);
  };

  ReplayNativeAnchorRequest mismatch = matching;
  ++mismatch.snapshot.contentRevision;
  expectFailure(mismatch, ReplayPreflightReason::ContentRevisionMismatch);
  mismatch = matching;
  ++mismatch.snapshot.completedSubmissionId;
  expectFailure(mismatch, ReplayPreflightReason::SubmissionMismatch);
  mismatch = matching;
  --mismatch.snapshot.display.aperture.width;
  expectFailure(mismatch, ReplayPreflightReason::ApertureMismatch);
  mismatch = matching;
  mismatch.snapshot.display.oddField = true;
  expectFailure(mismatch, ReplayPreflightReason::DisplayStateMismatch);
  mismatch = matching;
  ++mismatch.snapshot.frameNumber;
  expectFailure(mismatch, ReplayPreflightReason::TargetLatchUnavailable);
  mismatch = matching;
  --mismatch.snapshot.frameNumber;
  expectFailure(mismatch, ReplayPreflightReason::TargetLatchUnavailable);
}

TEST(GpuModernReplayPreflightAnchorTest,
     CapabilityMissIsAnchorableButStructuralFailuresAreNot) {
  const ReplayPreflightState scheduled = anchoredState();
  const FrameLatch expected = latch(10, 100, 10, 100, 101);
  const auto draw = makeFrame(expected, {Operation{projectedPacket(100)}});
  const ReplayCapabilityMask withoutDraw =
      CRASH_REPLAY_V1_ALL_CAPABILITIES &
      ~replayCapabilityBit(ReplayCapability::DrawPacket);
  const ReplayPreflightResult missingCapability =
      runOne(draw, expected, scheduled, withoutDraw);
  ASSERT_EQ(missingCapability.reason, ReplayPreflightReason::MissingCapability);
  EXPECT_EQ(anchorOne(draw, expected, scheduled).route,
            ReplayNativeAnchorRoute::M0Reanchor);

  const auto expectStructuralFailure = [&](const CrashReplayFramePtr &frame,
                                           const FrameLatch &frameLatch,
                                           ReplayPreflightReason reason) {
    const ReplayNativeAnchorResult result =
        anchorOne(frame, frameLatch, scheduled);
    EXPECT_EQ(result.route, ReplayNativeAnchorRoute::M0PresentOnly);
    EXPECT_EQ(result.reason, reason);
    EXPECT_EQ(result.candidateState, scheduled);
  };

  DrawPacket badRead = texturedPacket(100, scheduled);
  badRead.nativeRead.versionHash ^= UINT64_C(0x100);
  expectStructuralFailure(makeFrame(expected, {Operation{badRead}}), expected,
                          ReplayPreflightReason::NativeReadHashMismatch);

  ClearDepth skippedDepth;
  skippedDepth.nextEpoch = 3;
  expectStructuralFailure(makeFrame(expected, {Operation{skippedDepth}}),
                          expected,
                          ReplayPreflightReason::DepthEpochDiscontinuity);

  NativeTileUpload repeatedRevision;
  repeatedRevision.destination = NativeRect{0, 0, 8, 8};
  repeatedRevision.pixels.resize(64);
  repeatedRevision.nativeRevision = 1;
  expectStructuralFailure(makeFrame(expected, {Operation{repeatedRevision}}),
                          expected,
                          ReplayPreflightReason::NativeRevisionNotAdvanced);

  const FrameLatch sequenceGap = latch(10, 100, 10, 101, 102);
  expectStructuralFailure(makeFrame(sequenceGap), sequenceGap,
                          ReplayPreflightReason::SequenceRangeDiscontinuity);

  constexpr NativeRect transfer{0, 0, 2, 1};
  const auto a0Gap = makeFrame(
      expected,
      {Operation{cpuUpload(100, CpuUploadPhase::Begin, transfer)},
       Operation{cpuUpload(100, CpuUploadPhase::Data, transfer, 1, {1})}});
  expectStructuralFailure(a0Gap, expected,
                          ReplayPreflightReason::CpuUploadOffsetDiscontinuity);

  DrawPacket rawMismatch = projectedPacket(100);
  rawMismatch.state.texturePage.raw ^= 1u;
  const auto nonCanonical = makeFrame(expected, {Operation{rawMismatch}});
  expectStructuralFailure(nonCanonical, expected,
                          ReplayPreflightReason::DecodedTextureStateMismatch);

  ReplayNativeAnchorRequest importRequest = anchorRequestFor(expected);
  importRequest.requireCanonicalRawState = false;
  const std::array<CrashReplayFramePtr, 1> importFifo{nonCanonical};
  EXPECT_EQ(
      deriveNativeSnapshotAnchor(importFifo, importRequest, scheduled).route,
      ReplayNativeAnchorRoute::M0Reanchor);
}

TEST(GpuModernReplayPreflightAnchorTest,
     IncompleteOrUnsupportedLatchRequiresHardSeedAndNeverAdvancesState) {
  const ReplayPreflightState scheduled = anchoredState();

  FrameLatch incomplete = latch(10, 100, 10, 100, 101);
  incomplete.complete = false;
  const auto incompleteFrame = makeFrame(incomplete);
  const ReplayNativeAnchorResult incompleteResult =
      anchorOne(incompleteFrame, incomplete, scheduled);
  EXPECT_EQ(incompleteResult.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(incompleteResult.reason, ReplayPreflightReason::FrameIncomplete);
  EXPECT_EQ(incompleteResult.candidateState, scheduled);

  // A missing operation could have been an A0 Begin, ClearDepth or Reset.
  // The native pixels dominate color/mask, but cannot reconstruct that
  // continuation state, so unsupportedCount is never a soft reanchor.
  FrameLatch missingStructural = incomplete;
  missingStructural.unsupportedCount = 1;
  const auto missingStructuralFrame = makeFrame(missingStructural);
  const ReplayNativeAnchorResult unsupportedResult =
      anchorOne(missingStructuralFrame, missingStructural, scheduled);
  EXPECT_EQ(unsupportedResult.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(unsupportedResult.reason,
            ReplayPreflightReason::UnsupportedMutationCount);
  EXPECT_EQ(unsupportedResult.candidateState, scheduled);
}

TEST(GpuModernReplayPreflightAnchorTest,
     IncompleteNativeReanchorOptInRequiresExactTokenAndAdvancesPastPrefix) {
  const ReplayPreflightState scheduled = anchoredState();

  const auto exercise = [&](bool unsupported) {
    SCOPED_TRACE(unsupported ? "unsupported" : "incomplete");
    FrameLatch brokenLatch = latch(10, 100, 10, 100, 101);
    brokenLatch.complete = false;
    if (unsupported)
      brokenLatch.unsupportedCount = 1;
    const auto broken = makeFrame(brokenLatch);

    const FrameLatch targetLatch = latch(11, 101, 11, 101, 101);
    const auto target = makeFrame(targetLatch);
    const std::array<CrashReplayFramePtr, 2> fifo{broken, target};
    const ReplayPreflightReason expectedStrictReason =
        unsupported ? ReplayPreflightReason::UnsupportedMutationCount
                    : ReplayPreflightReason::FrameIncomplete;

    // The strict M1 gate never inherits the native-anchor opt-in.
    const ReplayPreflightResult strict =
        preflightCrashReplay(fifo, requestFor(targetLatch), scheduled);
    EXPECT_EQ(strict.route, ReplayPreflightRoute::M0Fallback);
    EXPECT_EQ(strict.reason, expectedStrictReason);
    EXPECT_EQ(strict.candidateState, scheduled);

    // Default false preserves the original fail-closed anchor behavior.
    const ReplayNativeAnchorRequest defaultRequest =
        anchorRequestFor(targetLatch, target);
    const ReplayNativeAnchorResult defaultResult =
        deriveNativeSnapshotAnchor(fifo, defaultRequest, scheduled);
    EXPECT_EQ(defaultResult.route, ReplayNativeAnchorRoute::M0PresentOnly);
    EXPECT_EQ(defaultResult.reason, expectedStrictReason);
    EXPECT_EQ(defaultResult.candidateState, scheduled);

    // Opt-in alone is insufficient for an offline/scalar latch look-up.
    ReplayNativeAnchorRequest noStrongToken = defaultRequest;
    noStrongToken.allowIncompleteNativeReanchor = true;
    noStrongToken.snapshot.sealedFrame.reset();
    const ReplayNativeAnchorResult tokenless =
        deriveNativeSnapshotAnchor(fifo, noStrongToken, scheduled);
    EXPECT_EQ(tokenless.route, ReplayNativeAnchorRoute::M0PresentOnly);
    EXPECT_EQ(tokenless.reason, expectedStrictReason);
    EXPECT_EQ(tokenless.candidateState, scheduled);

    ReplayNativeAnchorRequest optedIn = defaultRequest;
    optedIn.allowIncompleteNativeReanchor = true;
    const ReplayNativeAnchorResult anchored =
        deriveNativeSnapshotAnchor(fifo, optedIn, scheduled);
    ASSERT_EQ(anchored.route, ReplayNativeAnchorRoute::M0Reanchor);
    EXPECT_EQ(anchored.reason, ReplayPreflightReason::None);
    ASSERT_EQ(anchored.frames.size(), 2u);
    EXPECT_EQ(anchored.frames[0].get(), broken.get());
    EXPECT_EQ(anchored.frames[1].get(), target.get());
    EXPECT_EQ(anchored.candidateState.lastFrameNumber, 11u);
    EXPECT_EQ(anchored.candidateState.contentRevision, 101u);
    EXPECT_EQ(anchored.candidateState.completedSubmissionId, 11u);
    EXPECT_EQ(anchored.candidateState.nextPacketSequence, 101u);

    // The candidate is beyond the incomplete frame, so the following latch is
    // no longer held behind it and can pass ordinary strict M1 preflight.
    const FrameLatch followingLatch = latch(12, 102, 12, 101, 101);
    const ReplayPreflightResult following = runOne(
        makeFrame(followingLatch), followingLatch, anchored.candidateState);
    EXPECT_EQ(following.route, ReplayPreflightRoute::M1Replay);
    EXPECT_EQ(following.reason, ReplayPreflightReason::None);
    EXPECT_EQ(following.candidateState.lastFrameNumber, 12u);
  };

  exercise(false);
  exercise(true);

  FrameLatch unrecoverableLatch = latch(10, 100, 10, 100, 101);
  unrecoverableLatch.complete = false;
  unrecoverableLatch.unsupportedCount = 1;
  unrecoverableLatch.unrecoverableCount = 1;
  const auto unrecoverable = makeFrame(unrecoverableLatch);
  const std::array<CrashReplayFramePtr, 1> fifo{unrecoverable};
  ReplayNativeAnchorRequest request =
      anchorRequestFor(unrecoverableLatch, unrecoverable);
  request.allowIncompleteNativeReanchor = true;

  const ReplayNativeAnchorResult rejected =
      deriveNativeSnapshotAnchor(fifo, request, scheduled);
  EXPECT_EQ(rejected.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(rejected.reason, ReplayPreflightReason::UnsupportedMutationCount);
  EXPECT_EQ(rejected.candidateState, scheduled);
}

TEST(GpuModernReplayPreflightAnchorTest,
     UnknownRevisionBaselineRequiresDominatingWholeSurfaceSeed) {
  ReplayPreflightState unknown = anchoredState();
  unknown.knownNativeTiles.reset();

  const FrameLatch targetLatch = latch(10, 100, 10, 100, 101);
  const auto noSeed = makeFrame(targetLatch);
  const ReplayNativeAnchorResult unavailable =
      anchorOne(noSeed, targetLatch, unknown);
  ASSERT_EQ(unavailable.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(unavailable.reason,
            ReplayPreflightReason::NativeAnchorBaselineUnavailable);
  EXPECT_EQ(unavailable.candidateState, unknown);

  SurfaceSeed seed;
  seed.destination =
      NativeRect{0, 0, CRASH_REPLAY_NATIVE_WIDTH, CRASH_REPLAY_NATIVE_HEIGHT};
  seed.pixels.resize(static_cast<std::size_t>(CRASH_REPLAY_NATIVE_WIDTH) *
                     CRASH_REPLAY_NATIVE_HEIGHT);
  seed.nativeRevision = 7;
  seed.depthEpoch = 9;
  const auto seeded = makeFrame(targetLatch, {Operation{std::move(seed)}});
  const ReplayNativeAnchorResult recovered =
      anchorOne(seeded, targetLatch, unknown);
  ASSERT_EQ(recovered.route, ReplayNativeAnchorRoute::M0Reanchor);
  EXPECT_TRUE(recovered.candidateState.knownNativeTiles.all());
  EXPECT_EQ(recovered.candidateState.nativeTileRevisions.front(), 7u);
  EXPECT_EQ(recovered.candidateState.nativeTileRevisions.back(), 7u);
  EXPECT_EQ(recovered.candidateState.depthEpoch, 9u);
}

TEST(GpuModernReplayPreflightAnchorTest,
     CapturePolicyRemainsIndependentAndStrictDuringReanchor) {
  const ReplayPreflightState scheduled = anchoredState();
  FrameLatch actual = latch(10, 100, 10, 100, 101);
  actual.policy = ReplayPolicyState{4, true, true, ReplayDepthMode::Hybrid};
  const auto frame = makeFrame(actual);
  const std::array<CrashReplayFramePtr, 1> fifo{frame};

  ReplayNativeAnchorRequest request = anchorRequestFor(actual, frame);
  EXPECT_EQ(deriveNativeSnapshotAnchor(fifo, request, scheduled).route,
            ReplayNativeAnchorRoute::M0Reanchor);

  request.expectedPolicy.internalScale = 1;
  const ReplayNativeAnchorResult mismatch =
      deriveNativeSnapshotAnchor(fifo, request, scheduled);
  EXPECT_EQ(mismatch.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(mismatch.reason, ReplayPreflightReason::PolicyMismatch);
  EXPECT_EQ(mismatch.candidateState, scheduled);

  request.expectedPolicy = actual.policy;
  request.expectedPolicy.internalScale = 0;
  const ReplayNativeAnchorResult invalid =
      deriveNativeSnapshotAnchor(fifo, request, scheduled);
  EXPECT_EQ(invalid.route, ReplayNativeAnchorRoute::M0PresentOnly);
  EXPECT_EQ(invalid.reason, ReplayPreflightReason::InvalidExpectedPolicy);
  EXPECT_EQ(invalid.candidateState, scheduled);
}
