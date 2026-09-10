#include "runtime/games/crash3_profile.h"
#include "runtime/games/crash3_screen_composition.h"
#include "runtime/gpu/gpu.h"
#include "runtime/gpu/modern_replay_execution_plan.h"
#include "runtime/gpu/modern_replay_preflight.h"
#include "runtime/gpu/modern_replay_vulkan_bake.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using ps1::PGXPOrigin;
using ps1::PGXPValue;
using ps1::gpu::GPU;
using namespace ps1::gpu::replay;

namespace {

constexpr uint32_t xy(int x, int y) {
  return (static_cast<uint32_t>(y) & 0xFFFFu) << 16 |
         (static_cast<uint32_t>(x) & 0xFFFFu);
}

constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16);
}

constexpr uint32_t command(uint8_t opcode, uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(opcode) << 24) | rgb(r, g, b);
}

constexpr uint32_t uv(uint8_t u, uint8_t v, uint16_t upper = 0) {
  return static_cast<uint32_t>(u) | (static_cast<uint32_t>(v) << 8) |
         (static_cast<uint32_t>(upper) << 16);
}

void submit(GPU &gpu, const std::vector<uint32_t> &words) {
  for (const uint32_t word : words)
    gpu.writeGP0(word);
}

PGXPValue preciseVertex(uint32_t packed, float preciseX, float preciseY,
                        double rawGteW) {
  PGXPValue value;
  value.x = preciseX;
  value.y = preciseY;
  value.z = rawGteW;
  value.value = packed;
  value.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z;
  value.origin = PGXPOrigin::GteProjection;
  return value;
}

PGXPValue nearClippedVertex(uint32_t packed, double rawGteW, double nearDepth) {
  PGXPValue value = preciseVertex(
      packed, static_cast<float>(static_cast<int16_t>(packed)),
      static_cast<float>(static_cast<int16_t>(packed >> 16)), rawGteW);
  value.flags |= PGXPValue::VALID_SIGNED_PROJECTION;
  value.qx = static_cast<double>(value.x) * rawGteW;
  value.qy = static_cast<double>(value.y) * rawGteW;
  value.nearDepth = nearDepth;
  value.projectionId = 1;
  value.projectionContext = 1;
  return value;
}

PGXPValue signedVertex(uint32_t packed, float preciseX, float preciseY,
                       double rawGteW, double nearDepth,
                       uint64_t projectionId) {
  PGXPValue value = preciseVertex(packed, preciseX, preciseY, rawGteW);
  value.flags |= PGXPValue::VALID_SIGNED_PROJECTION;
  value.qx = static_cast<double>(preciseX) * rawGteW;
  value.qy = static_cast<double>(preciseY) * rawGteW;
  value.nearDepth = nearDepth;
  value.projectionId = projectionId;
  value.projectionContext = 1;
  value.projectionTransformValueFingerprint = 0x5566778899AABBCCu;
  value.projectionTransformLineage = 0xAABBCCDDu;
  value.modelVertexSource = 0x80030001u + (projectionId - 1u) * 8u;
  value.modelValueFlowSourceFingerprint = 0x11000000u + projectionId;
  value.modelValueFlowProgramFingerprint = 0x2233445566778899u;
  value.flags |= PGXPValue::VALID_TRANSFORM_LINEAGE |
                 PGXPValue::VALID_MODEL_VERTEX_SOURCE |
                 PGXPValue::VALID_MODEL_VALUE_FLOW;
  value.ramLookup = ps1::PGXPRamLookup::HitXYZ;
  value.submissionPath = ps1::PGXPSubmissionPath::HleOrderingTable;
  return value;
}

PGXPValue xyOnlyVertex(uint32_t packed, float preciseX, float preciseY) {
  PGXPValue value;
  value.x = preciseX;
  value.y = preciseY;
  value.z = std::numeric_limits<double>::quiet_NaN();
  value.value = packed;
  value.flags = PGXPValue::VALID_XY;
  value.origin = PGXPOrigin::GteProjection;
  return value;
}

std::vector<const DrawPacket *> drawsIn(const CrashReplayFrame &frame) {
  std::vector<const DrawPacket *> draws;
  for (const Operation &operation : frame.operations()) {
    if (const auto *draw = std::get_if<DrawPacket>(&operation))
      draws.push_back(draw);
  }
  return draws;
}

template <typename T>
std::vector<const T *> operationsOf(const CrashReplayFrame &frame) {
  std::vector<const T *> operations;
  for (const Operation &operation : frame.operations()) {
    if (const auto *typed = std::get_if<T>(&operation))
      operations.push_back(typed);
  }
  return operations;
}

bool sameNativeVram(const GPU &left, const GPU &right) {
  constexpr std::size_t pixelCount =
      static_cast<std::size_t>(GPU::VRAM_WIDTH) * GPU::VRAM_HEIGHT;
  return std::memcmp(left.getVRAM(), right.getVRAM(),
                     pixelCount * sizeof(ps1::gpu::Color16)) == 0;
}

GPU::ReplayConfig captureConfig(bool pgxp = false) {
  GPU::ReplayConfig config;
  config.enabled = true;
  config.internalScale = 4;
  config.pgxp = pgxp;
  config.trueColor = true;
  config.trueDepthMode = ReplayDepthMode::Hybrid;
  return config;
}

} // namespace

TEST(GpuModernReplayCaptureTest,
     ProjectedPointKeepsSubpixelPositionAndDepthWithoutChangingNativePixel) {
  GPU reference;
  GPU captured;
  captured.configureReplay(captureConfig(true));
  const uint32_t packed = xy(30, 40);
  const PGXPValue precision = signedVertex(packed, 30.25f, 40.75f, 5000, 1, 3);
  for (GPU *gpu : {&reference, &captured}) {
    gpu->writeGP0(command(0x68, 255, 128, 32));
    gpu->writeGP0(packed, UINT32_MAX, &precision);
  }
  EXPECT_TRUE(sameNativeVram(reference, captured));
  captured.snapshotDisplayBuffer(1);
  const auto frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const auto &point = *draws[0];
  EXPECT_EQ(point.opcode, 0x68u);
  EXPECT_EQ(point.route, PacketRoute::Projected3D);
  EXPECT_EQ(point.validMask, 15u);
  EXPECT_EQ(point.vertexCount, 4u);
  for (uint32_t corner = 0; corner != 4u; ++corner) {
    EXPECT_DOUBLE_EQ(point.vertices[corner].x, 30.25 + (corner & 1u));
    EXPECT_DOUBLE_EQ(point.vertices[corner].y, 40.75 + (corner >> 1u));
    EXPECT_EQ(point.vertices[corner].nativeX, 30 + (corner & 1u));
    EXPECT_EQ(point.vertices[corner].nativeY, 40 + (corner >> 1u));
    EXPECT_TRUE(point.vertices[corner].preciseXY);
    EXPECT_TRUE(point.vertices[corner].rawDepthValid);
    EXPECT_FALSE(point.vertices[corner].inventedW);
  }
  EXPECT_TRUE(measureReplayPrecisionCoverage(std::array{frame})
                  .projectedCoordinatesComplete());
  auto invalid = point;
  invalid.route = PacketRoute::Screen2D;
  CrashReplayFrameBuilder builder;
  EXPECT_THROW(builder.record(invalid), std::invalid_argument);
  invalid = point;
  invalid.validMask = 7u;
  EXPECT_THROW(builder.record(invalid), std::invalid_argument);
}

TEST(GpuModernReplayCaptureTest, UnprovenPointsNeverInventAProjection) {
  for (uint32_t variant = 0; variant != 5u; ++variant) {
    GPU gpu;
    gpu.configureReplay(captureConfig(true));
    const uint32_t packed = xy(30, 40);
    auto precision = signedVertex(packed, 30.25f, 40.75f, 5000, 1, 3);
    if (variant == 0u)
      precision = {};
    else if (variant == 1u)
      precision = xyOnlyVertex(packed, 30.25f, 40.75f);
    else if (variant == 2u)
      precision.value = xy(99, 99);
    else if (variant == 3u)
      precision = signedVertex(packed, 30.25f, 40.75f, -5000, 1, 3);
    else
      precision = signedVertex(packed, 30.25f, 40.75f, 0.5, 1, 3);
    gpu.writeGP0(command(0x68, 255, 255, 255));
    gpu.writeGP0(packed, UINT32_MAX, &precision);
    gpu.snapshotDisplayBuffer(1);
    const auto frame = gpu.tryConsumeReplay();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(drawsIn(*frame).empty()) << variant;
    const auto points = operationsOf<PaintRect>(*frame);
    ASSERT_EQ(points.size(), 1u) << variant;
    EXPECT_EQ(points[0]->destination, (NativeRect{30, 40, 1, 1}));
  }
}

TEST(GpuModernReplayCaptureTest,
     ProjectedFixedLinesKeepNativeRasterAndEndpointDepth) {
  for (uint8_t opcode : {0x40u, 0x42u, 0x50u, 0x52u}) {
    GPU reference;
    GPU captured;
    captured.configureReplay(captureConfig(true));
    const bool gouraud = (opcode & 16u) != 0u;
    const auto a = signedVertex(xy(10, 10), 10.25f, 10.25f, 5000, 1, 1);
    const auto b = signedVertex(xy(10, 20), 10.25f, 20.25f, 5100, 1, 2);
    for (GPU *gpu : {&reference, &captured}) {
      gpu->writeGP0(command(opcode, 255, 128, 32));
      gpu->writeGP0(a.value, UINT32_MAX, &a);
      if (gouraud)
        gpu->writeGP0(rgb(32, 128, 255));
      gpu->writeGP0(b.value, UINT32_MAX, &b);
    }
    EXPECT_TRUE(sameNativeVram(reference, captured))
        << static_cast<int>(opcode);
    captured.snapshotDisplayBuffer(1);
    const auto frame = captured.tryConsumeReplay();
    ASSERT_TRUE(frame);
    const auto draws = drawsIn(*frame);
    ASSERT_EQ(draws.size(), 1u);
    const auto &line = *draws[0];
    EXPECT_EQ(line.opcode, opcode);
    EXPECT_EQ(line.route, PacketRoute::Projected3D);
    EXPECT_EQ(line.vertexCount, 4u);
    EXPECT_EQ(line.validMask, 15u);
    EXPECT_EQ(line.gouraud, gouraud);
    EXPECT_EQ(line.state.blend.enabled, (opcode & 2u) != 0u);
    EXPECT_DOUBLE_EQ(line.vertices[0].x, 11.25);
    EXPECT_DOUBLE_EQ(line.vertices[1].x, 10.25);
    EXPECT_DOUBLE_EQ(line.vertices[0].y, 10.25);
    EXPECT_DOUBLE_EQ(line.vertices[2].y, 21.25);
    EXPECT_TRUE(operationsOf<PaintPixels>(*frame).empty());
    EXPECT_TRUE(
        measureReplayPrecisionCoverage(std::array{frame}).fullyHighPrecision());
  }
}

TEST(GpuModernReplayCaptureTest,
     LineWithOneUnprovenEndpointStaysAtomicNativePaint) {
  GPU captured;
  captured.configureReplay(captureConfig(true));
  const auto a = signedVertex(xy(10, 10), 10.25f, 10.25f, 5000, 1, 1);
  captured.writeGP0(command(0x52, 255, 128, 32));
  captured.writeGP0(a.value, UINT32_MAX, &a);
  captured.writeGP0(rgb(32, 128, 255));
  captured.writeGP0(xy(10, 20));
  captured.snapshotDisplayBuffer(1);
  const auto frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  EXPECT_TRUE(drawsIn(*frame).empty());
  EXPECT_EQ(operationsOf<PaintPixels>(*frame).size(), 1u);
}

TEST(GpuModernReplayCaptureTest,
     CapturesEveryPolygonFamilyWithoutChangingNativeRaster) {
  GPU reference;
  GPU captured;
  captured.configureReplay(captureConfig());

  ASSERT_TRUE(captured.isReplayCaptureEnabled());
  EXPECT_FALSE(captured.isEnhancedRasterEnabled());
  EXPECT_EQ(captured.internalResolutionScale(), 1u);
  EXPECT_FALSE(captured.isTrueColorEnabled());

  // Rectangle flip bits deliberately remain latched in the native GPU but
  // must not leak into any polygon packet.
  const uint32_t drawMode = 0xE1000000u | (1u << 12) | (1u << 13);
  reference.writeGP0(drawMode);
  captured.writeGP0(drawMode);

  constexpr uint16_t clut = 0;
  constexpr uint16_t direct15Page = 0x100;
  const std::vector<std::vector<uint32_t>> packets = {
      {command(0x20, 255, 0, 0), xy(8, 8), xy(24, 8), xy(8, 24)},
      {command(0x27, 128, 128, 128), xy(40, 8), uv(0, 0, clut), xy(56, 8),
       uv(15, 0, direct15Page), xy(40, 24), uv(0, 15)},
      {command(0x2A, 0, 255, 0), xy(72, 8), xy(88, 8), xy(72, 24), xy(88, 24)},
      {command(0x2D, 128, 128, 128), xy(104, 8), uv(0, 0, clut), xy(120, 8),
       uv(15, 0, direct15Page), xy(104, 24), uv(0, 15), xy(120, 24),
       uv(15, 15)},
      {command(0x30, 255, 0, 0), xy(8, 40), rgb(0, 255, 0), xy(24, 40),
       rgb(0, 0, 255), xy(8, 56)},
      {command(0x37, 255, 0, 0), xy(40, 40), uv(0, 0, clut), rgb(0, 255, 0),
       xy(56, 40), uv(15, 0, direct15Page), rgb(0, 0, 255), xy(40, 56),
       uv(0, 15)},
      {command(0x3A, 255, 0, 0), xy(72, 40), rgb(0, 255, 0), xy(88, 40),
       rgb(0, 0, 255), xy(72, 56), rgb(255, 255, 255), xy(88, 56)},
      {command(0x3D, 255, 0, 0), xy(104, 40), uv(0, 0, clut), rgb(0, 255, 0),
       xy(120, 40), uv(15, 0, direct15Page), rgb(0, 0, 255), xy(104, 56),
       uv(0, 15), rgb(255, 255, 255), xy(120, 56), uv(15, 15)},
  };

  for (const auto &packet : packets) {
    submit(reference, packet);
    submit(captured, packet);
  }
  EXPECT_TRUE(sameNativeVram(reference, captured));

  captured.snapshotDisplayBuffer(41);
  const CrashReplayFramePtr frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->latch().frameNumber, 41u);
  EXPECT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);

  const auto draws = drawsIn(*frame);
  const std::array<uint8_t, 8> expectedOpcodes = {0x20, 0x27, 0x2A, 0x2D,
                                                  0x30, 0x37, 0x3A, 0x3D};
  ASSERT_EQ(draws.size(), expectedOpcodes.size());
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const DrawPacket &draw = *draws[i];
    EXPECT_EQ(draw.opcode, expectedOpcodes[i]);
    EXPECT_EQ(draw.packetSequence, i + 1u); // E1 is sequence zero.
    EXPECT_EQ(draw.route, PacketRoute::Projected3D);
    EXPECT_EQ(draw.semanticReason, PacketSemanticReason::PolygonDefault);
    EXPECT_EQ(draw.validMask, 0u);
    EXPECT_EQ(draw.fallbackReason,
              PacketFallbackReason::MissingPrecisionPacketAtomic);
    EXPECT_EQ(draw.vertexCount, (draw.opcode & 0x08u) ? 4u : 3u);
    EXPECT_EQ(draw.indexCount, (draw.opcode & 0x08u) ? 6u : 3u);
    EXPECT_EQ(draw.textured, (draw.opcode & 0x04u) != 0);
    EXPECT_EQ(draw.gouraud, (draw.opcode & 0x10u) != 0);
    EXPECT_EQ(draw.rawTexture, (draw.opcode & 0x05u) == 0x05u);
    EXPECT_EQ(draw.state.blend.enabled, (draw.opcode & 0x02u) != 0);
    EXPECT_FALSE(draw.state.depth.testEnabled);
    EXPECT_FALSE(draw.state.depth.writeEnabled);
    EXPECT_EQ(draw.state.depth.compare, DepthCompare::Always);
    EXPECT_FALSE(draw.state.texturePage.xFlip);
    EXPECT_FALSE(draw.state.texturePage.yFlip);
    for (std::size_t vertex = 0; vertex < draw.vertexCount; ++vertex)
      EXPECT_DOUBLE_EQ(draw.vertices[vertex].w, CRASH_REPLAY_FALLBACK_W);
  }

  // The first native triangle dirties texture-page zero after rasterization;
  // the next textured packet must receive its tile uploads before DrawPacket.
  std::size_t firstDraw = std::numeric_limits<std::size_t>::max();
  std::size_t secondDraw = std::numeric_limits<std::size_t>::max();
  std::size_t drawCount = 0;
  for (std::size_t i = 0; i < frame->operations().size(); ++i) {
    if (std::holds_alternative<DrawPacket>(frame->operations()[i])) {
      if (drawCount == 0)
        firstDraw = i;
      else if (drawCount == 1)
        secondDraw = i;
      ++drawCount;
    }
  }
  ASSERT_LT(firstDraw, secondDraw);
  bool uploadBeforeTextureRead = false;
  for (std::size_t i = firstDraw + 1; i < secondDraw; ++i)
    uploadBeforeTextureRead |=
        std::holds_alternative<NativeTileUpload>(frame->operations()[i]);
  EXPECT_TRUE(uploadBeforeTextureRead);
  EXPECT_TRUE(draws[1]->nativeRead.required);
  EXPECT_GT(draws[1]->nativeRead.tileCount, 0u);
}

TEST(GpuModernReplayCaptureTest,
     PacksPrecisePostOffsetWAndFourXUvStateWithoutEnablingCpuFourX) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  constexpr uint32_t textureWindow = 3u | (4u << 5) | (5u << 10) | (6u << 15);
  gpu.writeGP0(0xE1000000u | (1u << 9) | (1u << 12) | (1u << 13));
  gpu.writeGP0(0xE2000000u | textureWindow);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 255u | (255u << 10));
  gpu.writeGP0(0xE5000000u | 5u | ((static_cast<uint32_t>(-3) & 0x7FFu) << 11));
  gpu.writeGP0(0xE6000003u);

  constexpr std::array<uint32_t, 4> packed = {xy(10, 10), xy(20, 10),
                                              xy(10, 20), xy(20, 20)};
  const std::array<PGXPValue, 4> precision = {
      preciseVertex(packed[0], 10.25f, 10.5f, 65536.0),
      preciseVertex(packed[1], 20.25f, 10.5f, 65536.0),
      preciseVertex(packed[2], 10.25f, 20.5f, 65536.0),
      preciseVertex(packed[3], 20.25f, 20.5f, 65536.0),
  };
  constexpr std::array<std::array<uint8_t, 2>, 4> tex = {
      std::array<uint8_t, 2>{10, 10}, {0, 10}, {10, 0}, {0, 0}};
  constexpr uint16_t clut = 0x41;
  constexpr uint16_t tpage = 0x102;

  gpu.writeGP0(command(0x2D, 128, 128, 128));
  gpu.writeGP0(packed[0], UINT32_MAX, &precision[0]);
  gpu.writeGP0(uv(tex[0][0], tex[0][1], clut));
  gpu.writeGP0(packed[1], UINT32_MAX, &precision[1]);
  gpu.writeGP0(uv(tex[1][0], tex[1][1], tpage));
  gpu.writeGP0(packed[2], UINT32_MAX, &precision[2]);
  gpu.writeGP0(uv(tex[2][0], tex[2][1]));
  gpu.writeGP0(packed[3], UINT32_MAX, &precision[3]);
  gpu.writeGP0(uv(tex[3][0], tex[3][1]));

  EXPECT_FALSE(gpu.isEnhancedRasterEnabled());
  EXPECT_EQ(gpu.internalResolutionScale(), 1u);
  EXPECT_FALSE(gpu.isTrueColorEnabled());

  gpu.snapshotDisplayBuffer(42);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().sequenceBegin, 0u);
  EXPECT_EQ(frame->latch().sequenceEnd, 7u);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();

  EXPECT_EQ(draw.opcode, 0x2D);
  EXPECT_EQ(draw.packetSequence, 6u);
  EXPECT_EQ(draw.route, PacketRoute::Projected3D);
  EXPECT_EQ(draw.validMask, 0xFu);
  EXPECT_EQ(draw.fallbackReason, PacketFallbackReason::None);
  EXPECT_TRUE(draw.textured);
  EXPECT_TRUE(draw.rawTexture);
  EXPECT_FALSE(draw.state.blend.enabled);
  EXPECT_EQ(draw.state.drawOffset, (DrawOffset{5, -3}));
  EXPECT_EQ(draw.state.drawArea, (DrawArea{0, 0, 255, 255}));
  EXPECT_EQ(draw.state.clut, (ClutState{clut, static_cast<uint16_t>(16),
                                        static_cast<uint16_t>(1)}));
  EXPECT_EQ(draw.state.textureWindow.raw, textureWindow);
  EXPECT_EQ(draw.state.textureWindow.maskX, 24u);
  EXPECT_EQ(draw.state.textureWindow.maskY, 32u);
  EXPECT_EQ(draw.state.textureWindow.offsetX, 40u);
  EXPECT_EQ(draw.state.textureWindow.offsetY, 48u);
  EXPECT_EQ(draw.state.texturePage.raw, static_cast<uint16_t>(tpage | 0x200));
  EXPECT_EQ(draw.state.texturePage.baseX, 128u);
  EXPECT_EQ(draw.state.texturePage.baseY, 0u);
  EXPECT_EQ(draw.state.texturePage.depth, TextureDepth::Direct15);
  EXPECT_FALSE(draw.state.texturePage.xFlip);
  EXPECT_FALSE(draw.state.texturePage.yFlip);
  EXPECT_FALSE(draw.state.texturePage.textureDisable);
  EXPECT_TRUE(draw.state.dither);
  EXPECT_TRUE(draw.state.mask.forceMaskBit);
  EXPECT_TRUE(draw.state.mask.checkMaskBit);
  EXPECT_FALSE(draw.state.depth.testEnabled);
  EXPECT_TRUE(draw.state.depth.writeEnabled);
  EXPECT_EQ(draw.state.depth.compare, DepthCompare::Always);
  EXPECT_EQ(draw.state.depth.epoch, 1u);

  ASSERT_TRUE(draw.state.textureUvDomain.enabled);
  EXPECT_TRUE(draw.state.textureUvDomain.flippedU);
  EXPECT_TRUE(draw.state.textureUvDomain.flippedV);
  EXPECT_EQ(draw.state.textureUvDomain.offsetU, 1u);
  EXPECT_EQ(draw.state.textureUvDomain.offsetV, 1u);
  EXPECT_DOUBLE_EQ(draw.state.textureUvDomain.minU, 1.0);
  EXPECT_DOUBLE_EQ(draw.state.textureUvDomain.maxU, 10.0);
  EXPECT_DOUBLE_EQ(draw.state.textureUvDomain.minV, 1.0);
  EXPECT_DOUBLE_EQ(draw.state.textureUvDomain.maxV, 10.0);

  for (std::size_t i = 0; i < draw.vertexCount; ++i) {
    EXPECT_EQ(draw.vertices[i].nativeX,
              static_cast<int32_t>(packed[i] & 0xFFFFu) + 5);
    EXPECT_EQ(draw.vertices[i].nativeY,
              static_cast<int32_t>(packed[i] >> 16) - 3);
    EXPECT_DOUBLE_EQ(draw.vertices[i].x,
                     static_cast<double>(precision[i].x) + 5.0);
    EXPECT_DOUBLE_EQ(draw.vertices[i].y,
                     static_cast<double>(precision[i].y) - 3.0);
    EXPECT_DOUBLE_EQ(draw.vertices[i].u, tex[i][0] + 1.0);
    EXPECT_DOUBLE_EQ(draw.vertices[i].v, tex[i][1] + 1.0);
    EXPECT_DOUBLE_EQ(draw.vertices[i].w, 65536.0);
    EXPECT_TRUE(draw.vertices[i].preciseXY);
    EXPECT_TRUE(draw.vertices[i].preciseX);
    EXPECT_TRUE(draw.vertices[i].preciseY);
    EXPECT_TRUE(draw.vertices[i].rawDepthValid);
    EXPECT_DOUBLE_EQ(draw.vertices[i].rawDepth, 65536.0);
    EXPECT_FALSE(draw.vertices[i].inventedW);
    EXPECT_FALSE(draw.vertices[i].reconciledX);
    EXPECT_FALSE(draw.vertices[i].reconciledY);
    EXPECT_TRUE(draw.vertices[i].preciseUV);
    EXPECT_EQ(draw.vertices[i].coordinateSpace,
              ReplayVertex::CoordinateSpace::PostDrawOffsetNativePixels);
  }
}

TEST(GpuModernReplayCaptureTest,
     ReconciledNativeComponentCannotClaimFullPrecisionCoverage) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  constexpr std::array<uint32_t, 3> packed = {xy(10, 10), xy(20, 10),
                                              xy(10, 20)};
  std::array<PGXPValue, 3> sidecars = {
      preciseVertex(packed[0], 19.25f, 10.25f, 32768.0),
      preciseVertex(packed[1], 20.25f, 10.25f, 32768.0),
      preciseVertex(packed[2], 10.25f, 20.25f, 32768.0),
  };

  gpu.writeGP0(command(0x20, 255, 255, 255));
  for (std::size_t index = 0; index < packed.size(); ++index)
    gpu.writeGP0(packed[index], UINT32_MAX, &sidecars[index]);

  gpu.snapshotDisplayBuffer(41);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  ASSERT_EQ(draw.validMask, 0x7u)
      << "genuine Z remains valid when one screen component is reconciled";

  const ReplayVertex &reconciled = draw.vertices[0];
  EXPECT_DOUBLE_EQ(reconciled.x, reconciled.nativeX);
  EXPECT_FALSE(reconciled.preciseX);
  EXPECT_TRUE(reconciled.preciseY);
  EXPECT_FALSE(reconciled.preciseXY);
  EXPECT_TRUE(reconciled.reconciledX);
  EXPECT_FALSE(reconciled.reconciledY);
  EXPECT_TRUE(reconciled.rawDepthValid);
  EXPECT_FALSE(reconciled.inventedW);

  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame});
  EXPECT_EQ(coverage.projectedVertices, 3u);
  EXPECT_EQ(coverage.preciseProjectedXVertices, 2u);
  EXPECT_EQ(coverage.preciseProjectedYVertices, 3u);
  EXPECT_EQ(coverage.reconciledProjectedXVertices, 1u);
  EXPECT_EQ(coverage.validProjectedWVertices, 3u);
  EXPECT_FALSE(coverage.projectedCoordinatesComplete());
  EXPECT_FALSE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayCaptureTest,
     CapturedCrashSignedProjectionRetainsCompleteTexturedCoverage) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  constexpr uint32_t packetHead = 0x80076B30u;
  constexpr std::array<uint32_t, 4> packed = {xy(-1000, 1000), 0x03FFFC30u,
                                              xy(-960, 1010), xy(-950, 1020)};
  constexpr double depth = 152.896484375;
  constexpr double nearDepth = 144.000244140625;
  const std::array<PGXPValue, 4> precision = {
      signedVertex(packed[0], -999.75f, 1000.0f, depth, nearDepth, 1),
      signedVertex(packed[1], -967.323974609375f, 1023.0f, depth, nearDepth, 2),
      signedVertex(packed[2], -959.75f, 1010.0f, depth, nearDepth, 3),
      signedVertex(packed[3], -949.75f, 1020.0f, depth, nearDepth, 4),
  };
  constexpr std::array<std::array<uint8_t, 2>, 4> tex = {
      std::array<uint8_t, 2>{0, 0}, {31, 0}, {0, 31}, {31, 31}};

  gpu.writeGP0(command(0x2e, 128, 128, 128), packetHead);
  for (std::size_t i = 0; i < packed.size(); ++i) {
    const uint32_t coordinateSource =
        packetHead + static_cast<uint32_t>((1 + i * 2) * 4);
    gpu.writeGP0(packed[i], coordinateSource, &precision[i]);
    gpu.writeGP0(uv(tex[i][0], tex[i][1]), coordinateSource + 4);
  }

  gpu.snapshotDisplayBuffer(4989);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  ASSERT_EQ(draw.opcode, 0x2e);
  ASSERT_EQ(draw.route, PacketRoute::Projected3D);
  ASSERT_EQ(draw.semanticReason, PacketSemanticReason::PolygonDefault);
  ASSERT_EQ(draw.validMask, 0xFu);
  ASSERT_EQ(draw.fallbackReason, PacketFallbackReason::None);
  ASSERT_TRUE(draw.textured);

  const ReplayVertex &live = draw.vertices[1];
  EXPECT_DOUBLE_EQ(live.x, static_cast<double>(precision[1].x));
  EXPECT_DOUBLE_EQ(live.y, static_cast<double>(precision[1].y));
  EXPECT_TRUE(live.preciseX);
  EXPECT_TRUE(live.preciseY);
  EXPECT_TRUE(live.preciseXY);
  EXPECT_TRUE(live.preciseUV);
  EXPECT_TRUE(live.rawDepthValid);
  EXPECT_FALSE(live.reconciledX);
  EXPECT_FALSE(live.reconciledY);
  EXPECT_FALSE(live.inventedW);
  EXPECT_EQ(live.provenance.projectionId, precision[1].projectionId);
  EXPECT_EQ(live.provenance.projectionContext, precision[1].projectionContext);
  EXPECT_EQ(live.provenance.transformValueFingerprint,
            precision[1].projectionTransformValueFingerprint);
  EXPECT_EQ(live.provenance.transformLineage,
            precision[1].projectionTransformLineage);
  EXPECT_EQ(live.provenance.modelVertexSource, precision[1].modelVertexSource);
  EXPECT_EQ(live.provenance.modelValueFlowSourceFingerprint,
            precision[1].modelValueFlowSourceFingerprint);
  EXPECT_EQ(live.provenance.modelValueFlowProgramFingerprint,
            precision[1].modelValueFlowProgramFingerprint);
  EXPECT_EQ(live.provenance.gp0WordSource, packetHead + 13u);
  EXPECT_EQ(live.provenance.evidence,
            REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE &
                ~ReplayVertexDmaTraversalExact &
                ~ReplayVertexOrderingTableAnchorExact);

  const ReplayTriangleProvenance firstTriangle =
      captureReplayTriangleProvenance(draw, 0);
  ASSERT_TRUE(firstTriangle.validTopology);
  EXPECT_TRUE(firstTriangle.coherentProjectionContext());
  EXPECT_TRUE(firstTriangle.coherentTransformValue());
  EXPECT_TRUE(firstTriangle.coherentTransformLineage());
  EXPECT_EQ(firstTriangle.completeEvidence(),
            REPLAY_VERTEX_PROVENANCE_KNOWN_EVIDENCE &
                ~ReplayVertexDmaTraversalExact &
                ~ReplayVertexOrderingTableAnchorExact);

  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame});
  EXPECT_EQ(coverage.projectedVertices, 4u);
  EXPECT_EQ(coverage.preciseProjectedXYVertices, 4u);
  EXPECT_EQ(coverage.texturedProjectedVertices, 4u);
  EXPECT_EQ(coverage.preciseProjectedUVVertices, 4u);
  EXPECT_EQ(coverage.reconciledProjectedXVertices, 0u);
  EXPECT_EQ(coverage.reconciledProjectedYVertices, 0u);
  EXPECT_EQ(coverage.triangleOwners, 2u);
  EXPECT_EQ(coverage.projectionEventOwnerTriangles, 2u);
  EXPECT_EQ(coverage.coherentProjectionContextOwnerTriangles, 2u);
  EXPECT_EQ(coverage.transformLineageOwnerTriangles, 2u);
  EXPECT_EQ(coverage.coherentTransformLineageOwnerTriangles, 2u);
  EXPECT_EQ(coverage.directModelSourceOwnerTriangles, 2u);
  EXPECT_EQ(coverage.computedModelFlowOwnerTriangles, 2u);
  EXPECT_EQ(coverage.gp0WordSourceOwnerTriangles, 2u);
  EXPECT_EQ(coverage.dmaTraversalOwnerTriangles, 0u);
  EXPECT_EQ(coverage.coherentDmaTraversalOwnerTriangles, 0u);
  EXPECT_TRUE(coverage.fullyHighPrecision());
}

TEST(GpuModernReplayCaptureTest,
     InvalidSignedOriginCannotPublishHomogeneousNumerators) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  constexpr std::array<uint32_t, 3> packed = {xy(10, 10), xy(20, 10),
                                              xy(10, 20)};
  std::array<PGXPValue, 3> sidecars = {
      signedVertex(packed[0], 10.25f, 10.25f, 32768.0, 1.0, 1),
      signedVertex(packed[1], 20.25f, 10.25f, 32768.0, 1.0, 2),
      signedVertex(packed[2], 10.25f, 20.25f, 32768.0, 1.0, 3),
  };
  for (PGXPValue &sidecar : sidecars) {
    sidecar.origin = PGXPOrigin::Unknown;
    sidecar.qx = ps1::PGXP_SIGNED_PROJECTION_GUARD_BAND * 2.0 * sidecar.z;
    ASSERT_FALSE(sidecar.hasSignedProjection(sidecar.value));
  }

  gpu.writeGP0(command(0x20, 255, 255, 255));
  for (std::size_t index = 0; index < packed.size(); ++index)
    gpu.writeGP0(packed[index], UINT32_MAX, &sidecars[index]);

  gpu.snapshotDisplayBuffer(4990);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  ASSERT_EQ(draw.validMask, 0x7u);
  ASSERT_EQ(draw.fallbackReason, PacketFallbackReason::None);
  for (std::size_t index = 0; index < draw.vertexCount; ++index) {
    const ReplayVertex &vertex = draw.vertices[index];
    EXPECT_FALSE(vertex.homogeneousQ);
    EXPECT_DOUBLE_EQ(vertex.x, static_cast<double>(sidecars[index].x));
    EXPECT_DOUBLE_EQ(vertex.qx, vertex.x * vertex.rawDepth)
        << "legacy XYZ must reconstruct Q locally, never consume fake Q";
  }
  EXPECT_TRUE(
      measureReplayPrecisionCoverage(std::array{frame}).fullyHighPrecision())
      << "the established exact legacy XYZ path remains compatible";
}

TEST(GpuModernReplayCaptureTest,
     AuthenticGteSaturationKeepsCompleteReplayCoverageWithExtremeQ) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  // The renderer consumes the one-time native surface seed as its own M0
  // reanchor. Keep this frame focused on the subsequent precision draw batch.
  gpu.snapshotDisplayBuffer(5045);
  ASSERT_TRUE(gpu.tryConsumeReplay());

  // The final tuple and coordinate source below were sampled at the Crash 3
  // demo transition. The first three vertices form a synthetic, non-degenerate
  // continuation so the test also proves the complete quad raster payload.
  constexpr uint32_t packetHead = 0x80093704u;
  constexpr DrawOffset drawOffset{1023, -1000};
  constexpr std::array<uint32_t, 4> packed = {xy(-1023, 1000), xy(-1021, 1000),
                                              xy(-1023, 1002), 0x03FFFC00u};
  constexpr std::array<uint32_t, 4> tex = {uv(0, 0), uv(31, 0, 0x100),
                                           uv(0, 31), uv(31, 31)};
  std::array<PGXPValue, 4> sidecars = {
      signedVertex(packed[0], -1022.75f, 1000.25f, 8.0, 144.000244140625, 1),
      signedVertex(packed[1], -1021.25f, 1000.0f, 8.0, 144.000244140625, 2),
      signedVertex(packed[2], -1022.75f, 1002.25f, 8.0, 144.000244140625, 3),
      signedVertex(packed[3], -1024.0f, 1023.0f, 8.0, 144.000244140625, 4),
  };
  for (PGXPValue &sidecar : sidecars) {
    sidecar.qx = -209628.625;
    sidecar.qy = 136427.3125;
  }

  gpu.writeGP0(0xE5000000u | (static_cast<uint32_t>(drawOffset.x) & 0x7FFu) |
               ((static_cast<uint32_t>(drawOffset.y) & 0x7FFu) << 11));
  gpu.writeGP0(command(0x2e, 128, 128, 128), packetHead);
  for (std::size_t index = 0; index < packed.size(); ++index) {
    const uint32_t coordinateSource =
        packetHead + static_cast<uint32_t>((1 + index * 2) * 4);
    gpu.writeGP0(packed[index], coordinateSource, &sidecars[index]);
    gpu.writeGP0(tex[index], coordinateSource + 4);
  }
  EXPECT_EQ(packetHead + (1u + 3u * 2u) * 4u, 0x80093720u);

  gpu.snapshotDisplayBuffer(5046);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  ASSERT_EQ(draw.opcode, 0x2e);
  ASSERT_EQ(draw.route, PacketRoute::Projected3D);
  ASSERT_EQ(draw.semanticReason, PacketSemanticReason::PolygonDefault);
  ASSERT_EQ(draw.vertexCount, 4u);
  ASSERT_EQ(draw.indexCount, 6u);
  EXPECT_EQ(draw.triangleIndices, (std::array<uint8_t, 6>{0, 1, 2, 1, 2, 3}));
  ASSERT_TRUE(draw.textured);
  EXPECT_FALSE(draw.rawTexture);
  EXPECT_TRUE(draw.state.blend.enabled);
  EXPECT_TRUE(draw.state.blend.texturedStpOnly);
  EXPECT_TRUE(draw.nativeRead.required);
  EXPECT_EQ(draw.state.texturePage.depth, TextureDepth::Direct15);
  EXPECT_FALSE(draw.state.texturePage.textureDisable);
  EXPECT_TRUE(draw.state.textureUvDomain.enabled);
  ASSERT_EQ(draw.validMask, 0xFu);
  ASSERT_EQ(draw.fallbackReason, PacketFallbackReason::None);
  for (std::size_t index = 0; index < draw.vertexCount; ++index) {
    const ReplayVertex &vertex = draw.vertices[index];
    EXPECT_DOUBLE_EQ(vertex.x, sidecars[index].x + drawOffset.x);
    EXPECT_DOUBLE_EQ(vertex.y, sidecars[index].y + drawOffset.y);
    EXPECT_DOUBLE_EQ(vertex.u, static_cast<uint8_t>(tex[index]));
    EXPECT_DOUBLE_EQ(vertex.v, static_cast<uint8_t>(tex[index] >> 8));
    EXPECT_TRUE(vertex.preciseX);
    EXPECT_TRUE(vertex.preciseY);
    EXPECT_TRUE(vertex.preciseXY);
    EXPECT_TRUE(vertex.preciseUV);
    EXPECT_TRUE(vertex.rawDepthValid);
    EXPECT_TRUE(vertex.homogeneousQ);
    EXPECT_TRUE(vertex.nearSaturated);
    EXPECT_FALSE(vertex.reconciledX);
    EXPECT_FALSE(vertex.reconciledY);
    EXPECT_DOUBLE_EQ(vertex.rawDepth, 8.0);
    EXPECT_DOUBLE_EQ(vertex.nearDepth, 144.000244140625);
    EXPECT_DOUBLE_EQ(vertex.w, vertex.nearDepth);
  }

  const ReplayPrecisionCoverage coverage =
      measureReplayPrecisionCoverage(std::array{frame});
  EXPECT_EQ(coverage.projectedVertices, 4u);
  EXPECT_EQ(coverage.preciseProjectedXYVertices, 4u);
  EXPECT_EQ(coverage.validProjectedWVertices, 4u);
  EXPECT_EQ(coverage.homogeneousProjectedQVertices, 4u);
  EXPECT_EQ(coverage.nearSaturatedProjectedVertices, 4u);
  EXPECT_EQ(coverage.texturedProjectedVertices, 4u);
  EXPECT_EQ(coverage.preciseProjectedUVVertices, 4u);
  EXPECT_EQ(coverage.reconciledProjectedXVertices, 0u);
  EXPECT_EQ(coverage.reconciledProjectedYVertices, 0u);
  EXPECT_TRUE(coverage.fullyHighPrecision());

  const ReplayExecutionPlanResult plan =
      planModernReplayExecution(std::array{frame});
  ASSERT_TRUE(plan.accepted()) << replayExecutionPlanReasonName(plan.reason);
  const auto planDraw = std::find_if(
      plan.plan.operations().begin(), plan.plan.operations().end(),
      [](const ReplayExecutionOperation &operation) {
        return operation.kind == ReplayExecutionKind::DrawTranslucent;
      });
  ASSERT_NE(planDraw, plan.plan.operations().end());
  EXPECT_EQ(planDraw->draw.blendSelection, ReplayBlendSelection::SourceStp);
  EXPECT_TRUE(planDraw->draw.textured);
  EXPECT_TRUE(planDraw->access.nativeRead);
  EXPECT_TRUE(planDraw->access.colorRead);
  EXPECT_TRUE(planDraw->access.colorWrite);
  EXPECT_FALSE(planDraw->depth.test);
  EXPECT_FALSE(planDraw->depth.write);
  EXPECT_TRUE(planDraw->depth.invalidateOnAccepted);
  EXPECT_TRUE(planDraw->access.depthWrite);
  EXPECT_TRUE(planDraw->hazards.destinationFeedbackBoundary);
  EXPECT_TRUE(planDraw->hazards.destinationScratchRequired);
  EXPECT_TRUE(planDraw->hazards.refreshDestinationBetweenTriangles);
  EXPECT_TRUE(planDraw->hazards.barrierBefore);
  EXPECT_TRUE(planDraw->hazards.barrierAfter);

  VulkanReplayBakeFeatures features;
  features.allowTextured = true;
  features.allowSemiTransparent = true;
  features.allowDestinationColorFeedback = true;
  features.allowDepthInvalidate = true;
  const VulkanReplayBakeResult bake =
      bakeModernReplayVulkan(plan.plan, 4, features);
  ASSERT_TRUE(bake.accepted()) << vulkanReplayBakeReasonName(bake.reason);
  EXPECT_EQ(bake.batch.internalScale, 4u);
  ASSERT_EQ(bake.batch.vertices.size(), 6u);

  const VulkanReplayBakedDraw *bakedDraw = nullptr;
  for (const VulkanReplayBakedCommand &bakedCommand : bake.batch.commands) {
    if (const auto *candidate =
            std::get_if<VulkanReplayBakedDraw>(&bakedCommand.payload)) {
      ASSERT_EQ(bakedDraw, nullptr);
      bakedDraw = candidate;
    }
  }
  ASSERT_NE(bakedDraw, nullptr);
  EXPECT_EQ(bakedDraw->firstVertex, 0u);
  EXPECT_EQ(bakedDraw->vertexCount, 6u);
  EXPECT_EQ(bakedDraw->depthPath,
            VulkanReplayDrawDepthPath::InvalidateAcceptedFar);
  EXPECT_EQ(bakedDraw->feedbackPlanes,
            vulkanReplayFeedbackPlaneBit(VulkanReplayFeedbackPlane::Color));
  EXPECT_EQ(bakedDraw->uniforms.textureState[2], shaderDrawFlags(draw));
  ASSERT_EQ(bakedDraw->segmentCount, 2u);
  EXPECT_EQ(bakedDraw->segments[0].firstVertex, 0u);
  EXPECT_EQ(bakedDraw->segments[0].vertexCount, 3u);
  EXPECT_EQ(bakedDraw->segments[0].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Initial);
  EXPECT_GT(bakedDraw->segments[0].destinationSnapshot.width, 0u);
  EXPECT_GT(bakedDraw->segments[0].destinationSnapshot.height, 0u);
  EXPECT_EQ(bakedDraw->segments[1].firstVertex, 3u);
  EXPECT_EQ(bakedDraw->segments[1].vertexCount, 3u);
  EXPECT_EQ(bakedDraw->segments[1].snapshotAction,
            VulkanReplayDestinationSnapshotAction::Refresh);

  constexpr std::array<uint8_t, 6> flattened = {0, 1, 2, 1, 2, 3};
  for (std::size_t index = 0; index < flattened.size(); ++index) {
    const ReplayVertex &source = draw.vertices[flattened[index]];
    const vulkan_abi::Vertex &bakedVertex = bake.batch.vertices[index];
    EXPECT_FLOAT_EQ(bakedVertex.positionNative[0],
                    static_cast<float>(source.x));
    EXPECT_FLOAT_EQ(bakedVertex.positionNative[1],
                    static_cast<float>(source.y));
    EXPECT_FLOAT_EQ(bakedVertex.projectiveW,
                    static_cast<float>(source.nearDepth));
    EXPECT_FLOAT_EQ(bakedVertex.uv[0], static_cast<float>(source.u));
    EXPECT_FLOAT_EQ(bakedVertex.uv[1], static_cast<float>(source.v));
  }
}

TEST(GpuModernReplayCaptureTest,
     HybridDepthOnlyActivatesForFullyValidOpaqueProjected3D) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  constexpr std::array<uint32_t, 3> packed = {xy(10, 10), xy(20, 10),
                                              xy(10, 20)};
  const std::array<PGXPValue, 3> precise = {
      preciseVertex(packed[0], 10.25f, 10.25f, 32768.0),
      preciseVertex(packed[1], 20.25f, 10.25f, 32768.0),
      preciseVertex(packed[2], 10.25f, 20.25f, 32768.0),
  };
  const PGXPValue invalid = xyOnlyVertex(packed[2], 10.25f, 20.25f);
  std::array<PGXPValue, 3> mismatched = {
      nearClippedVertex(packed[0], 32768.0, 1.0),
      nearClippedVertex(packed[1], 32768.0, 1.0),
      nearClippedVertex(packed[2], 32768.0, 1.0),
  };
  ++mismatched[2].projectionContext;

  const auto submitPacket =
      [&](uint8_t opcode, const std::array<const PGXPValue *, 3> &sidecars) {
        gpu.writeGP0(command(opcode, 255, 255, 255));
        for (std::size_t i = 0; i < packed.size(); ++i)
          gpu.writeGP0(packed[i], UINT32_MAX, sidecars[i]);
      };
  submitPacket(0x20, {&precise[0], &precise[1], &precise[2]});
  submitPacket(0x22, {&precise[0], &precise[1], &precise[2]});
  submitPacket(0x20, {&precise[0], &precise[1], &invalid});
  submitPacket(0x20, {&mismatched[0], &mismatched[1], &mismatched[2]});

  gpu.snapshotDisplayBuffer(42);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 4u);

  const DepthState &opaque = draws[0]->state.depth;
  EXPECT_FALSE(opaque.testEnabled);
  EXPECT_TRUE(opaque.writeEnabled);
  EXPECT_EQ(opaque.compare, DepthCompare::Always);

  const DepthState &semi = draws[1]->state.depth;
  EXPECT_TRUE(draws[1]->state.blend.enabled);
  EXPECT_FALSE(semi.testEnabled);
  EXPECT_FALSE(semi.writeEnabled);
  EXPECT_EQ(semi.compare, DepthCompare::Always);
  EXPECT_TRUE(semi.invalidateOnAccepted);

  EXPECT_EQ(draws[2]->validMask, 0x3u);
  EXPECT_EQ(draws[2]->fallbackReason,
            PacketFallbackReason::InvalidWPacketAtomic);
  const DepthState &invalidDepth = draws[2]->state.depth;
  EXPECT_FALSE(invalidDepth.testEnabled);
  EXPECT_FALSE(invalidDepth.writeEnabled);
  EXPECT_EQ(invalidDepth.compare, DepthCompare::Always);
  for (std::size_t i = 0; i < draws[2]->vertexCount; ++i)
    EXPECT_DOUBLE_EQ(draws[2]->vertices[i].w, CRASH_REPLAY_FALLBACK_W);

  EXPECT_EQ(draws[3]->validMask, 0x7u);
  EXPECT_EQ(draws[3]->fallbackReason,
            PacketFallbackReason::ProjectiveContextMismatch);
  const DepthState &mismatchDepth = draws[3]->state.depth;
  EXPECT_FALSE(mismatchDepth.testEnabled);
  EXPECT_FALSE(mismatchDepth.writeEnabled);
  EXPECT_EQ(mismatchDepth.compare, DepthCompare::Always);

  const std::array<CrashReplayFramePtr, 1> batch{frame};
  const ReplayExecutionPlanResult plan = planModernReplayExecution(batch);
  EXPECT_TRUE(plan.accepted()) << replayExecutionPlanReasonName(plan.reason);
}

TEST(GpuModernReplayCaptureTest,
     StrictInvalidWFarWriteRemainsDiagnosticAndPlannerRejectsIt) {
  GPU gpu;
  GPU::ReplayConfig config = captureConfig(true);
  config.trueDepthMode = ReplayDepthMode::Strict;
  gpu.configureReplay(config);

  constexpr std::array<uint32_t, 3> packed = {xy(10, 10), xy(20, 10),
                                              xy(10, 20)};
  const std::array<PGXPValue, 3> sidecars = {
      preciseVertex(packed[0], 10.25f, 10.25f, 32768.0),
      preciseVertex(packed[1], 20.25f, 10.25f, 32768.0),
      xyOnlyVertex(packed[2], 10.25f, 20.25f),
  };
  gpu.writeGP0(command(0x20, 255, 255, 255));
  for (std::size_t i = 0; i < packed.size(); ++i)
    gpu.writeGP0(packed[i], UINT32_MAX, &sidecars[i]);

  gpu.snapshotDisplayBuffer(43);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0]->fallbackReason,
            PacketFallbackReason::InvalidWPacketAtomic);
  EXPECT_FALSE(draws[0]->state.depth.testEnabled);
  EXPECT_TRUE(draws[0]->state.depth.writeEnabled);
  EXPECT_EQ(draws[0]->state.depth.compare, DepthCompare::Always);

  const std::array<CrashReplayFramePtr, 1> batch{frame};
  const ReplayExecutionPlanResult plan = planModernReplayExecution(batch);
  EXPECT_EQ(plan.reason, ReplayExecutionPlanReason::DonorDepthStateMismatch);
  EXPECT_TRUE(plan.plan.operations().empty());
}

TEST(GpuModernReplayCaptureTest,
     RoutesExactDisplayAnchoredCompositeAsScreen2DWithDepthInvalidation) {
  GPU gpu;
  // Enabling PGXP capture must still leave semantic 2D vertices in their
  // exact packed coordinate domain.
  gpu.configureReplay(captureConfig(true));

  gpu.writeGP1(0x0800000Au);
  gpu.writeGP1(0x06000000u | 608u | (688u << 12));
  gpu.writeGP1(0x07000000u | 37u | (53u << 10));
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 15u | (15u << 10));
  gpu.writeGP0(0xE5000000u);
  submit(gpu, {command(0x2A, 16, 16, 16), xy(0, 0), xy(16, 0), xy(0, 16),
               xy(16, 16)});

  gpu.snapshotDisplayBuffer(43);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  EXPECT_EQ(draw.route, PacketRoute::Screen2D);
  EXPECT_EQ(draw.semanticReason,
            PacketSemanticReason::DisplayAnchoredComposite);
  EXPECT_EQ(draw.fallbackReason, PacketFallbackReason::None);
  EXPECT_EQ(draw.validMask, 0u);
  EXPECT_FALSE(draw.state.depth.testEnabled);
  EXPECT_FALSE(draw.state.depth.writeEnabled);
  EXPECT_TRUE(draw.state.depth.invalidateOnAccepted);
  for (std::size_t i = 0; i < draw.vertexCount; ++i) {
    EXPECT_DOUBLE_EQ(draw.vertices[i].w, CRASH_REPLAY_FALLBACK_W);
    EXPECT_FALSE(draw.vertices[i].preciseXY);
    EXPECT_DOUBLE_EQ(draw.vertices[i].x, draw.vertices[i].nativeX);
    EXPECT_DOUBLE_EQ(draw.vertices[i].y, draw.vertices[i].nativeY);
  }
}

TEST(
    GpuModernReplayCaptureTest,
    CrashPageOverlaysSpanWideCanvasWhileNativeRasterAndTexturedPanelsStayUnchanged) {
  struct ProfileScope {
    ProfileScope() {
      ps1::games::crash3::configure({.enabled = true, .widescreen16x9 = true});
    }
    ~ProfileScope() { ps1::games::crash3::configure({}); }
  } profile;
  for (int left : {0, 512}) {
    for (int top : {0, 256}) {
      SCOPED_TRACE(left);
      SCOPED_TRACE(top);
      GPU reference;
      GPU captured;
      captured.configureReplay(captureConfig(true));
      for (GPU *gpu : {&reference, &captured}) {
        gpu->writeGP1(0x0800000Au); // PAL, 512-wide programmed canvas
        gpu->writeGP1(0x06000000u | 608u | (3168u << 12));
        gpu->writeGP1(0x07000000u | 37u | (293u << 10));
        gpu->writeGP1(0x05000000u | static_cast<uint32_t>(left) |
                      (static_cast<uint32_t>(top) << 10));
      }
      const std::vector<uint32_t> setup{
          0xE3000000u | static_cast<uint32_t>(left) |
              (static_cast<uint32_t>(top) << 10),
          0xE4000000u | static_cast<uint32_t>(left + 511) |
              (static_cast<uint32_t>(top + 255) << 10),
          0xE5000000u | static_cast<uint32_t>(left) |
              (static_cast<uint32_t>(top) << 11)};
      submit(reference, setup);
      submit(captured, setup);
      for (uint32_t blend = 0; blend < 4; ++blend) {
        reference.writeGP0(0xE1000000u | (blend << 5));
        captured.writeGP0(0xE1000000u | (blend << 5));
        const std::vector<uint32_t> overlay{command(0x2A, 78, 78, 78), xy(0, 0),
                                            xy(512, 0), xy(0, 256),
                                            xy(512, 256)};
        submit(reference, overlay);
        submit(captured, overlay);
      }
      const std::vector<uint32_t> panel{command(0x2E, 128, 128, 128),
                                        xy(0, 0),
                                        uv(0, 0),
                                        xy(512, 0),
                                        uv(255, 0, 0x100),
                                        xy(0, 256),
                                        uv(0, 255),
                                        xy(512, 256),
                                        uv(255, 255)};
      submit(reference, panel);
      submit(captured, panel);
      EXPECT_TRUE(sameNativeVram(reference, captured));
      captured.snapshotDisplayBuffer(43);
      const auto frame = captured.tryConsumeReplay();
      ASSERT_TRUE(frame);
      const auto draws = drawsIn(*frame);
      ASSERT_EQ(draws.size(), 5u);
      for (std::size_t index = 0; index < 4; ++index) {
        const auto &draw = *draws[index];
        EXPECT_EQ(draw.route, PacketRoute::Screen2D);
        EXPECT_DOUBLE_EQ(draw.vertices[0].x, left);
        EXPECT_DOUBLE_EQ(draw.vertices[1].x, left + 512);
        EXPECT_DOUBLE_EQ(draw.vertices[2].y, top + 256);
        EXPECT_TRUE(draw.state.depth.invalidateOnAccepted);
      }
      EXPECT_EQ(draws[4]->route, PacketRoute::Screen2D);
      EXPECT_TRUE(draws[4]->textured);
      EXPECT_DOUBLE_EQ(draws[4]->vertices[0].x,
                       ps1::games::crash3::screen2DPresentationX(
                           left, left, left + 511, left));
    }
  }
}

TEST(GpuModernReplayCaptureTest,
     CrashPageOverlayRequiresExactCompleteQuadTopology) {
  using Quad = std::array<std::array<int32_t, 2>, 4>;
  const Quad complete{{{0, 0}, {512, 0}, {0, 256}, {512, 256}}};
  EXPECT_TRUE(ps1::games::crash3::isFullPageScreenQuad(complete));
  auto partial = complete;
  partial[1][0] = 511;
  EXPECT_FALSE(ps1::games::crash3::isFullPageScreenQuad(partial));
  auto duplicate = complete;
  duplicate[1] = duplicate[0];
  EXPECT_FALSE(ps1::games::crash3::isFullPageScreenQuad(duplicate));
  auto overlapping = complete;
  std::swap(overlapping[2], overlapping[3]);
  EXPECT_FALSE(ps1::games::crash3::isFullPageScreenQuad(overlapping));
  auto offset = complete;
  for (auto &v : offset)
    ++v[0];
  EXPECT_FALSE(ps1::games::crash3::isFullPageScreenQuad(offset));
}

TEST(GpuModernReplayCaptureTest,
     RoutesTexturedTransitionPanelAsDisplayAnchoredScreen2D) {
  GPU gpu;
  gpu.configureReplay(captureConfig(true));

  gpu.writeGP1(0x0800000Au);
  gpu.writeGP1(0x06000000u | 608u | (688u << 12));
  gpu.writeGP1(0x07000000u | 37u | (53u << 10));
  gpu.writeGP1(0x05000000u);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 15u | (15u << 10));
  gpu.writeGP0(0xE5000000u);

  constexpr uint16_t direct15Page = 0x100;
  submit(gpu, {command(0x2E, 128, 128, 128), xy(8, 8), uv(0, 0), xy(-1, 9),
               uv(15, 0, direct15Page), xy(7, -1), uv(0, 15), xy(-2, 0),
               uv(15, 15)});

  gpu.snapshotDisplayBuffer(43);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  EXPECT_EQ(draw.route, PacketRoute::Screen2D);
  EXPECT_EQ(draw.semanticReason,
            PacketSemanticReason::DisplayAnchoredComposite);
  EXPECT_EQ(draw.fallbackReason, PacketFallbackReason::None);
  EXPECT_TRUE(draw.textured);
  EXPECT_TRUE(draw.nativeRead.required);
  EXPECT_EQ(draw.validMask, 0u);
  EXPECT_FALSE(draw.state.depth.testEnabled);
  EXPECT_FALSE(draw.state.depth.writeEnabled);
  EXPECT_TRUE(draw.state.depth.invalidateOnAccepted);
  for (std::size_t i = 0; i < draw.vertexCount; ++i) {
    EXPECT_DOUBLE_EQ(draw.vertices[i].w, CRASH_REPLAY_FALLBACK_W);
    EXPECT_FALSE(draw.vertices[i].preciseXY);
    EXPECT_DOUBLE_EQ(draw.vertices[i].x, draw.vertices[i].nativeX);
    EXPECT_DOUBLE_EQ(draw.vertices[i].y, draw.vertices[i].nativeY);
  }
}

TEST(GpuModernReplayCaptureTest,
     CapturesObservedNativeMutationFamiliesInOrderWithoutChangingRaster) {
  GPU reference;
  GPU captured;
  captured.configureReplay(captureConfig());

  const auto submitBoth = [&](const std::vector<uint32_t> &words) {
    submit(reference, words);
    submit(captured, words);
  };

  // GP0(02) aligns X, rounds width to 16 and ignores E6. A zero dimension is
  // a represented command-level no-op, not an incomplete replay packet.
  submitBoth({command(0x02, 255, 0, 0), xy(18, 20), xy(2, 1)});
  submitBoth({command(0x02, 1, 2, 3), xy(512, 12), xy(512, 0)});

  // Crash 3's observed GP0(68) points share mask/depth state and coalesce.
  reference.writeGP0(0xE6000003u);
  captured.writeGP0(0xE6000003u);
  submitBoth({command(0x68, 0, 255, 0), xy(30, 30)});
  submitBoth({command(0x68, 0, 255, 0), xy(31, 30)});
  reference.writeGP0(0xE6000000u);
  captured.writeGP0(0xE6000000u);

  submitBoth({0x9F000000u, xy(16, 20), xy(40, 20), xy(2, 1)});
  submitBoth({0xBF000000u, xy(50, 50), xy(3, 1)});
  reference.writeGP0(0x22221111u);
  captured.writeGP0(0x22221111u);
  reference.writeGP0(0xDEAD3333u);
  captured.writeGP0(0xDEAD3333u);

  EXPECT_TRUE(sameNativeVram(reference, captured));
  captured.snapshotDisplayBuffer(50);
  const CrashReplayFramePtr frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);
  EXPECT_EQ(frame->latch().sequenceBegin, 0u);
  EXPECT_EQ(frame->latch().sequenceEnd, 8u);

  ASSERT_EQ(frame->operations().size(), 9u);
  EXPECT_TRUE(std::holds_alternative<SurfaceSeed>(frame->operations()[0]));
  ASSERT_TRUE(std::holds_alternative<FillRect>(frame->operations()[1]));
  ASSERT_TRUE(std::holds_alternative<PaintRect>(frame->operations()[2]));
  ASSERT_TRUE(std::holds_alternative<NativeTileUpload>(frame->operations()[3]));
  ASSERT_TRUE(std::holds_alternative<CopyRect>(frame->operations()[4]));
  ASSERT_TRUE(std::holds_alternative<CpuUpload>(frame->operations()[5]));
  ASSERT_TRUE(std::holds_alternative<CpuUpload>(frame->operations()[6]));
  ASSERT_TRUE(std::holds_alternative<CpuUpload>(frame->operations()[7]));
  EXPECT_TRUE(std::holds_alternative<FrameLatch>(frame->operations()[8]));

  const FillRect &fill = std::get<FillRect>(frame->operations()[1]);
  EXPECT_EQ(fill.destination, (NativeRect{16, 20, 16, 1}));
  EXPECT_EQ(fill.color, (Rgb8{255, 0, 0}));
  EXPECT_EQ(fill.nativeColor, 0x001Fu);

  const PaintRect &points = std::get<PaintRect>(frame->operations()[2]);
  EXPECT_EQ(points.destination, (NativeRect{30, 30, 2, 1}));
  EXPECT_EQ(points.nativeColor, 0x03E0u);
  EXPECT_EQ(points.color, (Rgb8{0, 255, 0}));
  EXPECT_EQ(points.mask, (MaskState{true, true}));
  EXPECT_TRUE(points.depth.invalidateOnAccepted);
  EXPECT_EQ(points.depth.epoch, 1u);
  EXPECT_EQ(points.coalescedCommandCount, 2u);

  const NativeTileUpload &copySource =
      std::get<NativeTileUpload>(frame->operations()[3]);
  EXPECT_EQ(copySource.destination, (NativeRect{16, 16, 8, 8}));
  EXPECT_EQ(copySource.nativeRevision, 2u);

  const CopyRect &copy = std::get<CopyRect>(frame->operations()[4]);
  EXPECT_EQ(copy.source, (NativePoint{16, 20}));
  EXPECT_EQ(copy.destination, (NativePoint{40, 20}));
  EXPECT_EQ(copy.width, 2u);
  EXPECT_EQ(copy.height, 1u);
  EXPECT_TRUE(copy.nativeRead.required);
  EXPECT_EQ(copy.nativeRead.tileCount, 1u);
  EXPECT_EQ(
      copy.nativeRead.versionHash,
      appendNativeReadDependencyHash(CRASH_REPLAY_READ_HASH_OFFSET, 258, 2));
  EXPECT_EQ(copy.mask, MaskState{});

  const CpuUpload &begin = std::get<CpuUpload>(frame->operations()[5]);
  const CpuUpload &upload = std::get<CpuUpload>(frame->operations()[6]);
  const CpuUpload &complete = std::get<CpuUpload>(frame->operations()[7]);
  EXPECT_EQ(begin.phase, CpuUploadPhase::Begin);
  EXPECT_EQ(begin.transactionId, 7u);
  EXPECT_EQ(upload.phase, CpuUploadPhase::Data);
  EXPECT_EQ(upload.transactionId, 7u);
  EXPECT_EQ(upload.transfer, (NativeRect{50, 50, 3, 1}));
  EXPECT_EQ(upload.pixelOffset, 0u);
  EXPECT_EQ(upload.pixels, (std::vector<uint16_t>{0x1111, 0x2222, 0x3333}));
  EXPECT_EQ(upload.acceptedBits, (std::vector<uint64_t>{0b111}));
  EXPECT_EQ(upload.mask, MaskState{});
  EXPECT_EQ(complete.phase, CpuUploadPhase::Complete);
  EXPECT_EQ(complete.transactionId, 7u);
  EXPECT_EQ(complete.pixelOffset, 3u);

  EXPECT_EQ(captured.getVRAM()[30 * GPU::VRAM_WIDTH + 30].raw, 0x83E0u);
  EXPECT_EQ(captured.getVRAM()[30 * GPU::VRAM_WIDTH + 31].raw, 0x83E0u);
  EXPECT_EQ(captured.getVRAM()[50 * GPU::VRAM_WIDTH + 53].raw, 0u)
      << "the high halfword of the odd A0 tail is transport padding";
}

TEST(GpuModernReplayCaptureTest,
     CapturesEveryLineRectangleAndSpriteAliasWithoutWholeVramFallback) {
  GPU reference;
  GPU captured;
  captured.configureReplay(captureConfig());

  const auto submitBoth = [&](const std::vector<uint32_t> &words) {
    submit(reference, words);
    submit(captured, words);
  };
  for (uint16_t rawOpcode = 0x40; rawOpcode <= 0x7F; ++rawOpcode) {
    const uint8_t opcode = static_cast<uint8_t>(rawOpcode);
    const uint32_t head = command(opcode, 80, 120, 160);
    if (opcode <= 0x47) {
      submitBoth({head, xy(10, 10), xy(12, 10)});
    } else if (opcode <= 0x4F) {
      submitBoth({head, xy(10, 12), xy(12, 12), xy(12, 14), 0x50005000u});
    } else if (opcode <= 0x57) {
      submitBoth({head, xy(14, 10), rgb(160, 120, 80), xy(16, 10)});
    } else if (opcode <= 0x5F) {
      submitBoth({head, xy(14, 12), rgb(160, 120, 80), xy(16, 12),
                  rgb(40, 80, 200), xy(16, 14), 0x50005000u});
    } else if (opcode <= 0x63) {
      submitBoth({head, xy(20, 10), xy(2, 2)});
    } else if (opcode <= 0x67) {
      submitBoth({head, xy(24, 10), uv(10, 10, 0), xy(2, 2)});
    } else if (opcode <= 0x6B) {
      submitBoth({head, xy(28, 10)});
    } else if (opcode <= 0x6F) {
      submitBoth({head, xy(30, 10), uv(10, 10, 0)});
    } else if (opcode <= 0x73) {
      submitBoth({head, xy(32, 10)});
    } else if (opcode <= 0x77) {
      submitBoth({head, xy(42, 10), uv(10, 10, 0)});
    } else if (opcode <= 0x7B) {
      submitBoth({head, xy(52, 10)});
    } else {
      submitBoth({head, xy(70, 10), uv(10, 10, 0)});
    }
  }

  EXPECT_TRUE(sameNativeVram(reference, captured));
  captured.snapshotDisplayBuffer(51);
  const CrashReplayFramePtr frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);
  EXPECT_EQ(frame->latch().sequenceBegin, 0u);
  EXPECT_EQ(frame->latch().sequenceEnd, 64u);
  EXPECT_FALSE(operationsOf<PaintPixels>(*frame).empty());
  ASSERT_EQ(operationsOf<PaintRect>(*frame).size(), 1u)
      << "only the allocation-free GP0(68h) point keeps PaintRect";
}

TEST(GpuModernReplayCaptureTest,
     PaintPixelsContainOnlyAcceptedE6WritesAndRetainSourceColor) {
  GPU reference;
  GPU captured;
  captured.configureReplay(captureConfig());

  const auto writeBoth = [&](uint32_t word) {
    reference.writeGP0(word);
    captured.writeGP0(word);
  };
  writeBoth(0xE6000001u);
  writeBoth(command(0x68, 255, 0, 0));
  writeBoth(xy(5, 5));
  writeBoth(0xE6000002u);
  writeBoth(command(0x40, 0, 0, 255));
  writeBoth(xy(5, 5));
  writeBoth(xy(7, 5));

  EXPECT_TRUE(sameNativeVram(reference, captured));
  captured.snapshotDisplayBuffer(52);
  const CrashReplayFramePtr frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  const auto paints = operationsOf<PaintPixels>(*frame);
  ASSERT_EQ(paints.size(), 1u);
  const PaintPixels &line = *paints.front();
  ASSERT_EQ(line.pixels.size(), 2u);
  EXPECT_EQ(line.pixels[0].destination, (NativePoint{6, 5}));
  EXPECT_EQ(line.pixels[1].destination, (NativePoint{7, 5}));
  EXPECT_EQ(line.pixels[0].color, (Rgb8{0, 0, 255}));
  EXPECT_EQ(line.pixels[0].nativeColor, 0x7C00u);
  EXPECT_FALSE(line.pixels[0].sourceMaskBit);
  EXPECT_EQ(line.mask, (MaskState{false, true}));
  EXPECT_FALSE(line.blend.enabled);
  EXPECT_TRUE(line.depth.invalidateOnAccepted);
}

TEST(GpuModernReplayCaptureTest,
     TransferExtentsMaskUpperBitsAndExpandZeroFieldsWithoutRasterDrift) {
  GPU reference;
  GPU captured;
  captured.configureReplay(captureConfig());

  const auto submitBoth = [&](const std::vector<uint32_t> &words) {
    submit(reference, words);
    submit(captured, words);
  };

  constexpr uint32_t highBitSize = (0x201u << 16) | 0x403u;
  submitBoth({0xBF000000u, xy(100, 100), highBitSize});
  reference.writeGP0(0x22221111u);
  captured.writeGP0(0x22221111u);
  reference.writeGP0(0xDEAD3333u);
  captured.writeGP0(0xDEAD3333u);

  // A zero width expands to one complete 1024-pixel row.
  submitBoth({0xA0000000u, xy(0, 120), xy(0, 1)});
  for (uint32_t word = 0; word < GPU::VRAM_WIDTH / 2u; ++word) {
    reference.writeGP0(0x13571357u);
    captured.writeGP0(0x13571357u);
  }

  submitBoth({0x9F000000u, xy(100, 100), xy(110, 100), highBitSize});
  // A zero height expands to all 512 rows.
  submitBoth({0x80000000u, xy(100, 0), xy(101, 0), xy(1, 0)});

  EXPECT_TRUE(sameNativeVram(reference, captured));
  captured.snapshotDisplayBuffer(55);
  const CrashReplayFramePtr frame = captured.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);

  const auto uploads = operationsOf<CpuUpload>(*frame);
  ASSERT_EQ(uploads.size(), 6u);
  EXPECT_EQ(uploads[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(uploads[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(uploads[2]->phase, CpuUploadPhase::Complete);
  EXPECT_EQ(uploads[0]->transactionId, 0u);
  EXPECT_EQ(uploads[1]->transfer, (NativeRect{100, 100, 3, 1}));
  EXPECT_EQ(uploads[1]->pixels,
            (std::vector<uint16_t>{0x1111, 0x2222, 0x3333}));
  EXPECT_EQ(uploads[3]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(uploads[4]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(uploads[5]->phase, CpuUploadPhase::Complete);
  EXPECT_EQ(uploads[3]->transactionId, 1u);
  EXPECT_EQ(uploads[4]->transfer, (NativeRect{0, 120, 1024, 1}));
  EXPECT_EQ(uploads[4]->pixels.size(), 1024u);

  const auto copies = operationsOf<CopyRect>(*frame);
  ASSERT_EQ(copies.size(), 2u);
  EXPECT_EQ(copies[0]->width, 3u);
  EXPECT_EQ(copies[0]->height, 1u);
  EXPECT_EQ(copies[1]->width, 1u);
  EXPECT_EQ(copies[1]->height, 512u);
}

TEST(GpuModernReplayCaptureTest,
     IdentityCopyIsNoOpUnlessE6ForcesMaskAndRevisionAdvance) {
  constexpr std::size_t pixelCount =
      static_cast<std::size_t>(GPU::VRAM_WIDTH) * GPU::VRAM_HEIGHT;
  std::vector<uint16_t> initial(pixelCount, 0);
  initial[0] = 0x001Fu;

  const std::vector<uint32_t> textured = {command(0x24, 128, 128, 128),
                                          xy(100, 100),
                                          uv(0, 0, 0),
                                          xy(108, 100),
                                          uv(0, 0, 0x100),
                                          xy(100, 108),
                                          uv(0, 0)};

  {
    GPU reference;
    GPU captured;
    reference.loadVram(reinterpret_cast<const uint8_t *>(initial.data()));
    captured.loadVram(reinterpret_cast<const uint8_t *>(initial.data()));
    captured.configureReplay(captureConfig());

    submit(reference, {0x9F000000u, xy(0, 0), xy(0, 0), xy(1, 1)});
    submit(captured, {0x9F000000u, xy(0, 0), xy(0, 0), xy(1, 1)});
    submit(reference, textured);
    submit(captured, textured);
    EXPECT_TRUE(sameNativeVram(reference, captured));

    captured.snapshotDisplayBuffer(56);
    const CrashReplayFramePtr frame = captured.tryConsumeReplay();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->latch().complete);
    EXPECT_EQ(frame->latch().unsupportedCount, 0u);
    EXPECT_TRUE(operationsOf<CopyRect>(*frame).empty());
    EXPECT_TRUE(operationsOf<NativeTileUpload>(*frame).empty())
        << "an identity copy must not advance a native tile revision";
    ASSERT_EQ(drawsIn(*frame).size(), 1u);
  }

  {
    GPU reference;
    GPU captured;
    reference.loadVram(reinterpret_cast<const uint8_t *>(initial.data()));
    captured.loadVram(reinterpret_cast<const uint8_t *>(initial.data()));
    captured.configureReplay(captureConfig());

    reference.writeGP0(0xE6000001u);
    captured.writeGP0(0xE6000001u);
    submit(reference, {0x80000000u, xy(0, 0), xy(0, 0), xy(1, 1)});
    submit(captured, {0x80000000u, xy(0, 0), xy(0, 0), xy(1, 1)});
    reference.writeGP0(0xE6000000u);
    captured.writeGP0(0xE6000000u);
    submit(reference, textured);
    submit(captured, textured);
    EXPECT_TRUE(sameNativeVram(reference, captured));
    EXPECT_EQ(captured.getVRAM()[0].raw, 0x801Fu);

    captured.snapshotDisplayBuffer(57);
    const CrashReplayFramePtr frame = captured.tryConsumeReplay();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->latch().complete);
    EXPECT_EQ(frame->latch().unsupportedCount, 0u);
    const auto copies = operationsOf<CopyRect>(*frame);
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_TRUE(copies.front()->mask.forceMaskBit);
    const auto tiles = operationsOf<NativeTileUpload>(*frame);
    ASSERT_EQ(tiles.size(), 1u);
    EXPECT_EQ(tiles.front()->destination, (NativeRect{0, 0, 8, 8}));
    EXPECT_EQ(tiles.front()->nativeRevision, 2u);
    ASSERT_EQ(drawsIn(*frame).size(), 1u);
  }
}

TEST(GpuModernReplayCaptureTest,
     CpuUploadKeepsExactMaskAndRevisionsAcrossMidVblankOddTail) {
  constexpr std::size_t nativeBytes =
      static_cast<std::size_t>(GPU::VRAM_WIDTH) * GPU::VRAM_HEIGHT *
      sizeof(ps1::gpu::Color16);
  std::vector<uint8_t> initial(nativeBytes, 0);
  const std::size_t maskedPixel = 50u * GPU::VRAM_WIDTH + 50u;
  initial[maskedPixel * 2u] = 0x01;
  initial[maskedPixel * 2u + 1u] = 0x80;

  GPU reference;
  GPU captured;
  reference.loadVram(initial.data());
  captured.loadVram(initial.data());
  captured.configureReplay(captureConfig());

  reference.writeGP0(0xE6000003u);
  captured.writeGP0(0xE6000003u);
  submit(reference, {0xA0000000u, xy(50, 50), xy(3, 1)});
  submit(captured, {0xA0000000u, xy(50, 50), xy(3, 1)});
  reference.writeGP0(0x22221111u);
  captured.writeGP0(0x22221111u);
  EXPECT_TRUE(sameNativeVram(reference, captured));

  captured.snapshotDisplayBuffer(60);
  const CrashReplayFramePtr first = captured.tryConsumeReplay();
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->latch().complete);
  EXPECT_EQ(first->latch().unsupportedCount, 0u);
  EXPECT_EQ(first->latch().sequenceBegin, 0u);
  EXPECT_EQ(first->latch().sequenceEnd, 2u);
  const auto firstChunks = operationsOf<CpuUpload>(*first);
  ASSERT_EQ(firstChunks.size(), 2u);
  EXPECT_EQ(firstChunks[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(firstChunks[0]->transactionId, 1u);
  EXPECT_EQ(firstChunks[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(firstChunks[1]->transactionId, 1u);
  EXPECT_EQ(firstChunks[1]->transfer, (NativeRect{50, 50, 3, 1}));
  EXPECT_EQ(firstChunks[1]->pixelOffset, 0u);
  EXPECT_EQ(firstChunks[1]->pixels, (std::vector<uint16_t>{0x1111, 0x2222}));
  EXPECT_EQ(firstChunks[1]->acceptedBits, (std::vector<uint64_t>{0b10}));
  EXPECT_EQ(firstChunks[1]->mask, (MaskState{true, true}));

  reference.writeGP0(0xDEAD3333u);
  captured.writeGP0(0xDEAD3333u);
  EXPECT_TRUE(sameNativeVram(reference, captured));
  EXPECT_EQ(captured.getVRAM()[maskedPixel].raw, 0x8001u);
  EXPECT_EQ(captured.getVRAM()[maskedPixel + 1u].raw, 0xA222u);
  EXPECT_EQ(captured.getVRAM()[maskedPixel + 2u].raw, 0xB333u);
  EXPECT_EQ(captured.getVRAM()[maskedPixel + 3u].raw, 0u);

  captured.snapshotDisplayBuffer(61);
  const CrashReplayFramePtr second = captured.tryConsumeReplay();
  ASSERT_TRUE(second);
  ASSERT_TRUE(second->latch().complete);
  EXPECT_EQ(second->latch().unsupportedCount, 0u);
  EXPECT_EQ(second->latch().sequenceBegin, 2u);
  EXPECT_EQ(second->latch().sequenceEnd, 2u);
  const auto secondChunks = operationsOf<CpuUpload>(*second);
  ASSERT_EQ(secondChunks.size(), 2u);
  EXPECT_EQ(secondChunks[0]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(secondChunks[0]->transactionId, 1u);
  EXPECT_EQ(secondChunks[0]->pixelOffset, 2u);
  EXPECT_EQ(secondChunks[0]->pixels, (std::vector<uint16_t>{0x3333}));
  EXPECT_EQ(secondChunks[0]->acceptedBits, (std::vector<uint64_t>{1}));
  EXPECT_EQ(secondChunks[1]->phase, CpuUploadPhase::Complete);
  EXPECT_EQ(secondChunks[1]->transactionId, 1u);
  EXPECT_EQ(secondChunks[1]->pixelOffset, 3u);

  // A following native texture read must upload the tile at the revision of
  // the second accepted chunk, proving revisions advance after each write.
  reference.writeGP0(0xE6000000u);
  captured.writeGP0(0xE6000000u);
  const std::vector<uint32_t> textured = {command(0x24, 128, 128, 128),
                                          xy(100, 100),
                                          uv(50, 50, 0),
                                          xy(108, 100),
                                          uv(57, 50, 0x100),
                                          xy(100, 108),
                                          uv(50, 57)};
  submit(reference, textured);
  submit(captured, textured);
  EXPECT_TRUE(sameNativeVram(reference, captured));
  captured.snapshotDisplayBuffer(62);
  const CrashReplayFramePtr textureFrame = captured.tryConsumeReplay();
  ASSERT_TRUE(textureFrame);
  ASSERT_TRUE(textureFrame->latch().complete);
  bool foundLatestTile = false;
  for (const NativeTileUpload *tile :
       operationsOf<NativeTileUpload>(*textureFrame)) {
    if (tile->destination == NativeRect{48, 48, 8, 8}) {
      foundLatestTile = true;
      EXPECT_EQ(tile->nativeRevision, 3u);
    }
  }
  EXPECT_TRUE(foundLatestTile);
}

TEST(GpuModernReplayCaptureTest,
     CommandResetsAndLoadKeepInterruptedCpuUploadTimelineCoherent) {
  GPU gpu;
  gpu.configureReplay(captureConfig());

  const auto beginPartialUpload = [&](int x) {
    submit(gpu, {0xA0000000u, xy(x, 80), xy(4, 1)});
    gpu.writeGP0(0x22221111u);
  };
  const auto consumeCompleteFrame = [&](uint64_t frameNumber) {
    gpu.snapshotDisplayBuffer(frameNumber);
    CrashReplayFramePtr frame = gpu.tryConsumeReplay();
    EXPECT_TRUE(frame);
    if (frame) {
      EXPECT_TRUE(frame->latch().complete);
      EXPECT_EQ(frame->latch().unsupportedCount, 0u);
    }
    return frame;
  };

  beginPartialUpload(10);
  gpu.writeGP1(0x01000000u);
  const CrashReplayFramePtr commandReset = consumeCompleteFrame(70);
  ASSERT_TRUE(commandReset);
  const auto commandResetUploads = operationsOf<CpuUpload>(*commandReset);
  ASSERT_EQ(commandResetUploads.size(), 3u);
  EXPECT_EQ(commandResetUploads[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(commandResetUploads[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(commandResetUploads[1]->pixels.size(), 2u);
  EXPECT_EQ(commandResetUploads[2]->phase, CpuUploadPhase::Abort);
  EXPECT_EQ(commandResetUploads[2]->pixelOffset, 2u);
  for (const CpuUpload *upload : commandResetUploads)
    EXPECT_EQ(upload->transactionId, 0u);
  ASSERT_EQ(operationsOf<SurfaceSeed>(*commandReset).size(), 1u);
  EXPECT_EQ(operationsOf<SurfaceSeed>(*commandReset).front()->depthEpoch, 1u);

  beginPartialUpload(20);
  std::vector<uint8_t> loaded(static_cast<std::size_t>(GPU::VRAM_WIDTH) *
                                  GPU::VRAM_HEIGHT * sizeof(ps1::gpu::Color16),
                              0x5A);
  gpu.loadVram(loaded.data());
  const CrashReplayFramePtr load = consumeCompleteFrame(71);
  ASSERT_TRUE(load);
  const auto loadUploads = operationsOf<CpuUpload>(*load);
  ASSERT_EQ(loadUploads.size(), 3u);
  EXPECT_EQ(loadUploads[0]->phase, CpuUploadPhase::Begin);
  EXPECT_EQ(loadUploads[1]->phase, CpuUploadPhase::Data);
  EXPECT_EQ(loadUploads[2]->phase, CpuUploadPhase::Abort);
  for (const CpuUpload *upload : loadUploads)
    EXPECT_EQ(upload->transactionId, 1u);
  ASSERT_EQ(operationsOf<SurfaceSeed>(*load).size(), 1u);
  EXPECT_EQ(operationsOf<SurfaceSeed>(*load).front()->depthEpoch, 2u);
  const auto &loadOperations = load->operations();
  const auto loadAbort = std::find_if(
      loadOperations.begin(), loadOperations.end(), [](const Operation &op) {
        const auto *upload = std::get_if<CpuUpload>(&op);
        return upload != nullptr && upload->phase == CpuUploadPhase::Abort;
      });
  const auto loadSeed = std::find_if(
      loadOperations.begin(), loadOperations.end(), [](const Operation &op) {
        return std::holds_alternative<SurfaceSeed>(op);
      });
  EXPECT_LT(loadAbort, loadSeed);

  beginPartialUpload(30);
  gpu.writeGP1(0x00000000u);
  const CrashReplayFramePtr softReset = consumeCompleteFrame(72);
  ASSERT_TRUE(softReset);
  const auto softResetUploads = operationsOf<CpuUpload>(*softReset);
  ASSERT_EQ(softResetUploads.size(), 3u);
  EXPECT_EQ(softResetUploads[2]->phase, CpuUploadPhase::Abort);
  for (const CpuUpload *upload : softResetUploads)
    EXPECT_EQ(upload->transactionId, 2u);
  ASSERT_EQ(operationsOf<Reset>(*softReset).size(), 1u);
  EXPECT_EQ(operationsOf<Reset>(*softReset).front()->nextDepthEpoch, 3u);
  const auto softResetAbort = std::find_if(
      softReset->operations().begin(), softReset->operations().end(),
      [](const Operation &op) {
        const auto *upload = std::get_if<CpuUpload>(&op);
        return upload != nullptr && upload->phase == CpuUploadPhase::Abort;
      });
  const auto softResetOperation = std::find_if(
      softReset->operations().begin(), softReset->operations().end(),
      [](const Operation &op) { return std::holds_alternative<Reset>(op); });
  EXPECT_LT(softResetAbort, softResetOperation);

  beginPartialUpload(40);
  gpu.reset();
  const CrashReplayFramePtr powerReset = consumeCompleteFrame(73);
  ASSERT_TRUE(powerReset);
  const auto powerResetUploads = operationsOf<CpuUpload>(*powerReset);
  ASSERT_EQ(powerResetUploads.size(), 3u);
  EXPECT_EQ(powerResetUploads[2]->phase, CpuUploadPhase::Abort);
  for (const CpuUpload *upload : powerResetUploads)
    EXPECT_EQ(upload->transactionId, 3u);
  ASSERT_EQ(operationsOf<SurfaceSeed>(*powerReset).size(), 1u);
  EXPECT_EQ(operationsOf<SurfaceSeed>(*powerReset).front()->depthEpoch, 4u);
}

TEST(GpuModernReplayCaptureTest,
     CpuUploadAbortIsRecordedEvenBeforeTheFirstDataPixel) {
  const auto beginWithoutPixels = [](GPU &gpu, int x) {
    submit(gpu, {0xA0000000u, xy(x, 90), xy(2, 1)});
  };
  const auto consume = [](GPU &gpu, uint64_t frameNumber) {
    gpu.snapshotDisplayBuffer(frameNumber);
    return gpu.tryConsumeReplay();
  };
  const auto expectBeginAbort = [](const CrashReplayFramePtr &frame) {
    ASSERT_TRUE(frame);
    const auto uploads = operationsOf<CpuUpload>(*frame);
    ASSERT_EQ(uploads.size(), 2u);
    EXPECT_EQ(uploads[0]->phase, CpuUploadPhase::Begin);
    EXPECT_EQ(uploads[1]->phase, CpuUploadPhase::Abort);
    EXPECT_EQ(uploads[0]->transactionId, 0u);
    EXPECT_EQ(uploads[1]->transactionId, 0u);
    EXPECT_EQ(uploads[1]->pixelOffset, 0u);
    EXPECT_TRUE(uploads[1]->pixels.empty());
  };

  {
    GPU gpu;
    gpu.configureReplay(captureConfig());
    beginWithoutPixels(gpu, 10);
    gpu.writeGP1(0x01000000u);
    expectBeginAbort(consume(gpu, 74));
  }
  {
    GPU gpu;
    gpu.configureReplay(captureConfig());
    beginWithoutPixels(gpu, 20);
    gpu.reset();
    const CrashReplayFramePtr frame = consume(gpu, 75);
    expectBeginAbort(frame);
    ASSERT_EQ(operationsOf<SurfaceSeed>(*frame).size(), 2u);
  }
  {
    GPU gpu;
    gpu.configureReplay(captureConfig());
    beginWithoutPixels(gpu, 30);
    std::vector<uint8_t> loaded(static_cast<std::size_t>(GPU::VRAM_WIDTH) *
                                    GPU::VRAM_HEIGHT *
                                    sizeof(ps1::gpu::Color16),
                                0x5A);
    gpu.loadVram(loaded.data());
    const CrashReplayFramePtr frame = consume(gpu, 76);
    expectBeginAbort(frame);
    ASSERT_EQ(operationsOf<SurfaceSeed>(*frame).size(), 2u);
  }
}

TEST(GpuModernReplayCaptureTest,
     DefaultNearSaturationIsReplayableWhileGeometricClipFallsBackAtomically) {
  constexpr std::array<uint32_t, 3> packed = {xy(40, 40), xy(48, 40),
                                              xy(40, 48)};
  const std::array<PGXPValue, 3> precision = {
      nearClippedVertex(packed[0], 0.5, 1.0),
      nearClippedVertex(packed[1], 2.0, 1.0),
      nearClippedVertex(packed[2], 2.0, 1.0)};
  const auto submitNearPacket = [&](GPU &gpu) {
    gpu.writeGP0(command(0x20, 255, 255, 255));
    for (std::size_t i = 0; i < packed.size(); ++i)
      gpu.writeGP0(packed[i], UINT32_MAX, &precision[i]);
  };

  GPU saturated;
  saturated.configureReplay(captureConfig(true));
  submitNearPacket(saturated);
  saturated.snapshotDisplayBuffer(80);
  const CrashReplayFramePtr saturatedFrame = saturated.tryConsumeReplay();
  ASSERT_TRUE(saturatedFrame);
  ASSERT_TRUE(saturatedFrame->latch().complete);
  EXPECT_EQ(saturatedFrame->latch().unsupportedCount, 0u);
  const auto saturatedDraws = drawsIn(*saturatedFrame);
  ASSERT_EQ(saturatedDraws.size(), 1u);
  EXPECT_EQ(saturatedDraws.front()->fallbackReason, PacketFallbackReason::None);
  EXPECT_EQ(saturatedDraws.front()->validMask, 0x7u);
  EXPECT_DOUBLE_EQ(saturatedDraws.front()->vertices[0].w, 1.0);
  EXPECT_DOUBLE_EQ(saturatedDraws.front()->vertices[1].w, 2.0);

  ReplayPreflightRequest geometricRequest;
  geometricRequest.expectedLatch = saturatedFrame->latch();
  geometricRequest.expectedPolicy = saturatedFrame->latch().policy;
  geometricRequest.capabilities =
      CRASH_REPLAY_V1_ALL_CAPABILITIES |
      replayCapabilityBit(ReplayCapability::NearPlaneGeometricClip);
  geometricRequest.nearClipMode = ReplayNearClipMode::GeometricClip;
  const std::array<CrashReplayFramePtr, 1> saturatedBatch{saturatedFrame};
  const ReplayPreflightResult geometricResult = preflightCrashReplay(
      saturatedBatch, geometricRequest, ReplayPreflightState{});
  EXPECT_EQ(geometricResult.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(geometricResult.reason, ReplayPreflightReason::InvalidNearClipMode);

  GPU clipped;
  clipped.configureReplay(captureConfig(true));
  clipped.setProjectiveNearClip(true);
  submitNearPacket(clipped);
  clipped.snapshotDisplayBuffer(80);
  const CrashReplayFramePtr clippedFrame = clipped.tryConsumeReplay();
  ASSERT_TRUE(clippedFrame);
  EXPECT_FALSE(clippedFrame->latch().complete);
  EXPECT_EQ(clippedFrame->latch().unsupportedCount, 1u);
  EXPECT_TRUE(drawsIn(*clippedFrame).empty());
}

TEST(GpuModernReplayCaptureTest,
     TextureDisableBitIsIgnoredWithoutGp1GateLikeCrashDrawMode) {
  GPU gpu;
  gpu.configureReplay(captureConfig());
  // Crash writes draw mode 0x0A60 without ever enabling GP1(09). Its GP0(36)
  // polygon must therefore remain a textured Gouraud packet even when the
  // embedded tpage repeats bit 11.
  constexpr uint16_t crashDrawMode = 0x0A60;
  gpu.writeGP0(0xE1000000u | crashDrawMode);
  submit(gpu, {command(0x36, 128, 64, 32), xy(20, 20), uv(0, 0, 0),
               rgb(64, 128, 32), xy(28, 20), uv(7, 0, crashDrawMode),
               rgb(32, 64, 128), xy(20, 28), uv(0, 7)});

  gpu.snapshotDisplayBuffer(90);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);
  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  EXPECT_EQ(draw.opcode, 0x36u);
  EXPECT_TRUE(draw.textured);
  EXPECT_TRUE(draw.gouraud);
  EXPECT_TRUE(draw.state.blend.enabled);
  EXPECT_TRUE(draw.state.blend.texturedStpOnly);
  EXPECT_FALSE(draw.state.texturePage.textureDisable);
  EXPECT_EQ(draw.state.texturePage.raw & (1u << 11u), 0u);
  EXPECT_TRUE(draw.state.textureUvDomain.enabled);
  EXPECT_TRUE(draw.nativeRead.required);
  EXPECT_GT(draw.nativeRead.tileCount, 0u);
  EXPECT_EQ(gpu.readGPUSTAT() & (1u << 15u), 0u);
}

TEST(GpuModernReplayCaptureTest,
     Gp1GatedTextureDisableLowersPolygonToWholePrimitiveUntexturedDraw) {
  GPU reference;
  GPU gpu;
  gpu.configureReplay(captureConfig());

  // Dirty tile zero after the SurfaceSeed. A mistakenly textured capture
  // would have to queue a NativeTileUpload before reading this page.
  submit(gpu, {command(0x02, 255, 0, 0), xy(0, 0), xy(1, 1)});
  submit(reference, {command(0x02, 255, 0, 0), xy(0, 0), xy(1, 1)});
  gpu.writeGP1(0x09000001u);
  constexpr uint16_t disabledTpage = 1u << 11u;
  submit(gpu, {command(0x26, 248, 80, 40), xy(20, 20), uv(0, 0, 0), xy(28, 20),
               uv(7, 0, disabledTpage), xy(20, 28), uv(0, 7)});
  submit(reference,
         {command(0x22, 248, 80, 40), xy(20, 20), xy(28, 20), xy(20, 28)});

  EXPECT_TRUE(sameNativeVram(gpu, reference))
      << "gated texture-disable must rasterize as the equivalent untextured "
         "semi-transparent primitive";
  EXPECT_NE(gpu.readGPUSTAT() & (1u << 15u), 0u);

  gpu.snapshotDisplayBuffer(91);
  const CrashReplayFramePtr frame = gpu.tryConsumeReplay();
  ASSERT_TRUE(frame);
  ASSERT_TRUE(frame->latch().complete);
  EXPECT_EQ(frame->latch().unsupportedCount, 0u);
  EXPECT_TRUE(operationsOf<NativeTileUpload>(*frame).empty());

  const auto draws = drawsIn(*frame);
  ASSERT_EQ(draws.size(), 1u);
  const DrawPacket &draw = *draws.front();
  EXPECT_EQ(draw.opcode, 0x26u)
      << "the encoded textured packet layout remains part of provenance";
  EXPECT_FALSE(draw.textured);
  EXPECT_FALSE(draw.rawTexture);
  EXPECT_FALSE(draw.gouraud);
  EXPECT_TRUE(draw.state.blend.enabled);
  EXPECT_FALSE(draw.state.blend.texturedStpOnly);
  EXPECT_TRUE(draw.state.texturePage.textureDisable);
  EXPECT_NE(draw.state.texturePage.raw & (1u << 11u), 0u);
  EXPECT_FALSE(draw.state.textureUvDomain.enabled);
  EXPECT_FALSE(draw.nativeRead.required);
  EXPECT_EQ(draw.nativeRead.tileCount, 0u);
  EXPECT_EQ(draw.nativeRead.versionHash, CRASH_REPLAY_READ_HASH_OFFSET);
  for (std::size_t i = 0; i < draw.vertexCount; ++i)
    EXPECT_FALSE(draw.vertices[i].preciseUV);

  const ReplayExecutionPlanResult plan =
      planModernReplayExecution(std::array{frame});
  ASSERT_TRUE(plan.accepted());
  const auto item = std::find_if(
      plan.plan.operations().begin(), plan.plan.operations().end(),
      [](const ReplayExecutionOperation &operation) {
        return operation.kind == ReplayExecutionKind::DrawTranslucent;
      });
  ASSERT_NE(item, plan.plan.operations().end());
  EXPECT_FALSE(item->draw.textured);
  EXPECT_EQ(item->draw.blendSelection, ReplayBlendSelection::WholePrimitive);
  EXPECT_FALSE(item->draw.textureZeroCanDiscard);
  EXPECT_FALSE(item->draw.sourceStpWritesMask);
  EXPECT_FALSE(item->access.nativeRead);
}

TEST(GpuModernReplayCaptureTest,
     NativeSnapshotReturnsExactFrameAcrossEmptySameRevisionVBlanks) {
  GPU gpu;
  gpu.configureReplay(captureConfig());

  // Drain the initial SurfaceSeed so both observed frames below are genuinely
  // empty latches with the same native revision and packet range.
  gpu.snapshotDisplayBuffer(98);
  ASSERT_TRUE(gpu.tryConsumeReplay());

  gpu.snapshotDisplayBuffer(99);
  std::vector<ps1::gpu::Color16> native;
  GPU::DisplayState firstState;
  CrashReplayFramePtr firstToken;
  EXPECT_EQ(gpu.copyDisplayVRAMIfChanged(native, UINT64_MAX, &firstState,
                                         &firstToken),
            GPU::DisplayCopyStatus::Copied);
  const CrashReplayFramePtr first = gpu.tryConsumeReplay();
  ASSERT_TRUE(first);
  ASSERT_TRUE(firstToken);
  EXPECT_EQ(firstToken.get(), first.get());
  ASSERT_EQ(first->operations().size(), 1u);

  gpu.snapshotDisplayBuffer(100);
  GPU::DisplayState secondState;
  CrashReplayFramePtr secondToken;
  EXPECT_EQ(gpu.copyDisplayVRAMIfChanged(native, firstState.contentRevision,
                                         &secondState, &secondToken),
            GPU::DisplayCopyStatus::Unchanged);
  const CrashReplayFramePtr second = gpu.tryConsumeReplay();
  ASSERT_TRUE(second);
  ASSERT_TRUE(secondToken);
  EXPECT_EQ(secondToken.get(), second.get());
  EXPECT_NE(secondToken.get(), firstToken.get());
  ASSERT_EQ(second->operations().size(), 1u);

  EXPECT_EQ(secondState.contentRevision, firstState.contentRevision);
  EXPECT_EQ(secondState.completedSubmissionId,
            firstState.completedSubmissionId);
  EXPECT_EQ(second->latch().sequenceBegin, first->latch().sequenceBegin);
  EXPECT_EQ(second->latch().sequenceEnd, first->latch().sequenceEnd);
}

TEST(GpuModernReplayCaptureTest,
     ProducerFramesPassPreflightAcrossUploadResetAndLoad) {
  GPU gpu;
  gpu.configureReplay(captureConfig());

  // Exercise canonical E1/E6 state and a real textured polygon before an A0
  // transaction whose data crosses the first VBlank.
  gpu.writeGP0(0xE1000100u);
  gpu.writeGP0(0xE6000000u);
  submit(gpu, {command(0x24, 128, 128, 128), xy(8, 8), uv(0, 0, 0), xy(24, 8),
               uv(15, 0, 0x100), xy(8, 24), uv(0, 15)});
  submit(gpu, {0xA0000000u, xy(40, 40), xy(4, 1)});
  gpu.writeGP0(0x22221111u);
  gpu.snapshotDisplayBuffer(1);

  const CrashReplayFramePtr first = gpu.tryConsumeReplay();
  ASSERT_TRUE(first);
  ReplayPreflightRequest request;
  request.expectedLatch = first->latch();
  request.expectedPolicy = first->latch().policy;
  request.capabilities = CRASH_REPLAY_V1_ALL_CAPABILITIES;
  const std::array<CrashReplayFramePtr, 1> firstBatch{first};

  EXPECT_EQ(first->latch().policy,
            (ReplayPolicyState{4, false, true, ReplayDepthMode::Hybrid}));
  ReplayPreflightRequest wrongScale = request;
  wrongScale.expectedPolicy.internalScale = 1;
  const ReplayPreflightResult wrongScaleResult =
      preflightCrashReplay(firstBatch, wrongScale, ReplayPreflightState{});
  EXPECT_EQ(wrongScaleResult.route, ReplayPreflightRoute::M0Fallback);
  EXPECT_EQ(wrongScaleResult.reason, ReplayPreflightReason::PolicyMismatch);

  const ReplayPreflightResult firstResult =
      preflightCrashReplay(firstBatch, request, ReplayPreflightState{});
  ASSERT_EQ(firstResult.route, ReplayPreflightRoute::M1Replay)
      << replayPreflightReasonName(firstResult.reason);
  ASSERT_TRUE(firstResult.candidateState.cpuUpload.active);
  EXPECT_EQ(firstResult.candidateState.cpuUpload.nextOffset, 2u);
  EXPECT_EQ(firstResult.candidateState.depthEpoch, 1u);

  // Complete the inherited transfer, then reset guest draw/depth state. GP1
  // does not consume a GP0 packet sequence and must still replay after A0's
  // explicit terminal phase.
  gpu.writeGP0(0x44443333u);
  gpu.writeGP1(0x00000000u);
  gpu.snapshotDisplayBuffer(2);
  const CrashReplayFramePtr second = gpu.tryConsumeReplay();
  ASSERT_TRUE(second);
  request.expectedLatch = second->latch();
  const std::array<CrashReplayFramePtr, 1> secondBatch{second};
  const ReplayPreflightResult secondResult =
      preflightCrashReplay(secondBatch, request, firstResult.candidateState);
  ASSERT_EQ(secondResult.route, ReplayPreflightRoute::M1Replay)
      << replayPreflightReasonName(secondResult.reason);
  EXPECT_FALSE(secondResult.candidateState.cpuUpload.active);
  EXPECT_EQ(secondResult.candidateState.depthEpoch, 2u);
  EXPECT_EQ(secondResult.candidateState.nextPacketSequence,
            firstResult.candidateState.nextPacketSequence);

  // A save-state/native load is a dominating surface+depth seed. Feeding the
  // real producer frame into preflight catches schema drift that isolated
  // builder tests cannot observe.
  constexpr std::size_t pixelCount =
      static_cast<std::size_t>(GPU::VRAM_WIDTH) * GPU::VRAM_HEIGHT;
  std::vector<uint16_t> replacement(pixelCount, 0x1234u);
  gpu.loadVram(reinterpret_cast<const uint8_t *>(replacement.data()));
  gpu.snapshotDisplayBuffer(3);
  const CrashReplayFramePtr third = gpu.tryConsumeReplay();
  ASSERT_TRUE(third);
  request.expectedLatch = third->latch();
  const std::array<CrashReplayFramePtr, 1> thirdBatch{third};
  const ReplayPreflightResult thirdResult =
      preflightCrashReplay(thirdBatch, request, secondResult.candidateState);
  ASSERT_EQ(thirdResult.route, ReplayPreflightRoute::M1Replay)
      << replayPreflightReasonName(thirdResult.reason);
  EXPECT_TRUE(thirdResult.candidateState.surfaceInitialized);
  EXPECT_TRUE(thirdResult.candidateState.knownNativeTiles.all());
  EXPECT_EQ(thirdResult.candidateState.depthEpoch, 3u);
}
