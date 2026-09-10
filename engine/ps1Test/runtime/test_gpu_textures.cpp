#include "runtime/gpu/gpu.h"
#include "runtime/metrics.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

using namespace ps1::gpu;

namespace {

ps1::PGXPValue texturePrecision(uint32_t word, float x, float y, float depth) {
  ps1::PGXPValue value;
  value.x = x;
  value.y = y;
  value.z = depth;
  value.value = word;
  value.flags = ps1::PGXPValue::VALID_XY | ps1::PGXPValue::VALID_Z;
  return value;
}

ps1::PGXPValue projectiveTexturePrecision(uint32_t word, double qx, double qy,
                                          double depth,
                                          uint64_t projectionId) {
  ps1::PGXPValue value;
  value.x = static_cast<float>(static_cast<int16_t>(word));
  value.y = static_cast<float>(static_cast<int16_t>(word >> 16));
  value.z = depth;
  value.qx = qx;
  value.qy = qy;
  value.nearDepth = 1.0f;
  value.projectionId = projectionId;
  value.projectionContext = 0x12345678u;
  value.value = word;
  value.origin = ps1::PGXPOrigin::GteProjection;
  value.flags = ps1::PGXPValue::VALID_XY |
                ps1::PGXPValue::VALID_SIGNED_PROJECTION;
  if (depth > 0.0f)
    value.flags |= ps1::PGXPValue::VALID_Z;
  return value;
}

void setupPerspectiveTexture(GPU &gpu) {
  // Direct-color page x=64: U=0..3 red, U=4..7 green.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64);
  gpu.writeGP0((1u << 16) | 8u);
  gpu.writeGP0(0x001F001F);
  gpu.writeGP0(0x001F001F);
  gpu.writeGP0(0x03E003E0);
  gpu.writeGP0(0x03E003E0);
}

void drawPerspectiveTextureTriangle(GPU &gpu, bool completeProvenance) {
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 18u;
  constexpr uint32_t v2 = (18u << 16) | 10u;
  const ps1::PGXPValue p0 = texturePrecision(v0, 10.0f, 10.0f, 1.0f);
  const ps1::PGXPValue p1 = texturePrecision(v1, 18.0f, 10.0f, 10.0f);
  const ps1::PGXPValue p2 = texturePrecision(v2, 10.0f, 18.0f, 1.0f);

  gpu.writeGP0(0x25000000); // raw, opaque textured triangle
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(0); // UV=(0,0), CLUT unused for direct color
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0((0x0101u << 16) | 8u); // U=8, page x=64, 15-bit
  gpu.writeGP0(v2, UINT32_MAX, completeProvenance ? &p2 : nullptr);
  gpu.writeGP0(0); // UV=(0,0)
}

void drawPreciseRawTexturedTriangle(GPU &gpu, uint32_t command, uint16_t tpage,
                                    float depth) {
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = texturePrecision(v0, 10.0f, 10.0f, depth);
  const ps1::PGXPValue p1 = texturePrecision(v1, 20.0f, 10.0f, depth);
  const ps1::PGXPValue p2 = texturePrecision(v2, 10.0f, 20.0f, depth);

  gpu.writeGP0(command);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(0); // UV=(0,0), CLUT unused for direct color
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(static_cast<uint32_t>(tpage) << 16); // UV=(0,0) + tpage
  gpu.writeGP0(v2, UINT32_MAX, &p2);
  gpu.writeGP0(0); // UV=(0,0)
}

void drawPreciseMonoTriangle(GPU &gpu, uint32_t command, float depth) {
  constexpr std::array<uint32_t, 3> vertices{
      (10u << 16) | 10u, (10u << 16) | 20u, (20u << 16) | 10u};
  gpu.writeGP0(command);
  for (const uint32_t vertex : vertices) {
    const ps1::PGXPValue precision = texturePrecision(
        vertex, static_cast<float>(static_cast<int16_t>(vertex)),
        static_cast<float>(static_cast<int16_t>(vertex >> 16)), depth);
    gpu.writeGP0(vertex, UINT32_MAX, &precision);
  }
}

Color32 modernTexturePixel(GPU &gpu, uint32_t x, uint32_t y) {
  gpu.snapshotDisplayBuffer();
  std::vector<Color32> display;
  uint32_t width = 0;
  uint32_t height = 0;
  EXPECT_TRUE(gpu.copyDisplayRGBA8(display, width, height));
  EXPECT_LT(x, width);
  EXPECT_LT(y, height);
  return display[static_cast<std::size_t>(y) * width + x];
}

} // namespace

TEST(ModernUVDomainTest, NormalTriangleLimitsEachNonConstantAxis) {
  constexpr std::array<ModernUVInput, 3> inputs = {
      ModernUVInput{10.0f, 10.0f, 1.0f, 0, 0},
      ModernUVInput{20.0f, 10.0f, 2.0f, 10, 0},
      ModernUVInput{10.0f, 20.0f, 3.0f, 0, 10},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 4, false);

  ASSERT_TRUE(domain.enabled);
  EXPECT_DOUBLE_EQ(domain.minU, 0.0);
  EXPECT_DOUBLE_EQ(domain.maxU, 9.0);
  EXPECT_DOUBLE_EQ(domain.minV, 0.0);
  EXPECT_DOUBLE_EQ(domain.maxV, 9.0);
  EXPECT_EQ(domain.offsetU, 0u);
  EXPECT_EQ(domain.offsetV, 0u);
}

TEST(ModernUVDomainTest, UpscaledUniformWQuadCorrectsBothFlippedAxes) {
  constexpr std::array<ModernUVInput, 4> inputs = {
      ModernUVInput{10.0f, 10.0f, 2.0f, 10, 10},
      ModernUVInput{20.0f, 10.0f, 2.0f, 0, 10},
      ModernUVInput{10.0f, 20.0f, 2.0f, 10, 0},
      ModernUVInput{20.0f, 20.0f, 2.0f, 0, 0},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 4, true);

  ASSERT_TRUE(domain.enabled);
  EXPECT_TRUE(domain.flippedU);
  EXPECT_TRUE(domain.flippedV);
  EXPECT_EQ(domain.offsetU, 1u);
  EXPECT_EQ(domain.offsetV, 1u);
  EXPECT_DOUBLE_EQ(domain.minU, 1.0);
  EXPECT_DOUBLE_EQ(domain.maxU, 10.0);
  EXPECT_DOUBLE_EQ(domain.minV, 1.0);
  EXPECT_DOUBLE_EQ(domain.maxV, 10.0);
}

TEST(ModernUVDomainTest, NativeScaleDoesNotCorrectFlippedQuad) {
  constexpr std::array<ModernUVInput, 4> inputs = {
      ModernUVInput{10.0f, 10.0f, 2.0f, 10, 10},
      ModernUVInput{20.0f, 10.0f, 2.0f, 0, 10},
      ModernUVInput{10.0f, 20.0f, 2.0f, 10, 0},
      ModernUVInput{20.0f, 20.0f, 2.0f, 0, 0},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 1, true);

  EXPECT_FALSE(domain.flippedU);
  EXPECT_FALSE(domain.flippedV);
  EXPECT_EQ(domain.offsetU, 0u);
  EXPECT_EQ(domain.offsetV, 0u);
  EXPECT_DOUBLE_EQ(domain.minU, 0.0);
  EXPECT_DOUBLE_EQ(domain.maxU, 9.0);
  EXPECT_DOUBLE_EQ(domain.minV, 0.0);
  EXPECT_DOUBLE_EQ(domain.maxV, 9.0);
}

TEST(ModernUVDomainTest, NonUniformFirstTriangleWRejectsFlippedQuadCorrection) {
  constexpr std::array<ModernUVInput, 4> inputs = {
      ModernUVInput{10.0f, 10.0f, 2.0f, 10, 10},
      ModernUVInput{20.0f, 10.0f, 2.0f, 0, 10},
      ModernUVInput{10.0f, 20.0f, 3.0f, 10, 0},
      ModernUVInput{20.0f, 20.0f, 2.0f, 0, 0},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 4, true);

  EXPECT_FALSE(domain.flippedU);
  EXPECT_FALSE(domain.flippedV);
  EXPECT_EQ(domain.offsetU, 0u);
  EXPECT_EQ(domain.offsetV, 0u);
  EXPECT_DOUBLE_EQ(domain.maxU, 9.0);
  EXPECT_DOUBLE_EQ(domain.maxV, 9.0);
}

TEST(ModernUVDomainTest, FourthVertexWDoesNotAffectDonorFlipDecision) {
  constexpr std::array<ModernUVInput, 4> inputs = {
      ModernUVInput{10.0f, 10.0f, 2.0f, 10, 10},
      ModernUVInput{20.0f, 10.0f, 2.0f, 0, 10},
      ModernUVInput{10.0f, 20.0f, 2.0f, 10, 0},
      ModernUVInput{20.0f, 20.0f, 3.0f, 0, 0},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 4, true);

  EXPECT_TRUE(domain.flippedU);
  EXPECT_TRUE(domain.flippedV);
  EXPECT_EQ(domain.offsetU, 1u);
  EXPECT_EQ(domain.offsetV, 1u);
}

TEST(ModernUVDomainTest, FractionalPgxpXYPreventsPackedAxisFalsePositive) {
  constexpr std::array<ModernUVInput, 4> inputs = {
      ModernUVInput{10.0f, 10.0f, 2.0f, 10, 10},
      ModernUVInput{20.0f, 10.0f, 2.0f, 0, 10},
      ModernUVInput{10.25f, 20.0f, 2.0f, 10, 0},
      ModernUVInput{20.0f, 20.0f, 2.0f, 0, 0},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 4, true);

  EXPECT_FALSE(domain.flippedU)
      << "the donor tests the non-zero PGXP dU/dY, not packed integer XY";
  EXPECT_TRUE(domain.flippedV);
  EXPECT_EQ(domain.offsetU, 0u);
  EXPECT_EQ(domain.offsetV, 1u);
}

TEST(ModernUVDomainTest, FullByteRangeStopsAtTexel254BeforeWrapping) {
  constexpr std::array<ModernUVInput, 3> inputs = {
      ModernUVInput{0.0f, 0.0f, 1.0f, 0, 0},
      ModernUVInput{10.0f, 0.0f, 1.0f, 255, 0},
      ModernUVInput{0.0f, 10.0f, 1.0f, 0, 255},
  };
  const ModernUVDomain domain =
      prepareModernUVDomain(inputs.data(), inputs.size(), 4, false);

  ASSERT_TRUE(domain.enabled);
  EXPECT_DOUBLE_EQ(domain.minU, 0.0);
  EXPECT_DOUBLE_EQ(domain.maxU, 254.0);
  EXPECT_DOUBLE_EQ(domain.minV, 0.0);
  EXPECT_DOUBLE_EQ(domain.maxV, 254.0);
}

class GpuTexturesTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuTexturesTest, TexturedTriangle4BitWithCLUT) {
  // Disable Dithering explicitly for reliable color hex match
  gpu.writeGP0(0xE1000000);

  // Setup CLUT at 0, 480 (CLUT x=0, y=480) -> CLUT code = (480 << 6) | (0 >> 4)
  // = 0x7800 CLUT is 16 colors. We'll set color 0 = transparent, color 1 =
  // 0xAA55
  gpu.writeGP0(0xA0000000);      // CPU to VRAM
  gpu.writeGP0((480 << 16) | 0); // Y=480, X=0
  gpu.writeGP0((1 << 16) | 16);  // H=1, W=16 (one 16-entry CLUT)
  gpu.writeGP0(0xAA550000);      // P1=0xAA55 (idx 1), P0=0x0000 (idx 0)
  gpu.writeGP0(0);               // the rest 0
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);
  gpu.writeGP0(0);

  // Setup Texture Page at X=64 (TPage=1, base 64, 4-bit) (TPage code 0x01)
  gpu.writeGP0(0xA0000000);     // CPU to VRAM
  gpu.writeGP0((0 << 16) | 64); // Y=0, X=64
  gpu.writeGP0((1 << 16) | 1);  // H=1, W=1 (2 words, 4 pixels)
  gpu.writeGP0(0x00000100); // Pixel 0 = idx 0, Pixel 1 = idx 1, Pixel 2 = idx
                            // 0, Pixel 3 = idx 0
  // Note: 0x0100 is 0000 0001 0000 0000 -> Wait, little endian 4-bit indices.
  // 0x0100 has 1 at the 8th bit, meaning it's the 3rd nibble (Pixel 2).
  // Wait, let's just make it 0x00000010 -> Pixel 1 = idx 1

  // Actually, write a block of entirely index 1: 0x11111111 0x11111111
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0((0 << 16) | 64);
  gpu.writeGP0((10 << 16) | 20); // 20x10 packed 4-bit texture indices
  for (int i = 0; i < 100; i++)
    gpu.writeGP0(0x11111111);

  // Draw Textured Triangle (0x24)
  // V0: 10,10. UV: 0,0. CLUT: 0x7800
  // V1: 20,10. UV: 10,0. TPage: 0x0001 (4-bit, base 64,0)
  // V2: 10,20. UV: 0,10.
  // 0x80 is the PS1 texture-modulation identity value. A zero command color
  // correctly produces black and used to pass only because modulation was
  // accidentally ignored.
  gpu.writeGP0(0x24808080);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((0x7800 << 16) | (0 << 8) | 0);
  gpu.writeGP0((10 << 16) | 20);
  gpu.writeGP0((0x0001 << 16) | (0 << 8) | 10);
  gpu.writeGP0((20 << 16) | 10);
  gpu.writeGP0((10 << 8) | 0);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[10 * 1024 + 10].raw, 0xAA55);
}

TEST_F(GpuTexturesTest, SemiTransparentSpriteBlendsOnlyStpTexels) {
  // Direct-color texture page at x=64: opaque red followed by STP red.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64);
  gpu.writeGP0((1 << 16) | 2);
  gpu.writeGP0(0x801F001F);

  // Green background at (10,10)..(11,10).
  gpu.writeGP0(0x6000FF00);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((1 << 16) | 2);

  // Page x=64, 15-bit, ABR 0. Draw a semi-transparent modulated sprite.
  gpu.writeGP0(0xE1000101);
  gpu.writeGP0(0x66808080);
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0(0); // UV=(0,0), unused CLUT
  gpu.writeGP0((1 << 16) | 2);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[10 * 1024 + 10].raw, 0x001F)
      << "STP=0 texel must remain opaque";
  EXPECT_EQ(vram[10 * 1024 + 11].raw & 0x7FFF, 15 | (15 << 5))
      << "STP=1 texel must use ABR blending";
}

TEST_F(GpuTexturesTest, TexturedPolygonUsesPacketTpageBlendMode) {
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64);
  gpu.writeGP0((1 << 16) | 1);
  gpu.writeGP0(0x0000801F); // one STP red texel; high half is padding

  gpu.writeGP0(0x6000FF00); // green destination triangle area
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0((5 << 16) | 5);

  // Deliberately select ABR 1 globally, then ABR 2 in the polygon tpage.
  gpu.writeGP0(0xE1000121);
  gpu.writeGP0(0x27000000); // raw + semi-transparent textured triangle
  gpu.writeGP0((10 << 16) | 10);
  gpu.writeGP0(0);
  gpu.writeGP0((10 << 16) | 14);
  gpu.writeGP0((0x0141u << 16)); // page x=64, ABR 2, 15-bit
  gpu.writeGP0((14 << 16) | 10);
  gpu.writeGP0(0);

  const auto pixel = gpu.getVRAM()[11 * 1024 + 11].raw & 0x7FFF;
  EXPECT_EQ(pixel, 31 << 5)
      << "polygon must use tpage ABR (background minus foreground), not E1";
}

TEST_F(GpuTexturesTest, TexturedPolygonLatchesTpageForFollowingSprite) {
  // Page x=64 is green; page x=128 is red. Both are direct-color pages.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64);
  gpu.writeGP0((1u << 16) | 1u);
  gpu.writeGP0(0x000003E0);
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(128);
  gpu.writeGP0((1u << 16) | 1u);
  gpu.writeGP0(0x0000001F);

  gpu.writeGP0(0xE1000101); // Start on direct-color page x=64.

  // The polygon carries direct-color page x=128 in its tpage word.
  gpu.writeGP0(0x25000000);
  gpu.writeGP0((10u << 16) | 10u);
  gpu.writeGP0(0);
  gpu.writeGP0((10u << 16) | 12u);
  gpu.writeGP0(0x0102u << 16);
  gpu.writeGP0((12u << 16) | 10u);
  gpu.writeGP0(0);

  // Sprites have no tpage word and must consume the polygon-latched page.
  gpu.writeGP0(0x65000000);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0(0);
  gpu.writeGP0((1u << 16) | 1u);

  EXPECT_EQ(gpu.getVRAM()[30 * GPU::VRAM_WIDTH + 30].raw, 0x001F);
  EXPECT_EQ(gpu.readGPUSTAT() & 0x1FFu, 0x0102u);
}

TEST_F(GpuTexturesTest, TexturedPolygonLatchesAbrForFollowingPrimitive) {
  // Green destination for an untextured semi-transparent red rectangle.
  gpu.writeGP0(0x6000FF00);
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((1u << 16) | 1u);

  gpu.writeGP0(0xE1000101); // Global ABR 0 before the polygon.

  // The texture may be transparent; executing the polygon must still latch
  // ABR 1 from its embedded tpage (0x20).
  gpu.writeGP0(0x25000000);
  gpu.writeGP0((10u << 16) | 10u);
  gpu.writeGP0(0);
  gpu.writeGP0((10u << 16) | 12u);
  gpu.writeGP0(0x0121u << 16);
  gpu.writeGP0((12u << 16) | 10u);
  gpu.writeGP0(0);

  gpu.writeGP0(0x620000FF); // semi-transparent untextured red
  gpu.writeGP0((30u << 16) | 30u);
  gpu.writeGP0((1u << 16) | 1u);

  EXPECT_EQ(gpu.getVRAM()[30 * GPU::VRAM_WIDTH + 30].raw & 0x7FFF,
            31u | (31u << 5))
      << "following primitive must use polygon-latched additive ABR";
}

TEST_F(GpuTexturesTest, OpaqueStpTexelParticipatesInTrueDepth) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);

  // One direct-color STP red texel at page x=64.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64);
  gpu.writeGP0((1u << 16) | 1u);
  gpu.writeGP0(0x0000801F);

  drawPreciseRawTexturedTriangle(gpu, 0x25000000, 0x0101, 1.0f);

  // A farther opaque polygon must not overwrite it. STP has no blending
  // meaning because the textured packet above is opaque.
  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 = texturePrecision(v0, 10.0f, 10.0f, 10.0f);
  const ps1::PGXPValue p1 = texturePrecision(v1, 20.0f, 10.0f, 10.0f);
  const ps1::PGXPValue p2 = texturePrecision(v2, 10.0f, 20.0f, 10.0f);
  gpu.writeGP0(0x20FF0000);
  gpu.writeGP0(v0, UINT32_MAX, &p0);
  gpu.writeGP0(v1, UINT32_MAX, &p1);
  gpu.writeGP0(v2, UINT32_MAX, &p2);

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00)
      << "PGXP depth must never change native painter-ordered VRAM";
  EXPECT_EQ(modernTexturePixel(gpu, 12, 12), (Color32{255, 0, 0, 255}))
      << "opaque STP texels must still participate in modern depth";
}

TEST_F(GpuTexturesTest, TexturedRectangleHonorsE1XYFlipBits) {
  // A B / C D at direct-color texture page x=64.
  gpu.writeGP0(0xA0000000);
  gpu.writeGP0(64);
  gpu.writeGP0((2 << 16) | 2);
  gpu.writeGP0(0x03E0001F); // A=red, B=green
  gpu.writeGP0(0x7FFF7C00); // C=blue, D=white

  // Direct 15-bit page with both rectangle flip bits. UV=(1,1) is the
  // top-left source and decrementing produces D C / B A.
  gpu.writeGP0(0xE1003101);
  gpu.writeGP0(0x65000000); // raw textured variable rectangle
  gpu.writeGP0((30 << 16) | 30);
  gpu.writeGP0((1 << 8) | 1);
  gpu.writeGP0((2 << 16) | 2);

  const auto *vram = gpu.getVRAM();
  EXPECT_EQ(vram[30 * 1024 + 30].raw, 0x7FFF);
  EXPECT_EQ(vram[30 * 1024 + 31].raw, 0x7C00);
  EXPECT_EQ(vram[31 * 1024 + 30].raw, 0x03E0);
  EXPECT_EQ(vram[31 * 1024 + 31].raw, 0x001F);
}

TEST_F(GpuTexturesTest,
       EnhancedRasterRequiresTripleProvenanceAndCorrectsPerspectiveUV) {
  GPU legacy;
  EXPECT_FALSE(legacy.isEnhancedRasterEnabled());
  setupPerspectiveTexture(legacy);
  drawPerspectiveTextureTriangle(legacy, true);
  EXPECT_EQ(legacy.getVRAM()[11 * GPU::VRAM_WIDTH + 14].raw, 0x03E0)
      << "the default-off path must retain legacy affine UV interpolation";

  GPU incomplete;
  incomplete.setEnhancedRaster(true);
  setupPerspectiveTexture(incomplete);
  drawPerspectiveTextureTriangle(incomplete, false);
  EXPECT_EQ(incomplete.getVRAM()[11 * GPU::VRAM_WIDTH + 14].raw, 0x03E0)
      << "one missing precise vertex must fall back for the whole triangle";

  GPU enhanced;
  enhanced.configureEnhancedPresentation(1, true);
  enhanced.setEnhancedRaster(true);
  setupPerspectiveTexture(enhanced);
  drawPerspectiveTextureTriangle(enhanced, true);
  EXPECT_EQ(enhanced.getVRAM()[11 * GPU::VRAM_WIDTH + 14].raw, 0x03E0)
      << "native VRAM must remain affine even with complete provenance";
  EXPECT_EQ(modernTexturePixel(enhanced, 14, 11), (Color32{255, 0, 0, 255}))
      << "complete provenance must correct UV only in the modern companion";
}

TEST_F(GpuTexturesTest,
       EnhancedQuadSharesOneUvDomainAndCachesIdenticalFourXTexels) {
  gpu.configureEnhancedPresentation(4, true);
  gpu.setEnhancedRaster(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr std::array<uint32_t, 4> words = {
      (10u << 16) | 10u,
      (10u << 16) | 20u,
      (20u << 16) | 10u,
      (20u << 16) | 20u,
  };
  constexpr std::array<TexCoord, 4> texCoords = {
      TexCoord{10, 10}, TexCoord{0, 10}, TexCoord{10, 0}, TexCoord{0, 0}};
  std::array<ps1::PGXPValue, 4> precision{};
  for (std::size_t i = 0; i < precision.size(); ++i) {
    precision[i] = texturePrecision(
        words[i], static_cast<float>(static_cast<int16_t>(words[i])),
        static_cast<float>(static_cast<int16_t>(words[i] >> 16)), 2.0f);
  }

  gpu.writeGP0(0x2D000000u); // raw, opaque textured quad
  gpu.writeGP0(words[0], UINT32_MAX, &precision[0]);
  gpu.writeGP0(static_cast<uint32_t>(texCoords[0].v) << 8 | texCoords[0].u);
  gpu.writeGP0(words[1], UINT32_MAX, &precision[1]);
  gpu.writeGP0((0x0101u << 16) | (static_cast<uint32_t>(texCoords[1].v) << 8) |
               texCoords[1].u);
  gpu.writeGP0(words[2], UINT32_MAX, &precision[2]);
  gpu.writeGP0(static_cast<uint32_t>(texCoords[2].v) << 8 | texCoords[2].u);
  gpu.writeGP0(words[3], UINT32_MAX, &precision[3]);
  gpu.writeGP0(static_cast<uint32_t>(texCoords[3].v) << 8 | texCoords[3].u);
  gpu.snapshotDisplayBuffer();

  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.uv.limit.primitive"), 1u)
      << "both quad halves must share one packet domain";
  EXPECT_EQ(ps1::metrics::get("gpu.modern.uv.flip_u.primitive"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.uv.flip_v.primitive"), 1u);
  EXPECT_GT(ps1::metrics::get("gpu.modern.uv.clamp.sample"), 0u);
  const uint64_t cacheHits =
      ps1::metrics::get("gpu.modern.uv.sample_cache.hit");
  const uint64_t cacheMisses =
      ps1::metrics::get("gpu.modern.uv.sample_cache.miss");
  EXPECT_GT(cacheHits, 0u);
  EXPECT_GT(cacheMisses, 0u);
  EXPECT_EQ(cacheHits + cacheMisses,
            ps1::metrics::get("gpu.modern.raster.reject.transparent_texel"));

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuTexturesTest,
       NearClippedTexturedTrianglePreservesUvAndNativeAuthority) {
  GPU native;
  setupPerspectiveTexture(native);
  setupPerspectiveTexture(gpu);
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  constexpr uint32_t v0 = (10u << 16) | 10u;
  constexpr uint32_t v1 = (10u << 16) | 20u;
  constexpr uint32_t v2 = (20u << 16) | 10u;
  const ps1::PGXPValue p0 =
      projectiveTexturePrecision(v0, 10.0f, 10.0f, 0.0f, 1);
  const ps1::PGXPValue p1 =
      projectiveTexturePrecision(v1, 40.0f, 20.0f, 2.0f, 2);
  const ps1::PGXPValue p2 =
      projectiveTexturePrecision(v2, 20.0f, 40.0f, 2.0f, 3);

  auto draw = [](GPU &target, const ps1::PGXPValue *a,
                 const ps1::PGXPValue *b, const ps1::PGXPValue *c) {
    target.writeGP0(0x25000000u); // raw, opaque textured triangle
    target.writeGP0(v0, UINT32_MAX, a);
    target.writeGP0(0); // U=0, V=0
    target.writeGP0(v1, UINT32_MAX, b);
    target.writeGP0((0x0101u << 16) | 7u); // U=7, direct-color page x=64
    target.writeGP0(v2, UINT32_MAX, c);
    target.writeGP0(0); // U=0, V=0
  };
  draw(native, nullptr, nullptr, nullptr);
  draw(gpu, &p0, &p1, &p2);

  EXPECT_EQ(std::memcmp(native.getVRAM(), gpu.getVRAM(),
                        GPU::VRAM_WIDTH * GPU::VRAM_HEIGHT * sizeof(Color16)),
            0)
      << "homogeneous texture clipping changed authoritative native VRAM";
  gpu.snapshotDisplayBuffer();
  EXPECT_EQ(ps1::metrics::get("gpu.frame.precise_triangle"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.frame.compatibility_triangle"), 0u);

  std::vector<Color32> display;
  uint32_t width = 0;
  uint32_t height = 0;
  ASSERT_TRUE(gpu.copyDisplayRGBA8(display, width, height));
  uint32_t red = 0;
  uint32_t green = 0;
  for (uint32_t y = 8; y < 28; ++y) {
    for (uint32_t x = 8; x < 28; ++x) {
      const Color32 pixel = display[static_cast<std::size_t>(y) * width + x];
      red += pixel == Color32{255, 0, 0, 255};
      green += pixel == Color32{0, 255, 0, 255};
    }
  }
  EXPECT_GT(red, 0u);
  EXPECT_GT(green, 0u)
      << "UVs were not interpolated across the generated near-plane edges";

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuTexturesTest,
       EmptyRasterTelemetryAttributesTransparentTextureOnly) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepth(true);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  // Direct-color page x=64 remains zero-filled, so every covered sample
  // reaches the texture stage and terminates as a transparent texel.
  drawPreciseRawTexturedTriangle(gpu, 0x25000000u, 0x0101u, 2.0f);
  gpu.snapshotDisplayBuffer();

  const uint64_t covered = ps1::metrics::get("gpu.modern.raster.covered");
  ASSERT_GT(covered, 0u);
  EXPECT_EQ(
      ps1::metrics::get("gpu.modern.raster.reject.transparent_texel"),
      covered);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.sample"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty"), 1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty.transparent_only"),
            1u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty.depth_only"), 0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.empty.mixed"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}

TEST_F(GpuTexturesTest,
       HybridTransparentTexelsCannotHideFollowingOtOpaque) {
  gpu.configureEnhancedPresentation(1, true);
  gpu.setEnhancedRaster(true);
  gpu.setTrueDepthMode(TrueDepthMode::Hybrid);
  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(true);

  drawPreciseMonoTriangle(gpu, 0x200000FFu, 1.0f); // A: near red owner

  // B is a semi-transparent raw-textured painter packet, but its direct-color
  // page is still zero-filled. Every fragment is rejected before the
  // accepted-far write, so no visibility boundary sample is produced.
  drawPreciseRawTexturedTriangle(gpu, 0x27000000u, 0x0101u, 10.0f);
  drawPreciseMonoTriangle(gpu, 0x20FF0000u, 5.0f); // C: farther blue

  EXPECT_EQ(gpu.getVRAM()[12 * GPU::VRAM_WIDTH + 12].raw, 0x7C00u);
  EXPECT_EQ(modernTexturePixel(gpu, 12, 12), (Color32{0, 0, 255, 255}));
  EXPECT_GT(
      ps1::metrics::get("gpu.modern.raster.reject.transparent_texel"), 0u);
  EXPECT_EQ(ps1::metrics::get(
                "gpu.modern.depth.packet.blend_visibility_boundary"),
            1u);
  EXPECT_EQ(ps1::metrics::get(
                "gpu.modern.depth.sample.visibility_boundary"),
            0u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.depth.packet.eligible"), 2u);
  EXPECT_EQ(ps1::metrics::get("gpu.modern.raster.reject.depth"), 0u);

  ps1::metrics::reset();
  ps1::metrics::setEnabledForTesting(false);
}
