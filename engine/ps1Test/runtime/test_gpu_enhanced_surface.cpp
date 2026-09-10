#include "runtime/gpu/enhanced_surface.h"
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

using namespace ps1::gpu;

TEST(GpuEnhancedSurfaceTest, ConfiguresTrueFourTimesPixelGrid) {
  EnhancedSurface surface;
  surface.configure(1024, 512, 4);
  EXPECT_EQ(surface.scale(), 4u);
  EXPECT_EQ(surface.width(), 4096u);
  EXPECT_EQ(surface.height(), 2048u);
}

TEST(GpuEnhancedSurfaceTest, MirrorsNativeColorMaskAndPixelFootprint) {
  EnhancedSurface surface;
  surface.configure(2, 2, 4);
  surface.mirrorNativePixel(1, 0, 0xFFFFu);

  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 4; x < 8; ++x) {
      EXPECT_EQ(surface.colorAt(x, y), (Color32{255, 255, 255, 255}));
      EXPECT_TRUE(surface.maskAt(x, y));
      EXPECT_EQ(surface.depthAt(x, y), std::numeric_limits<float>::infinity());
    }
  }
  EXPECT_EQ(surface.colorAt(3, 0), (Color32{0, 0, 0, 255}));
}

TEST(GpuEnhancedSurfaceTest, UsesTrueColorAndNearestDepth) {
  EnhancedSurface surface;
  surface.configure(1, 1, 4);

  ASSERT_TRUE(surface.writeSample(2, 2, Color32{1, 127, 254, 255}, false, false,
                                  false, 10.0f, true));
  EXPECT_EQ(surface.colorAt(2, 2), (Color32{1, 127, 254, 255}));
  EXPECT_FLOAT_EQ(surface.depthAt(2, 2), 10.0f);

  EXPECT_FALSE(surface.writeSample(2, 2, Color32{255, 0, 0, 255}, false, false,
                                   false, 11.0f, true));
  EXPECT_EQ(surface.colorAt(2, 2), (Color32{1, 127, 254, 255}));

  EXPECT_TRUE(surface.writeSample(2, 2, Color32{0, 255, 0, 255}, false, false,
                                  false, 9.0f, true));
  EXPECT_EQ(surface.colorAt(2, 2), (Color32{0, 255, 0, 255}));
}

TEST(GpuEnhancedSurfaceTest, AdvancingEpochLazilyHidesOldDepthAndAcceptsNewWrite) {
  EnhancedSurface surface;
  surface.configure(2, 1, 2);
  ASSERT_TRUE(surface.writeSample(1, 0, Color32{1, 2, 3, 255}, false, false,
                                  false, 2.0f, true));
  const auto before = surface.mutableRow(0);
  ASSERT_TRUE(before);
  ASSERT_EQ(before.depthEpoch[1], before.activeDepthEpoch);

  surface.advanceDepthEpoch();

  const auto after = surface.mutableRow(0);
  ASSERT_TRUE(after);
  EXPECT_NE(after.activeDepthEpoch, before.activeDepthEpoch);
  EXPECT_EQ(after.depthEpoch[1], before.activeDepthEpoch);
  EXPECT_TRUE(std::isinf(surface.depthAt(1, 0)));
  EXPECT_EQ(surface.colorAt(1, 0), (Color32{1, 2, 3, 255}));

  ASSERT_TRUE(surface.writeSample(1, 0, Color32{4, 5, 6, 255}, false, false,
                                  false, 50.0f, true));
  EXPECT_FLOAT_EQ(surface.depthAt(1, 0), 50.0f);
  EXPECT_EQ(after.depthEpoch[1], after.activeDepthEpoch);
}

TEST(GpuEnhancedSurfaceTest, KeepsMaskIndependentFromAlpha) {
  EnhancedSurface surface;
  surface.configure(1, 1, 2);
  ASSERT_TRUE(surface.writeSample(0, 0, Color32{10, 20, 30, 255}, true, false,
                                  false, 1.0f, false));
  EXPECT_TRUE(surface.maskAt(0, 0));
  EXPECT_EQ(surface.colorAt(0, 0).a, 255);
  EXPECT_FALSE(surface.writeSample(0, 0, Color32{40, 50, 60, 255}, false, false,
                                   true, 1.0f, false));
}

TEST(GpuEnhancedSurfaceTest, OrderedNativeWriteClearsDepthForItsBlock) {
  EnhancedSurface surface;
  surface.configure(1, 1, 2);
  ASSERT_TRUE(surface.writeSample(1, 1, Color32{1, 2, 3, 255}, false, false,
                                  false, 3.0f, true));
  surface.mirrorNativePixel(0, 0, 0x001Fu);

  EXPECT_EQ(surface.colorAt(1, 1), (Color32{255, 0, 0, 255}));
  EXPECT_EQ(surface.depthAt(1, 1), std::numeric_limits<float>::infinity());
}

TEST(GpuEnhancedSurfaceTest, ClearsOnlyTheScaledNativeDepthRectangle) {
  EnhancedSurface surface;
  surface.configure(4, 3, 2);
  for (uint32_t y = 0; y < surface.height(); ++y) {
    for (uint32_t x = 0; x < surface.width(); ++x)
      ASSERT_TRUE(surface.writeSample(x, y, {1, 2, 3, 255}, false, false,
                                      false, 7.0f, true));
  }

  surface.clearDepthNativeRect(1, 1, 2, 1);
  for (uint32_t y = 0; y < surface.height(); ++y) {
    for (uint32_t x = 0; x < surface.width(); ++x) {
      const bool cleared = x >= 2 && x < 6 && y >= 2 && y < 4;
      if (cleared)
        EXPECT_TRUE(std::isinf(surface.depthAt(x, y)));
      else
        EXPECT_FLOAT_EQ(surface.depthAt(x, y), 7.0f);
    }
  }
}

TEST(GpuEnhancedSurfaceTest, RegionalClearInvalidatesTagsWithoutAdvancingEpoch) {
  EnhancedSurface surface;
  surface.configure(3, 2, 2);
  for (uint32_t y = 0; y < surface.height(); ++y) {
    for (uint32_t x = 0; x < surface.width(); ++x)
      ASSERT_TRUE(surface.writeSample(x, y, {1, 2, 3, 255}, false, false,
                                      false, 7.0f, true));
  }
  const auto before = surface.mutableRow(0);
  ASSERT_TRUE(before);

  surface.clearDepthNativeRect(1, 0, 1, 1);

  const auto after = surface.mutableRow(0);
  ASSERT_TRUE(after);
  EXPECT_EQ(after.activeDepthEpoch, before.activeDepthEpoch);
  for (uint32_t y = 0; y < surface.height(); ++y) {
    for (uint32_t x = 0; x < surface.width(); ++x) {
      const bool cleared = x >= 2 && x < 4 && y < 2;
      if (cleared) {
        EXPECT_EQ(surface.mutableRow(y).depthEpoch[x], 0u);
        EXPECT_TRUE(std::isinf(surface.depthAt(x, y)));
      } else {
        EXPECT_EQ(surface.mutableRow(y).depthEpoch[x], after.activeDepthEpoch);
        EXPECT_FLOAT_EQ(surface.depthAt(x, y), 7.0f);
      }
    }
  }
}

TEST(GpuEnhancedSurfaceTest, CopiesTightlyPackedScaledDisplayRegion) {
  EnhancedSurface surface;
  surface.configure(4, 2, 2);
  surface.mirrorNativePixel(1, 0, 0x001Fu);
  surface.mirrorNativePixel(2, 0, 0x03E0u);

  std::vector<Color32> copy;
  surface.copyNativeRegion(1, 0, 2, 1, copy);
  ASSERT_EQ(copy.size(), 8u);
  EXPECT_EQ(copy[0], (Color32{255, 0, 0, 255}));
  EXPECT_EQ(copy[1], (Color32{255, 0, 0, 255}));
  EXPECT_EQ(copy[2], (Color32{0, 255, 0, 255}));
  EXPECT_EQ(copy[3], (Color32{0, 255, 0, 255}));
  EXPECT_EQ(copy[4], (Color32{255, 0, 0, 255}));
  EXPECT_EQ(copy[6], (Color32{0, 255, 0, 255}));
}

TEST(GpuEnhancedSurfaceTest, VramCopyPreservesTrueColorSamples) {
  EnhancedSurface surface;
  surface.configure(2, 1, 2);
  ASSERT_TRUE(surface.writeSample(0, 0, Color32{3, 129, 251, 255}, false, false,
                                  false, 2.0f, true));
  ASSERT_TRUE(surface.writeSample(1, 0, Color32{7, 131, 247, 255}, true, false,
                                  false, 2.0f, true));

  surface.copyNativePixel(0, 0, 1, 0, false);
  EXPECT_EQ(surface.colorAt(2, 0), (Color32{3, 129, 251, 255}));
  EXPECT_EQ(surface.colorAt(3, 0), (Color32{7, 131, 247, 255}));
  EXPECT_FALSE(surface.maskAt(2, 0));
  EXPECT_TRUE(surface.maskAt(3, 0));
  EXPECT_EQ(surface.depthAt(2, 0), std::numeric_limits<float>::infinity());
}

TEST(GpuEnhancedSurfaceTest, BulkFillScalesAndWrapsLikeNativeVram) {
  EnhancedSurface surface;
  surface.configure(4, 2, 2);
  surface.fillNativeRect(3, 1, 2, 1, Color32{9, 19, 29, 255});

  EXPECT_EQ(surface.colorAt(6, 2), (Color32{9, 19, 29, 255}));
  EXPECT_EQ(surface.colorAt(7, 3), (Color32{9, 19, 29, 255}));
  EXPECT_EQ(surface.colorAt(0, 2), (Color32{9, 19, 29, 255}));
  EXPECT_EQ(surface.colorAt(1, 3), (Color32{9, 19, 29, 255}));
  EXPECT_EQ(surface.colorAt(2, 2), (Color32{0, 0, 0, 255}));
}
