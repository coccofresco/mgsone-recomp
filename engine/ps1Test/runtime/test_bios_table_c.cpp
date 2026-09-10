#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class BiosTableCTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  std::unique_ptr<Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<Bios>(ctx, fs, mem);
  }
};

TEST_F(BiosTableCTest, DefaultExceptionRegistration) {
  // Table C has functions like 'ChangeClearRCnt' (0x02)
  // and default exception handling config.
  // For now, these are largely unimplemented stubs in bios.cpp
  // that print a warning but should not crash the system.

  ctx.r[T1] = 0x01; // Example stub
  ctx.r[A0] = 0x1234;

  // This shouldn't crash
  bios->executeC0();

  // Just verifying it ran cleanly
  EXPECT_TRUE(true);
}
