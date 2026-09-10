#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class BiosTableATest : public ::testing::Test {
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

  // Helper to write string to memory
  void writeString(uint32_t addr, const char *str) {
    while (*str) {
      mem.write8(addr++, *str++);
    }
    mem.write8(addr, '\0');
  }

  // Helper to read string from memory
  std::string readString(uint32_t addr) {
    std::string s;
    char c;
    while ((c = mem.read8(addr++)) != '\0') {
      s += c;
    }
    return s;
  }
};

TEST_F(BiosTableATest, Strlen) {
  writeString(0x1000, "hello world");
  ctx.r[A0] = 0x1000;

  ctx.r[T1] = 0x1B;
  bios->executeA0(); // strlen
  EXPECT_EQ(ctx.r[V0], 11);
}

TEST_F(BiosTableATest, Strcpy) {
  writeString(0x2000, "copy this");
  ctx.r[A0] = 0x1000; // dst
  ctx.r[A1] = 0x2000; // src

  ctx.r[T1] = 0x19;
  bios->executeA0(); // strcpy
  EXPECT_EQ(readString(0x1000), "copy this");
  EXPECT_EQ(ctx.r[V0], 0x1000);
}

TEST_F(BiosTableATest, Strcmp) {
  writeString(0x1000, "apple");
  writeString(0x2000, "apple");
  writeString(0x3000, "banana");

  // Equal
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 0x2000;
  ctx.r[T1] = 0x17;
  bios->executeA0(); // strcmp
  EXPECT_EQ(ctx.r[V0], 0);

  // Less than
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 0x3000;
  ctx.r[T1] = 0x17;
  bios->executeA0(); // strcmp
  EXPECT_LT(static_cast<int32_t>(ctx.r[V0]), 0);

  // Greater than
  ctx.r[A0] = 0x3000;
  ctx.r[A1] = 0x1000;
  ctx.r[T1] = 0x17;
  bios->executeA0(); // strcmp
  EXPECT_GT(static_cast<int32_t>(ctx.r[V0]), 0);
}

TEST_F(BiosTableATest, Strcat) {
  writeString(0x1000, "foo");
  writeString(0x2000, "bar");

  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 0x2000;
  ctx.r[T1] = 0x15;
  bios->executeA0(); // strcat

  EXPECT_EQ(readString(0x1000), "foobar");
}

TEST_F(BiosTableATest, Strncmp) {
  writeString(0x1000, "apple");
  writeString(0x2000, "applet");

  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 0x2000;
  ctx.r[A2] = 5;
  ctx.r[T1] = 0x18;
  bios->executeA0();       // strncmp
  EXPECT_EQ(ctx.r[V0], 0); // "apple" == "apple" for first 5 chars

  ctx.r[A2] = 6;
  ctx.r[T1] = 0x18;
  bios->executeA0();
  EXPECT_LT(static_cast<int32_t>(ctx.r[V0]), 0); // "apple" < "applet"
}

TEST_F(BiosTableATest, IndexAndRindex) {
  writeString(0x1000, "banana");

  // index (find first 'a')
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 'a';
  ctx.r[T1] = 0x1C;
  bios->executeA0(); // index
  EXPECT_EQ(ctx.r[V0], 0x1001);

  // rindex (find last 'a')
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 'a';
  ctx.r[T1] = 0x1D;
  bios->executeA0(); // rindex
  EXPECT_EQ(ctx.r[V0], 0x1005);

  // NotFound
  ctx.r[A1] = 'z';
  ctx.r[T1] = 0x1C;
  bios->executeA0();
  EXPECT_EQ(ctx.r[V0], 0);
}

TEST_F(BiosTableATest, MemcpyAndMemset) {
  // memset
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 0xAB;
  ctx.r[A2] = 4;
  ctx.r[T1] = 0x2B;
  bios->executeA0(); // memset
  EXPECT_EQ(mem.read32(0x1000), 0xABABABAB);

  // memcpy
  ctx.r[A0] = 0x2000; // dst
  ctx.r[A1] = 0x1000; // src
  ctx.r[A2] = 4;      // len
  ctx.r[T1] = 0x2A;
  bios->executeA0(); // memcpy
  EXPECT_EQ(mem.read32(0x2000), 0xABABABAB);
}
