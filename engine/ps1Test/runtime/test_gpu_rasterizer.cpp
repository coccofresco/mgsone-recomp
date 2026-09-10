#include "runtime/gpu/gpu.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace ps1::gpu;

namespace {

struct ReferenceVertex {
  uint32_t word = 0;
  float x = 0.0f;
  float y = 0.0f;
  float depth = 1.0f;
};

struct ReferenceTriangle {
  std::array<ReferenceVertex, 3> vertices{};
  double area = 0.0;
  std::array<bool, 3> inclusive{};
};

double referenceEnhancedEdge(const ReferenceVertex &a, const ReferenceVertex &b,
                             double x, double y) {
  return (x - static_cast<double>(a.x)) * (static_cast<double>(b.y) - a.y) -
         (y - static_cast<double>(a.y)) * (static_cast<double>(b.x) - a.x);
}

bool referenceEnhancedTopLeft(const ReferenceVertex &a,
                              const ReferenceVertex &b) {
  const double dy = static_cast<double>(b.y) - a.y;
  const double dx = static_cast<double>(b.x) - a.x;
  return dy > 0.0 || (dy == 0.0 && dx < 0.0);
}

ReferenceTriangle makeReferenceTriangle(std::array<ReferenceVertex, 3> vertices,
                                        float scale) {
  for (ReferenceVertex &vertex : vertices) {
    // Match the production float multiply before promotion to double.
    vertex.x *= scale;
    vertex.y *= scale;
  }
  ReferenceTriangle setup;
  setup.vertices = vertices;
  setup.area = referenceEnhancedEdge(setup.vertices[0], setup.vertices[1],
                                     setup.vertices[2].x, setup.vertices[2].y);
  if (setup.area < 0.0) {
    std::swap(setup.vertices[1], setup.vertices[2]);
    setup.area = -setup.area;
  }
  setup.inclusive = {
      referenceEnhancedTopLeft(setup.vertices[1], setup.vertices[2]),
      referenceEnhancedTopLeft(setup.vertices[2], setup.vertices[0]),
      referenceEnhancedTopLeft(setup.vertices[0], setup.vertices[1])};
  return setup;
}

bool referenceEnhancedSample(const ReferenceTriangle &setup, int x, int y,
                             float &depth,
                             std::array<double, 3> *barycentrics = nullptr) {
  const double px = static_cast<double>(x) + 0.5;
  const double py = static_cast<double>(y) + 0.5;
  const double w0 =
      referenceEnhancedEdge(setup.vertices[1], setup.vertices[2], px, py);
  const double w1 =
      referenceEnhancedEdge(setup.vertices[2], setup.vertices[0], px, py);
  const double w2 =
      referenceEnhancedEdge(setup.vertices[0], setup.vertices[1], px, py);
  const auto covers = [](double edge, bool inclusive) {
    return edge > 0.0 || (edge == 0.0 && inclusive);
  };
  if (!covers(w0, setup.inclusive[0]) || !covers(w1, setup.inclusive[1]) ||
      !covers(w2, setup.inclusive[2]))
    return false;

  const double l0 = w0 / setup.area;
  const double l1 = w1 / setup.area;
  const double l2 = w2 / setup.area;
  if (barycentrics != nullptr)
    *barycentrics = {l0, l1, l2};
  const double affineDepth = l0 * setup.vertices[0].depth +
                             l1 * setup.vertices[1].depth +
                             l2 * setup.vertices[2].depth;
  if (!(affineDepth > 0.0) || !std::isfinite(affineDepth))
    return false;
  depth = static_cast<float>(affineDepth);
  return depth > 0.0f && std::isfinite(depth);
}

ps1::PGXPValue preciseVertex(uint32_t word, float x, float y, float depth) {
  ps1::PGXPValue value;
  value.x = x;
  value.y = y;
  value.z = depth;
  value.value = word;
  value.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
  return value;
}

ps1::PGXPValue xyOnlyVertex(uint32_t word, float x, float y) {
  ps1::PGXPValue value;
  value.x = x;
  value.y = y;
  value.z = std::numeric_limits<double>::quiet_NaN();
  value.value = word;
  value.flags = ps1::PGXPValue::VALID_XY;
  value.origin = ps1::PGXPOrigin::GteProjection;
  return value;
}

ps1::PGXPValue projectiveVertex(uint32_t word, double qx, double qy,
                                double depth, double nearDepth,
                                uint64_t projectionId,
                                uint64_t projectionContext = 1) {
  ps1::PGXPValue value;
  value.x = static_cast<float>(static_cast<int16_t>(word));
  value.y = static_cast<float>(static_cast<int16_t>(word >> 16));
  value.z = depth;
  value.value = word;
  value.flags =
      ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_SIGNED_PROJECTION;
  if (depth > 0.0f)
    value.flags |= ps1::PGXPValue::VALID_Z;
  value.origin = ps1::PGXPOrigin::GteProjection;
  value.qx = qx;
  value.qy = qy;
  value.nearDepth = nearDepth;
  value.projectionId = projectionId;
  value.projectionContext = projectionContext;
  return value;
}

void drawPreciseMonoTriangle(GPU &gpu, uint32_t command, float depth) {
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = preciseVertex(v0, 10.0f, 10.0f, depth);
  const ps1::PGXPValue p1 = preciseVertex(v1, 20.0f, 10.0f, depth);
  const ps1::PGXPValue p2 = preciseVertex(v2, 10.0f, 20.0f, depth);
  gpu.writeGP0(command);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
}

void drawPreciseMonoTriangle(GPU &gpu, uint32_t command,
                             const std::array<ReferenceVertex, 3> &vertices) {
  gpu.writeGP0(command);
  for (const ReferenceVertex &vertex : vertices) {
    const ps1::PGXPValue precision =
        preciseVertex(vertex.word, vertex.x, vertex.y, vertex.depth);
    gpu.writeGP0(vertex.word, UINT32_MAX, &precision);
  }
}

void drawPreciseMonoQuad(GPU &gpu, uint32_t command,
                         const std::array<ReferenceVertex, 4> &vertices) {
  gpu.writeGP0(command);
  for (const ReferenceVertex &vertex : vertices) {
    const ps1::PGXPValue precision =
        preciseVertex(vertex.word, vertex.x, vertex.y, vertex.depth);
    gpu.writeGP0(vertex.word, UINT32_MAX, &precision);
  }
}

void drawMonoQuadWithDegeneratePackedHalf(GPU &gpu,
                                          bool includeDegenerateVertex) {
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 10u;
  constexpr uint32_t v2 = (10u << 16) | 20u;
  constexpr uint32_t v3 = (20u << 16) | 20u;
  const ps1::PGXPValue p0 = preciseVertex(v0, 10.25f, 10.25f, 1.0f);
  const ps1::PGXPValue p1 = preciseVertex(v1, 10.25f, 10.25f, 1.0f);
  const ps1::PGXPValue p2 = preciseVertex(v2, 20.25f, 10.25f, 1.0f);
  const ps1::PGXPValue p3 = preciseVertex(v3, 20.25f, 20.25f, 1.0f);

  gpu.writeGP0(0x280000FFu);
  gpu.writeGP0(v0, UINT32_MAX, includeDegenerateVertex ? &p0 : nullptr);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  gpu.writeGP0(v3, UINT32_MAX, &p3);
}

Color32 modernPixel(GPU &gpu, uint32_t x, uint32_t y) {
  gpu.snapshotDisplayBuffer();
  std::vector<Color32> display;
  uint32_t width = 0;
  uint32_t height = 0;
  EXPECT_TRUE(gpu.copyDisplayRGBA8(display, width, height));
  EXPECT_LT(x, width);
  EXPECT_LT(y, height);
  return display[static_cast<std::size_t>(y) * width + x];
}

std::vector<Color32> copyModernDisplay(GPU &gpu, uint32_t &width,
                                       uint32_t &height) {
  gpu.snapshotDisplayBuffer();
  std::vector<Color32> display;
  EXPECT_TRUE(gpu.copyDisplayRGBA8(display, width, height));
  return display;
}

void clearTriangleRegion(GPU &gpu) {
  gpu.writeGP0(0x02000000u);
  gpu.writeGP0(0u);
  gpu.writeGP0((32u << 16) | 32u);
}

} // namespace

class GpuRasterizerTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuRasterizerTest, MonochromeTriangleDrawsPixels) {
  // Command: Opcode 0x20
  // C: RGB(0, 255, 0)
  gpu.writeGP0(0x2000FF00); // 0x20 | B=0, G=FF, R=00

  // V0: X=10, Y=10
  gpu.writeGP0((10 << 16) | 10);

  // V1: X=20, Y=10
  gpu.writeGP0((10 << 16) | 20);

  // V2: X=10, Y=20
  gpu.writeGP0((20 << 16) | 10);

  // This should rasterize a right triangle at the top-left (10,10) to (20,10)
  // and (10,20)

  const Color16 *vram = gpu.getVRAM();

  // Green is g5 = 255>>3 = 31 -> packed as 31 << 5
  uint16_t expectedRaw = 31 << 5;

  // Top left vertex should be drawn
  EXPECT_EQ(vram[11 * GPU::VRAM_WIDTH + 11].raw, expectedRaw);

  // Outside bounding box should be 0
  EXPECT_EQ(vram[9 * GPU::VRAM_WIDTH + 10].raw, 0);
  EXPECT_EQ(vram[10 * GPU::VRAM_WIDTH + 9].raw, 0);
  EXPECT_EQ(vram[21 * GPU::VRAM_WIDTH + 10].raw, 0);
}

TEST_F(GpuRasterizerTest, MonochromeQuadDrawsTwoTriangles) {
  // Command: Opcode 0x28
  // C: RGB(0, 0, 255)
  gpu.writeGP0(0x28FF0000); // 0x28 | B=FF, G=00, R=00

  // V0: 10, 10
  gpu.writeGP0((10 << 16) | 10);
  // V1: 20, 10
  gpu.writeGP0((10 << 16) | 20);
  // V2: 10, 20
  gpu.writeGP0((20 << 16) | 10);
  // V3: 20, 20
  gpu.writeGP0((20 << 16) | 20);

  const Color16 *vram = gpu.getVRAM();

  // Blue is b5 = 255>>3 = 31 -> packed as 31 << 10
  uint16_t expectedRaw = 31 << 10;

  // Check typical internal points
  EXPECT_EQ(vram[15 * GPU::VRAM_WIDTH + 15].raw, expectedRaw);

  // Outside
  EXPECT_EQ(vram[9 * GPU::VRAM_WIDTH + 10].raw, 0);
}

TEST_F(GpuRasterizerTest, SemiTransparentQuadDoesNotBlendSharedEdgeTwice) {
  // ABR 1 adds foreground to background. A 0x40 red command quantizes to R=8;
  // if both component triangles cover their shared diagonal it becomes R=16.
  gpu.writeGP0(0xE1000020);
  gpu.writeGP0(0x2A000040);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((10 << 16) | 14);
  gpu.writeGP0((14 << 16) | 10);
  gpu.writeGP0((14 << 16) | 14);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[12 * 1024 + 12].raw & 0x1F, 8)
      << "shared quad diagonal was blended by both triangles";
  EXPECT_EQ(vram[14 * 1024 + 12].raw, 0)
      << "bottom edge must be excluded by the top-left fill rule";
  EXPECT_EQ(vram[12 * 1024 + 14].raw, 0)
      << "right edge must be excluded by the top-left fill rule";
}

TEST_F(GpuRasterizerTest, StrictTrueDepthOrdersOpaqueTrianglesByDepth) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  drawPreciseMonoTriangle(gpu, 0x200000FF, 1.0f);  // near red
  drawPreciseMonoTriangle(gpu, 0x2000FF00, 10.0f); // far green
  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x03E0)
      << "native VRAM must remain PS1 painter ordered";
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{255, 0, 0, 255}))
      << "modern companion must retain the nearer surface";

  // A wall-clock latch is not a render epoch and must not clear modern Z.
  drawPreciseMonoTriangle(gpu, 0x20FF0000, 20.0f); // farther blue
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{255, 0, 0, 255}));

  // A guest color clear invalidates depth in the affected region.
  clearTriangleRegion(gpu);
  drawPreciseMonoTriangle(gpu, 0x2000FF00, 10.0f); // far green
  drawPreciseMonoTriangle(gpu, 0x20FF0000, 5.0f);  // nearer blue
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}))
      << "the guest clear must start a fresh depth region";
}

TEST_F(GpuRasterizerTest,
       VramSelfCopyWithoutForcedMaskPreservesEnhancedColorAndDepth) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f); // near red
  ASSERT_EQ(modernPixel(gpu, 12, 12), (Color32{255, 0, 0, 255}));

  gpu.writeGP0(0xE6000000u);
  gpu.writeGP0(0x80000000u);
  gpu.writeGP0((12u << 16) | 12u);
  gpu.writeGP0((12u << 16) | 12u);
  gpu.writeGP0((1u << 16) | 1u);

  drawPreciseMonoTriangle(gpu, 0x2000FF00u, 10.0f); // far green
  EXPECT_EQ(gpu.getVRAM()[12u * GPU::VRAM_WIDTH + 12u].raw, 0x03E0u)
      << "native VRAM remains painter ordered";
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{255, 0, 0, 255}))
      << "an identity copy must not invalidate the existing depth sample";
}

TEST_F(GpuRasterizerTest,
       HybridOpaquePacketsFollowGp0OrderWithoutDepthRejects) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  // Physical W deliberately disagrees with GP0 order. Hybrid must retain the
  // latter as the cross-packet visibility authority while every fully-valid
  // opaque packet remains eligible to store its genuine W.
  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f);   // A: near red
  drawPreciseMonoTriangle(gpu, 0x2000FF00u, 10.0f);  // B: farther green
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 20.0f);  // C: farthest blue
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u);
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 3u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u)
      << "Hybrid must not infer a cross-packet LEQUAL relation from W";

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, HybridLargeWJumpNeedsNoVisibilityEpoch) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 100.0f);
  constexpr std::array<ReferenceVertex, 4> farQuad = {
      ReferenceVertex{(10u << 16) | 10u, 10.0f, 10.0f, 2500.0f},
      ReferenceVertex{(10u << 16) | 20u, 20.0f, 10.0f, 2500.0f},
      ReferenceVertex{(20u << 16) | 10u, 10.0f, 20.0f, 2500.0f},
      ReferenceVertex{(20u << 16) | 20u, 20.0f, 20.0f, 2500.0f},
  };
  drawPreciseMonoQuad(gpu, 0x2800FF00u, farQuad);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 255, 0, 255}));
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.epoch.clear.z_jump"), 0u)
      << "an OT-later packet must not need a heuristic clear to be visible";
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       HybridDrawAreaChangesDoNotCreateVisibilityEpochs) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 100.0f);  // near red
  drawPreciseMonoTriangle(gpu, 0x2000FF00u, 2500.0f); // OT-later green

  // A real 2D command consumes the changed drawing state. Neither it nor the
  // large W discontinuity above may be required to rescue opaque visibility.
  gpu.writeGP0(0xE3000000u | 100u | (100u << 10));
  gpu.writeGP0(0xE4000000u | 130u | (130u << 10));
  gpu.writeGP0(0x6000FFFFu);
  gpu.writeGP0((105u << 16) | 105u);
  gpu.writeGP0((4u << 16) | 4u);
  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 31u | (31u << 10));
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 3000.0f);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.epoch.clear.z_jump"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.epoch.clear.draw_area"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       HybridNearSaturatedGteDepthRemainsEligibleAndPainterOrdered) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(false);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f); // near red + Z
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0, 10.0, 0.0, 1.0, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 20.0, 10.0, 0.0, 1.0, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 10.0, 20.0, 0.0, 1.0, 3);
  gpu.writeGP0(0x2000FF00u); // divider-saturated green at donor near depth
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 10.0f); // far blue
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u);
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}))
      << "valid near-saturated W must not hide an OT-later opaque packet";
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.invalid_w"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 3u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       HybridInvalidWRetainsPgxpXYWithoutBlockingLaterOpaquePacket) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f); // near red

  constexpr std::array<uint32_t, 3> words = {
      (10u << 16) | 10u,
      (10u << 16) | 20u,
      (20u << 16) | 10u,
  };
  const std::array<ps1::PGXPValue, 3> invalidW = {
      xyOnlyVertex(words[0], 10.25f, 10.25f),
      xyOnlyVertex(words[1], 20.25f, 10.25f),
      xyOnlyVertex(words[2], 10.25f, 20.25f),
  };
  gpu.writeGP0(0x2000FF00u); // invalid-W green: painter + packet W=1
  for (std::size_t i = 0; i < words.size(); ++i)
    gpu.writeGP0(words[i], UINT32_MAX, &invalidW[i]);

  // Invalid W remains packet-atomic and must not publish a fabricated depth.
  // It also cannot make the preceding near owner hide OT-later opaque work.
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 10.0f); // far blue
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u);
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.invalid_w"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 3u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.partial_precision_packet"), 1u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, StrictInvalidWStillPublishesDiagnosticFarDepth) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Strict);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f); // near red

  constexpr std::array<uint32_t, 3> words = {
      (10u << 16) | 10u,
      (10u << 16) | 20u,
      (20u << 16) | 10u,
  };
  const std::array<ps1::PGXPValue, 3> invalidW = {
      xyOnlyVertex(words[0], 10.25f, 10.25f),
      xyOnlyVertex(words[1], 20.25f, 10.25f),
      xyOnlyVertex(words[2], 10.25f, 20.25f),
  };
  gpu.writeGP0(0x2000FF00u); // Strict ALWAYS + diagnostic far write
  for (std::size_t i = 0; i < words.size(); ++i)
    gpu.writeGP0(words[i], UINT32_MAX, &invalidW[i]);

  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 10.0f); // passes far W=32768
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
}

TEST_F(GpuRasterizerTest,
       HybridScreen2DDetourDoesNotControlLaterOpaqueVisibility) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(false);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f); // depth in area A

  // Move to B and consume the changed state with a genuine 2D rectangle.
  // Returning to A must not need a whole-depth epoch clear for visibility.
  gpu.writeGP0(0xE3000000u | 100u | (100u << 10));
  gpu.writeGP0(0xE4000000u | 130u | (130u << 10));
  gpu.writeGP0(0x6000FF00u);
  gpu.writeGP0((105u << 16) | 105u);
  gpu.writeGP0((4u << 16) | 4u);

  gpu.writeGP0(0xE3000000u);
  gpu.writeGP0(0xE4000000u | 31u | (31u << 10));
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 10.0f); // far blue in A
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u);
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}))
      << "OT order, not a draw-area heuristic, controls packet visibility";
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.epoch.clear.draw_area"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, EmptyRasterTelemetryAttributesDepthOnlyOcclusion) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f);
  gpu.snapshotDisplayBuffer();
  ps1::metrics::reset();
  drawPreciseMonoTriangle(gpu, 0x2000FF00u, 10.0f);
  gpu.snapshotDisplayBuffer();

  const uint64_t covered = ps1::metrics::get("gpu.modern.raster.covered");
  ASSERT_GT(covered, 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), covered);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.sample"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty.depth_only"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty.transparent_only"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty.mixed"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       ComponentReconciliationPreservesPacketAtomicTrueDepthRoute) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  // Establish a near red surface in the modern depth buffer.
  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f);

  constexpr std::array<uint32_t, 3> words = {
      (10u << 16) | 10u,
      (10u << 16) | 20u,
      (20u << 16) | 10u,
  };
  std::array<ps1::PGXPValue, 3> far = {
      preciseVertex(words[0], 19.0f, 10.25f, 10.0f),
      preciseVertex(words[1], 20.25f, 19.0f, 10.0f),
      preciseVertex(words[2], 10.25f, 20.25f, 10.0f),
  };

  // X on v0 and Y on v1 each exceed the side-channel safety threshold. They
  // resolve independently to packed X/Y while the complete triangle remains
  // one precise packet with its valid (farther) Z.
  gpu.writeGP0(0x2000FF00u);
  for (std::size_t i = 0; i < words.size(); ++i)
    gpu.writeGP0(words[i], UINT32_MAX, &far[i]);

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x03E0u)
      << "native PS1 VRAM must remain painter ordered";
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{255, 0, 0, 255}))
      << "one reconciled component switched the whole packet to compatibility";
}

TEST_F(GpuRasterizerTest,
       SignedProjectiveVertexClipsWithoutChangingPacketRoute) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  // v0 lies on the camera plane. Its homogeneous numerator remains finite;
  // clipping at W=1 turns the guest triangle into a modern quad (two fan
  // triangles) without falling back to packed/pixel-doubled geometry.
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0f, 10.0f, 0.0f, 1.0f, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 40.0f, 20.0f, 2.0f, 1.0f, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 20.0f, 40.0f, 2.0f, 1.0f, 3);
  gpu.writeGP0(0x200000FFu);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.partial_precision_packet"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.polygon"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.output_triangle"), 2u);
  EXPECT_EQ(modernPixel(gpu, 18, 14), (Color32{255, 0, 0, 255}));

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       AllBehindProjectiveTriangleLeavesNativeAuthorityUnchanged) {
  GPU native;
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  native.writeGP0(0x2000FF00u);
  native.writeGP0(v0);
  native.writeGP0(v1);
  native.writeGP0(v2);

  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0f, 10.0f, 0.0f, 1.0f, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 20.0f, 10.0f, 0.0f, 1.0f, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 10.0f, 20.0f, 0.0f, 1.0f, 3);
  gpu.writeGP0(0x2000FF00u);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);

  EXPECT_EQ(std::memcmp(native.getVRAM(), gpu.getVRAM(),
                        GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT * sizeof(Color16)),
            0)
      << "near clipping changed authoritative native PS1 VRAM";
  uint32_t width = 0;
  uint32_t height = 0;
  const std::vector<Color32> modern = copyModernDisplay(gpu, width, height);
  EXPECT_TRUE(std::all_of(modern.begin(), modern.end(),
                          [](Color32 pixel) { return pixel == Color32{}; }));
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.discarded"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.discarded.native_coverage"),
            1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.discarded.native_empty"),
            0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       DonorCompatibleSaturationKeepsAllBehindTriangleVisible) {
  GPU native;
  gpu.configureEnhancedPresentation(4, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(false);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  native.writeGP0(0x2000FF00u);
  native.writeGP0(v0);
  native.writeGP0(v1);
  native.writeGP0(v2);

  ps1::PGXPValue p0 = projectiveVertex(v0, 10.0, 10.0, 0.0, 1.0, 1);
  ps1::PGXPValue p1 = projectiveVertex(v1, 20.0, 10.0, 0.0, 1.0, 2);
  ps1::PGXPValue p2 = projectiveVertex(v2, 10.0, 20.0, 0.0, 1.0, 3);
  // Deliberately retain a sub-pixel saturated SXY distinct from the packed
  // coordinates. At 4x this proves the GTE value survives resolution and the
  // near-plane policy instead of being silently rounded back to native XY.
  p0.x = 10.75f;
  p0.y = 10.75f;
  p1.x = 20.75f;
  p1.y = 10.75f;
  p2.x = 10.75f;
  p2.y = 20.75f;
  gpu.writeGP0(0x2000FF00u);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(std::memcmp(native.getVRAM(), gpu.getVRAM(),
                        GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT * sizeof(Color16)),
            0);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_saturated.triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_saturated.vertex"), 3u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.discarded"), 0u);
  EXPECT_EQ(modernPixel(gpu, 41, 41), Color32{});
  EXPECT_EQ(modernPixel(gpu, 44, 44), (Color32{0, 255, 0, 255}));

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, ModernCoverageDivergenceTraceIdentifiesGuestPacket) {
  const char *path = "/tmp/ps1_modern_coverage_divergence_test.jsonl";
  std::remove(path);
  setenv("PS1_TRACE_JSONL", path, 1);
  setenv("PS1_TRACE_MODERN_REJECTS", "1", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM", "42", 1);
  setenv("PS1_TRACE_MODERN_REJECT_FRAME_TO", "42", 1);
  ps1::metrics::resetTraceForTesting();
  ps1::metrics::setTraceFrame(42);

  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  constexpr uint32_t source = 0x80001000u;
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0, 10.0, 0.0, 1.0, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 20.0, 10.0, 0.0, 1.0, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 10.0, 20.0, 0.0, 1.0, 3);
  gpu.writeGP0(0x2000FF00u, source);
  gpu.writeGP0(v0, source + 4, &p0);
  gpu.writeGP0(v1, source + 8, &p1);
  gpu.writeGP0(v2, source + 12, &p2);

  ps1::metrics::resetTraceForTesting();
  std::ifstream input(path);
  const std::string trace((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(trace.find("\"event\":\"gpu.modern_coverage_divergence\""),
            std::string::npos);
  EXPECT_NE(trace.find("\"word_address\":2147487744"), std::string::npos);
  EXPECT_NE(trace.find("\"opcode\":32"), std::string::npos);
  EXPECT_NE(trace.find("\"outcome\":1"), std::string::npos);
  EXPECT_NE(trace.find("\"post_near_clip\":1"), std::string::npos);
  EXPECT_NE(trace.find("\"fan_index\":-1"), std::string::npos);
  EXPECT_NE(trace.find("\"output_triangle_count\":0"), std::string::npos);
  EXPECT_NE(trace.find("\"native_x0\":10"), std::string::npos);

  unsetenv("PS1_TRACE_JSONL");
  unsetenv("PS1_TRACE_MODERN_REJECTS");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_FROM");
  unsetenv("PS1_TRACE_MODERN_REJECT_FRAME_TO");
  ps1::metrics::resetTraceForTesting();
  std::remove(path);
}

TEST_F(GpuRasterizerTest, TwoBehindVerticesProduceOneClippedTriangle) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0f, 10.0f, 0.0f, 1.0f, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 20.0f, 10.0f, 0.0f, 1.0f, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 20.0f, 40.0f, 2.0f, 1.0f, 3);
  gpu.writeGP0(0x200000FFu);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.polygon"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.output_triangle"), 1u);
  EXPECT_NE(modernPixel(gpu, 15, 23), Color32{});

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, ExactNearVertexIsNotDuplicatedByClipping) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0, 10.0, 1.0, 1.0, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 20.0, 10.0, 0.0, 1.0, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 20.0, 40.0, 2.0, 1.0, 3);
  gpu.writeGP0(0x200000FFu);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.polygon"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.near_clip.output_triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.precise_degenerate"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, ProjectivePacketRejectsMismatchedContextOrNearPlane) {
  for (const bool mismatchContext : {false, true}) {
    SCOPED_TRACE(mismatchContext ? "context" : "near plane");
    gpu.reset();
    gpu.configureEnhancedPresentation(1, true);
    gpu.setEnhancedRaster(true);
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);

    constexpr uint32_t v0 = (10u << 16) | 10u;
    constexpr uint32_t v1 = (10u << 16) | 20u;
    constexpr uint32_t v2 = (20u << 16) | 10u;
    const ps1::PGXPValue p0 = projectiveVertex(v0, 20.0, 20.0, 2.0, 1.0, 1);
    const ps1::PGXPValue p1 = projectiveVertex(v1, 40.0, 20.0, 2.0, 1.0, 2);
    ps1::PGXPValue p2 = projectiveVertex(v2, 20.0, 40.0, 2.0, 1.0, 3);
    if (mismatchContext)
      ++p2.projectionContext;
    else
      p2.nearDepth += 1.0 / 4096.0;

    gpu.writeGP0(0x200000FFu);
    gpu.writeGP0(v0, UINT32_MAX, &p0);
    gpu.writeGP0(v1, UINT32_MAX, &p1);
    gpu.writeGP0(v2, UINT32_MAX, &p2);
    gpu.snapshotDisplayBuffer();

    EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 0u);
    EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 1u);
    EXPECT_EQ(ps1::metrics::get("gpu.modern_setup_fallback"), 1u);
    EXPECT_EQ(ps1::metrics::get("gpu.modern.projective_context_mismatch"), 1u);
  }

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       NearClippedSemiTransparentFanDoesNotBlendSharedEdgeTwice) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  gpu.writeGP0(0xE1000020u); // ABR 1: additive, double hits are observable.

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0f, 10.0f, 0.0f, 1.0f, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 40.0f, 20.0f, 2.0f, 1.0f, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 20.0f, 40.0f, 2.0f, 1.0f, 3);
  gpu.writeGP0(0x22000040u);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);

  EXPECT_EQ(modernPixel(gpu, 17, 17), (Color32{64, 0, 0, 255}))
      << "the diagonal generated by near clipping blended twice";
}

TEST_F(GpuRasterizerTest, ReprojectedNearClippedPacketsShareOneCanonicalEdge) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);
  gpu.writeGP0(0xE1000020u); // ABR 1: additive, double hits are observable.

  constexpr uint32_t a = (10u << 16) | 10u;
  constexpr uint32_t b = (10u << 16) | 20u;
  constexpr uint32_t c = (20u << 16) | 10u;
  constexpr uint32_t d = (20u << 16) | 20u;
  const ps1::PGXPValue a0 = projectiveVertex(a, 20.0, 20.0, 2.0, 1.0, 100);
  const ps1::PGXPValue b0 = projectiveVertex(b, 20.0, 20.0, 0.0, 1.0, 400);
  const ps1::PGXPValue c0 = projectiveVertex(c, 20.0, 40.0, 2.0, 1.0, 200);
  // The adjacent packet re-projects the same shared endpoints with IDs whose
  // order is deliberately opposite. Geometry, not allocation order, must
  // canonicalize the generated intersection.
  const ps1::PGXPValue c1 = projectiveVertex(c, 20.0, 40.0, 2.0, 1.0, 900);
  const ps1::PGXPValue b1 = projectiveVertex(b, 20.0, 20.0, 0.0, 1.0, 100);
  const ps1::PGXPValue d1 = projectiveVertex(d, 20.0, 60.0, 2.0, 1.0, 800);

  gpu.writeGP0(0x22000040u);
  gpu.writeGP0(a, UINT32_MAX, &a0);
  gpu.writeGP0(b, UINT32_MAX, &b0);
  gpu.writeGP0(c, UINT32_MAX, &c0);
  gpu.writeGP0(0x22000040u);
  gpu.writeGP0(c, UINT32_MAX, &c1);
  gpu.writeGP0(b, UINT32_MAX, &b1);
  gpu.writeGP0(d, UINT32_MAX, &d1);

  EXPECT_EQ(modernPixel(gpu, 15, 25), (Color32{64, 0, 0, 255}))
      << "a re-projected shared clipped edge cracked or blended twice";
}

TEST_F(GpuRasterizerTest, NearClippedGouraudTriangleInterpolatesAttributes) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setProjectiveNearClip(true);
  gpu.setTrueDepth(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = projectiveVertex(v0, 10.0f, 10.0f, 0.0f, 1.0f, 1);
  const ps1::PGXPValue p1 = projectiveVertex(v1, 40.0f, 20.0f, 2.0f, 1.0f, 2);
  const ps1::PGXPValue p2 = projectiveVertex(v2, 20.0f, 40.0f, 2.0f, 1.0f, 3);

  gpu.writeGP0(0x300000FFu); // v0 red
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(0x0000FF00u); // v1 green
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(0x00FF0000u); // v2 blue
  gpu.writeGP0(v2, UINT32_MAX, &p2);

  const Color32 clippedColor = modernPixel(gpu, 17, 17);
  EXPECT_GT(clippedColor.r, 0u);
  EXPECT_GT(clippedColor.g, 0u);
  EXPECT_GT(clippedColor.b, 0u);
  EXPECT_EQ(clippedColor.a, 255u);
}

TEST_F(GpuRasterizerTest,
       PreciseDegenerateStaysModernInsteadOfFlashingPackedSliver) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr std::array<uint32_t, 3> words = {
      (10u << 16) | 10u,
      (11u << 16) | 20u,
      (13u << 16) | 30u,
  };
  // Packed coordinates form a tiny non-zero sliver; exact PGXP coordinates
  // are collinear. Geometry outcome must not switch the data route.
  const std::array<ps1::PGXPValue, 3> precision = {
      preciseVertex(words[0], 10.25f, 10.25f, 4.0f),
      preciseVertex(words[1], 20.25f, 11.25f, 4.0f),
      preciseVertex(words[2], 30.25f, 12.25f, 4.0f),
  };
  gpu.writeGP0(0x20FF0000u);
  for (std::size_t i = 0; i < words.size(); ++i)
    gpu.writeGP0(words[i], UINT32_MAX, &precision[i]);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern_setup_fallback"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.precise_degenerate"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.precise_degenerate.native_coverage"),
            1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.precise_degenerate.native_empty"),
            0u);
  EXPECT_TRUE(std::any_of(gpu.getVRAM(),
                          gpu.getVRAM() + GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT,
                          [](Color16 pixel) { return pixel.raw != 0; }))
      << "native packed sliver must remain guest-authoritative";

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       EnhancedSemiTransparentTrianglesRemainPainterOrdered) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  // A semi-transparent primitive must not seed its own W as an occluder: the
  // medium opaque blue draw passes after the translucent OT boundary.
  drawPreciseMonoTriangle(gpu, 0x2000FF00, 10.0f); // far opaque green
  drawPreciseMonoTriangle(gpu, 0x220000FF, 1.0f);  // near semi red
  drawPreciseMonoTriangle(gpu, 0x20FF0000, 5.0f);  // medium opaque blue
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}))
      << "semi-transparent geometry must not become a W occluder";

  // Nor may it test depth: a far translucent green still blends over the near
  // opaque red according to ordering-table submission.
  clearTriangleRegion(gpu);
  drawPreciseMonoTriangle(gpu, 0x200000FF, 1.0f);  // near opaque red
  drawPreciseMonoTriangle(gpu, 0x2200FF00, 10.0f); // far semi green
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{127, 127, 0, 255}))
      << "semi-transparent geometry must ignore existing true depth";
}

TEST_F(GpuRasterizerTest,
       HybridTransparentTriangleStartsAcceptedVisibilityEpoch) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FF, 1.0f);  // near opaque red
  drawPreciseMonoTriangle(gpu, 0x2200FF00, 10.0f); // far translucent green
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{127, 127, 0, 255}))
      << "transparent donor state must bypass the existing near depth";

  // The accepted translucent draw releases the preceding D32 owner at this
  // sample. The following OT-later opaque packet establishes a fresh real-Z
  // owner instead of disappearing behind stale depth from the red draw.
  drawPreciseMonoTriangle(gpu, 0x20FF0000, 5.0f); // medium opaque blue
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
  EXPECT_EQ(ps1::metrics::get(
                "gpu.modern.depth.packet.blend_visibility_boundary"),
            1u);
  EXPECT_GT(ps1::metrics::get(
                "gpu.modern.depth.sample.visibility_boundary"),
            0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 2u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       HybridMaskRejectedPacketCannotHideFollowingOtOpaque) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  // A owns both color, mask and near depth.
  gpu.writeGP0(0xE6000001u); // force mask, do not check
  drawPreciseMonoTriangle(gpu, 0x200000FF, 1.0f);

  // B is painter work but E6 rejects every covered sample. It must alter
  // neither color/mask nor the previous depth owner.
  gpu.writeGP0(0xE6000002u); // check mask, do not force
  drawPreciseMonoTriangle(gpu, 0x2200FF00, 10.0f);

  // C no longer checks E6. Although B never reached fragment acceptance, A's
  // old near W cannot reject this OT-later opaque packet in Hybrid mode.
  gpu.writeGP0(0xE6000000u);
  drawPreciseMonoTriangle(gpu, 0x20FF0000, 5.0f);
  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u);
  EXPECT_EQ(modernPixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
  EXPECT_GT(ps1::metrics::get("gpu.modern.raster.reject.mask"), 0u);
  EXPECT_EQ(ps1::metrics::get(
                "gpu.modern.depth.sample.visibility_boundary"),
            0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest,
       IncrementalEnhancedRasterMatchesExactCoverageAndDepthReference) {
  constexpr float scale = 4.0f;
  gpu.configureEnhancedPresentation(static_cast<uint32_t>(scale), true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  const std::array<ReferenceVertex, 3> redVertices = {{
      {(20u << 16) | 20u, 20.25f, 20.625f, 1.0f},
      {(21u << 16) | 44u, 44.5f, 21.25f, 8.0f},
      {(45u << 16) | 22u, 21.75f, 45.125f, 3.0f},
  }};
  std::array<ReferenceVertex, 3> greenVertices = redVertices;
  for (ReferenceVertex &vertex : greenVertices)
    vertex.depth = 4.0f;

  drawPreciseMonoTriangle(gpu, 0x200000FFu, redVertices);
  drawPreciseMonoTriangle(gpu, 0x2000FF00u, greenVertices);

  const ReferenceTriangle redReference =
      makeReferenceTriangle(redVertices, scale);
  const ReferenceTriangle greenReference =
      makeReferenceTriangle(greenVertices, scale);
  uint32_t displayWidth = 0;
  uint32_t displayHeight = 0;
  const std::vector<Color32> display =
      copyModernDisplay(gpu, displayWidth, displayHeight);

  // Include a margin around the exact subpixel bounds so both covered and
  // rejected samples are compared against the old per-pixel equations.
  for (int y = 76; y <= 186; ++y) {
    for (int x = 75; x <= 184; ++x) {
      float redDepth = 0.0f;
      float greenDepth = 0.0f;
      const bool redCovered =
          referenceEnhancedSample(redReference, x, y, redDepth);
      const bool greenCovered =
          referenceEnhancedSample(greenReference, x, y, greenDepth);
      Color32 expected{};
      if (redCovered)
        expected = Color32{255, 0, 0, 255};
      if (greenCovered && (!redCovered || greenDepth <= redDepth))
        expected = Color32{0, 255, 0, 255};

      SCOPED_TRACE(::testing::Message()
                   << "sample (" << x << ',' << y << ") redDepth=" << redDepth
                   << " greenDepth=" << greenDepth);
      ASSERT_LT(static_cast<uint32_t>(x), displayWidth);
      ASSERT_LT(static_cast<uint32_t>(y), displayHeight);
      EXPECT_EQ(display[static_cast<std::size_t>(y) * displayWidth + x],
                expected);
    }
  }
}

TEST_F(GpuRasterizerTest,
       IncrementalEnhancedQuadMatchesExactSharedEdgeReference) {
  constexpr float scale = 4.0f;
  gpu.configureEnhancedPresentation(static_cast<uint32_t>(scale), true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);
  gpu.writeGP0(0xE1000020u); // additive ABR makes a double hit observable

  const std::array<ReferenceVertex, 4> vertices = {{
      {(50u << 16) | 50u, 50.25f, 50.25f, 2.0f},
      {(51u << 16) | 70u, 70.5f, 50.75f, 2.0f},
      {(70u << 16) | 51u, 50.75f, 70.25f, 2.0f},
      {(71u << 16) | 71u, 70.75f, 70.5f, 2.0f},
  }};
  drawPreciseMonoQuad(gpu, 0x2A000040u, vertices);

  const ReferenceTriangle first =
      makeReferenceTriangle({vertices[0], vertices[1], vertices[2]}, scale);
  const ReferenceTriangle second =
      makeReferenceTriangle({vertices[1], vertices[2], vertices[3]}, scale);
  uint32_t displayWidth = 0;
  uint32_t displayHeight = 0;
  const std::vector<Color32> display =
      copyModernDisplay(gpu, displayWidth, displayHeight);

  for (int y = 195; y <= 288; ++y) {
    for (int x = 195; x <= 288; ++x) {
      float ignoredDepth = 0.0f;
      const int coverage =
          static_cast<int>(referenceEnhancedSample(first, x, y, ignoredDepth)) +
          static_cast<int>(referenceEnhancedSample(second, x, y, ignoredDepth));
      SCOPED_TRACE(::testing::Message()
                   << "sample (" << x << ',' << y
                   << ") exact triangle hits=" << coverage);
      ASSERT_LE(coverage, 1) << "reference top-left edges overlap";
      const Color32 expected =
          coverage == 0 ? Color32{} : Color32{64, 0, 0, 255};
      ASSERT_LT(static_cast<uint32_t>(x), displayWidth);
      ASSERT_LT(static_cast<uint32_t>(y), displayHeight);
      EXPECT_EQ(display[static_cast<std::size_t>(y) * displayWidth + x],
                expected);
    }
  }
}

TEST_F(GpuRasterizerTest,
       IncrementalEnhancedGouraudMatchesExactBarycentricReference) {
  constexpr float scale = 4.0f;
  gpu.configureEnhancedPresentation(static_cast<uint32_t>(scale), true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  const std::array<ReferenceVertex, 3> vertices = {{
      {(20u << 16) | 20u, 20.25f, 20.625f, 2.0f},
      {(21u << 16) | 44u, 44.5f, 21.25f, 2.0f},
      {(45u << 16) | 22u, 21.75f, 45.125f, 2.0f},
  }};
  const std::array<Color32, 3> vertexColors = {Color32{255, 0, 0, 255},
                                               Color32{0, 255, 0, 255},
                                               Color32{0, 0, 255, 255}};
  gpu.writeGP0(0x300000FFu);
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    const ps1::PGXPValue precision = preciseVertex(
        vertices[i].word, vertices[i].x, vertices[i].y, vertices[i].depth);
    gpu.writeGP0(vertices[i].word, UINT32_MAX, &precision);
    if (i + 1 < vertices.size()) {
      const Color32 color = vertexColors[i + 1];
      gpu.writeGP0(static_cast<uint32_t>(color.r) |
                   (static_cast<uint32_t>(color.g) << 8) |
                   (static_cast<uint32_t>(color.b) << 16));
    }
  }

  const ReferenceTriangle reference = makeReferenceTriangle(vertices, scale);
  auto colorForVertex = [&](const ReferenceVertex &vertex) {
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      if (vertices[i].word == vertex.word)
        return vertexColors[i];
    }
    return Color32{};
  };
  const std::array<Color32, 3> referenceColors = {
      colorForVertex(reference.vertices[0]),
      colorForVertex(reference.vertices[1]),
      colorForVertex(reference.vertices[2])};
  uint32_t displayWidth = 0;
  uint32_t displayHeight = 0;
  const std::vector<Color32> display =
      copyModernDisplay(gpu, displayWidth, displayHeight);

  for (int y = 76; y <= 186; ++y) {
    for (int x = 75; x <= 184; ++x) {
      float ignoredDepth = 0.0f;
      std::array<double, 3> barycentrics{};
      const bool covered =
          referenceEnhancedSample(reference, x, y, ignoredDepth, &barycentrics);
      Color32 expected{};
      if (covered) {
        const auto channel = [&](uint8_t Color32::*member) {
          return static_cast<uint8_t>(std::clamp(
              static_cast<int>(barycentrics[0] * referenceColors[0].*member +
                               barycentrics[1] * referenceColors[1].*member +
                               barycentrics[2] * referenceColors[2].*member),
              0, 255));
        };
        expected = Color32{channel(&Color32::r), channel(&Color32::g),
                           channel(&Color32::b), 255};
      }

      SCOPED_TRACE(::testing::Message() << "sample (" << x << ',' << y << ")");
      ASSERT_LT(static_cast<uint32_t>(x), displayWidth);
      ASSERT_LT(static_cast<uint32_t>(y), displayHeight);
      EXPECT_EQ(display[static_cast<std::size_t>(y) * displayWidth + x],
                expected);
    }
  }
}

TEST_F(GpuRasterizerTest,
       NativeVramIsBitIdenticalWithEnhancedTrueDepthEnabled) {
  GPU enhanced;
  gpu.configureEnhancedPresentation(1, true);
  enhanced.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(false);
  gpu.setTrueDepth(false);
  enhanced.setEnhancedRaster(true);
  enhanced.setTrueDepth(true);

  // Deliberately submit the surfaces back-to-front after the nearest one.
  // The modern target must keep red through true Z, while authoritative PS1
  // VRAM remains painter ordered in both configurations.
  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f);
  drawPreciseMonoTriangle(gpu, 0x2000FF00u, 10.0f);
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 5.0f);
  drawPreciseMonoTriangle(enhanced, 0x200000FFu, 1.0f);
  drawPreciseMonoTriangle(enhanced, 0x2000FF00u, 10.0f);
  drawPreciseMonoTriangle(enhanced, 0x20FF0000u, 5.0f);

  constexpr std::size_t vramBytes = static_cast<std::size_t>(GPU::VRAM_WIDTH) *
                                    GPU::VRAM_HEIGHT * sizeof(Color16);
  EXPECT_EQ(std::memcmp(gpu.getVRAM(), enhanced.getVRAM(), vramBytes), 0)
      << "modern replay or true Z changed guest-visible native VRAM";
  EXPECT_EQ(enhanced.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u)
      << "native VRAM must retain the last submitted opaque surface";
  EXPECT_EQ(modernPixel(enhanced, 12, 12), (Color32{255, 0, 0, 255}))
      << "the control sequence did not exercise modern true depth";
}

TEST_F(GpuRasterizerTest,
       QuadInvalidWRouteIsAtomicWithOnePackedDegenerateHalf) {
  gpu.configureEnhancedPresentation(4, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  // v0 == v1 makes the first packed triangle degenerate; v1/v2/v3 is valid.
  // With all four sidecars present, the valid half retains a subpixel diagonal.
  drawMonoQuadWithDegeneratePackedHalf(gpu, true);
  const Color32 preciseInside = modernPixel(gpu, 63, 60);
  const Color32 preciseOutside = modernPixel(gpu, 60, 63);
  EXPECT_EQ(preciseInside, (Color32{255, 0, 0, 255}));
  EXPECT_EQ(preciseOutside, (Color32{0, 0, 0, 255}));

  gpu.reset();

  // Only v0 loses all provenance. Swan retains native float XY for it, forces
  // W=1 across all four vertices and keeps both halves in the high-resolution
  // hardware route. It must not block-expand the packet through the 1x
  // compatibility mirror or leave the valid half on a different W policy.
  ps1::metrics::reset();
  drawMonoQuadWithDegeneratePackedHalf(gpu, false);
  const Color32 fallbackA = modernPixel(gpu, 63, 60);
  const Color32 fallbackB = modernPixel(gpu, 60, 63);
  EXPECT_EQ(gpu.getVRAM()[15 * GPU::VRAM_WIDTH + 15].raw, 0x001Fu)
      << "the non-degenerate packed half must remain drawable";
  EXPECT_EQ(fallbackA, (Color32{255, 0, 0, 255}));
  EXPECT_EQ(fallbackB, (Color32{0, 0, 0, 255}));
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.invalid_w"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.partial_precision_packet"), 1u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuRasterizerTest, SnapshotCannotObservePartialCommandSubmission) {
  struct SnapshotResult {
    GPU::DisplayState state;
    uint16_t firstPixel = 0;
    uint16_t secondPixel = 0;
  };

  gpu.beginCommandBatch();
  gpu.writeGP0(0x680000FFu);
  gpu.writeGP0((10u << 16) | 10u);

  std::promise<void> snapshotStarted;
  std::future<void> started = snapshotStarted.get_future();
  std::promise<SnapshotResult> snapshotDone;
  std::future<SnapshotResult> done = snapshotDone.get_future();
  std::thread snapshotThread([&] {
    snapshotStarted.set_value();
    gpu.snapshotDisplayBuffer(77u);
    std::vector<Color16> copy(11u * GPU::VRAM_WIDTH + 12u);
    SnapshotResult result;
    gpu.copyDisplayVRAM(copy.data(), copy.size(), &result.state);
    result.firstPixel = copy[10u * GPU::VRAM_WIDTH + 10u].raw;
    result.secondPixel = copy[10u * GPU::VRAM_WIDTH + 11u].raw;
    snapshotDone.set_value(result);
  });

  started.wait();
  const bool snapshotBlocked = done.wait_for(std::chrono::milliseconds(100)) ==
                               std::future_status::timeout;

  gpu.writeGP0(0x6800FF00u);
  gpu.writeGP0((10u << 16) | 11u);
  gpu.endCommandBatch();

  const std::future_status completion = done.wait_for(std::chrono::seconds(2));
  SnapshotResult result;
  if (completion == std::future_status::ready)
    result = done.get();
  snapshotThread.join();

  EXPECT_TRUE(snapshotBlocked)
      << "the VBlank latch completed while a command batch was still open";
  ASSERT_EQ(completion, std::future_status::ready);
  EXPECT_EQ(result.firstPixel, 0x001Fu);
  EXPECT_EQ(result.secondPixel, 0x03E0u)
      << "the latched frame exposed only the first command in the batch";
  EXPECT_EQ(result.state.completedSubmissionId, 1u);
  EXPECT_EQ(result.state.latchFrame, 77u);
}
