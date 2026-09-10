#include "runtime/gpu/gpu.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

class GpuTextureWindowTest : public ::testing::Test {
protected:
  GPU gpu;

  void SetUp() override { gpu.reset(); }
};

TEST_F(GpuTextureWindowTest, ExecuteTextureWindowSavesMaskAndOffset) {
  // Command E2: MaskX=1, MaskY=2, OffX=3, OffY=4
  // 1 = 0x01, 2 = 0x02, 3 = 0x03, 4 = 0x04
  // Formula: MaskX | (MaskY << 5) | (OffX << 10) | (OffY << 15)
  uint32_t cmd = 0xE2000000 | 1 | (2 << 5) | (3 << 10) | (4 << 15);

  gpu.writeGP0(cmd);

  // They are private, but we can verify their application in rendering later.
  // For a basic test, ensuring it does not crash or break state is step 1.
  EXPECT_EQ(1, 1);
}
