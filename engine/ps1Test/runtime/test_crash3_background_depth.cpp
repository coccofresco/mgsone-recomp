#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace {
uint32_t floatBitsToUint(float value) { return std::bit_cast<uint32_t>(value); }

// Test the actual shader helper, not a second CPU implementation. This
// deliberately small GLSL function is also valid scalar C++.
#include "../../ps1Runtime/shaders/games/crash3_background_depth.glsl"

constexpr float kBackgroundW = std::bit_cast<float>(UINT32_C(0x4f7fffff));
constexpr float kBackgroundSurface = std::bit_cast<float>(UINT32_C(0x3f7fffff));
constexpr std::array kProjectiveW{
    1.0f / 4096.0f, 1.0f / 7.0f, 3.0f / 7.0f, 0.5f, 1.0f, 1.75f,
    256.0f, 13734.0f, 32767.0f, 65536.0f, 1048575.0f, kBackgroundW};
} // namespace

TEST(Crash3BackgroundDepthTest, ReproducesFormerFarClassSelfOcclusion) {
  // The old Z = nextafter(1,0) * W rule produces unequal depths for two
  // supposedly coplanar triangles. The product is materialized as fp32 just
  // as it is in the Vulkan vertex-to-rasterizer interface.
  const float firstW = 1.0f / 7.0f;
  const float secondW = 3.0f / 7.0f;
  volatile float firstZ = kBackgroundSurface * firstW;
  volatile float secondZ = kBackgroundSurface * secondW;
  EXPECT_LT(firstZ / firstW, secondZ / secondW);
}

TEST(Crash3BackgroundDepthTest, BackgroundIsExactlyFarAtEveryProjectiveScale) {
  for (float w : kProjectiveW) {
    SCOPED_TRACE(w);
    const float z = crash3_clip_depth(kBackgroundSurface, w, 0.0f, kBackgroundW);
    EXPECT_EQ(floatBitsToUint(z), floatBitsToUint(w));
    EXPECT_EQ(floatBitsToUint(z / w), UINT32_C(0x3f800000));
    EXPECT_GT(z / w, kBackgroundSurface);
  }
}

TEST(Crash3BackgroundDepthTest, ClippedBackgroundEdgesStayExactlyFar) {
  for (float fromW : kProjectiveW) {
    for (float toW : kProjectiveW) {
      for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const float fromZ = crash3_clip_depth(kBackgroundSurface, fromW, 0.0f,
                                              kBackgroundW);
        const float toZ = crash3_clip_depth(kBackgroundSurface, toW, 0.0f,
                                            kBackgroundW);
        const float clippedW = (1.0f - t) * fromW + t * toW;
        const float clippedZ = (1.0f - t) * fromZ + t * toZ;
        EXPECT_EQ(floatBitsToUint(clippedZ), floatBitsToUint(clippedW));
      }
    }
  }
}

TEST(Crash3BackgroundDepthTest, LeavesEveryNonBackgroundDepthTransformUnchanged) {
  for (float w : kProjectiveW) {
    for (float depth : {0.0f, 0.0001f, 0.25f, 0.5f, kBackgroundSurface, 1.0f}) {
      for (const auto transform :
           {std::array{1.0f, 0.0f}, std::array{4.0f, 12800.0f},
            std::array{0.0f, 0.0f}, std::array{0.0f, 32768.0f},
            std::array{1.0f, kBackgroundW}}) {
        EXPECT_EQ(floatBitsToUint(crash3_clip_depth(depth, w, transform[0],
                                                   transform[1])),
                  floatBitsToUint(depth * w));
      }
    }
  }
}
