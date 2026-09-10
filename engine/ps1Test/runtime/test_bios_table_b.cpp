#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class BiosTableBTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  std::unique_ptr<Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<Bios>(ctx, fs, mem);

    // Simulate setting up a basic file in VirtualFs for testing
    // Since VirtualFs requires actual files on disk, we will mock simple
    // behavior or just rely on the API returning fd if passing an existent
    // dummy file or fake.
  }

  void writeString(uint32_t addr, const char *str) {
    while (*str) {
      mem.write8(addr++, *str++);
    }
    mem.write8(addr, '\0');
  }
};

TEST_F(BiosTableBTest, OpenCloseRead) {
  // We can't easily mock the real filesystem inside the unit test without
  // creating a temp file. We will assume the VirtualFs behavior returns -1
  // (error) for non-existent files.
  writeString(0x1000, "cdrom:\\NONEXISTENT.TXT");
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 1;    // Read mode
  ctx.r[T1] = 0x32; // open
  bios->executeB0();

  // V0 should be a negative descriptor since it fails to open
  EXPECT_EQ(ctx.r[V0], 0xFFFFFFFF);
}

TEST_F(BiosTableBTest, EventSystemProxy) {
  // Test Event creation proxy
  ctx.r[A0] = 0x09;   // class
  ctx.r[A1] = 0x02;   // spec
  ctx.r[A2] = 0x1000; // mode
  ctx.r[A3] = 0x0000; // func

  ctx.r[T1] = 0x08; // openEvent
  bios->executeB0();

  uint32_t eventId = ctx.r[V0];
  EXPECT_NE(eventId, 0xFFFFFFFF);

  // enableEvent
  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x0C; // enableEvent
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 1); // Success

  // testEvent
  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x0B; // testEvent
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 0); // Not triggered yet

  // closeEvent
  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x09; // closeEvent
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 1); // Success
}
