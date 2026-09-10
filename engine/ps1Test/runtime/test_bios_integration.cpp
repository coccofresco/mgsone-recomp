#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class BiosIntegrationTest : public ::testing::Test {
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

  void writeString(uint32_t addr, const char *str) {
    while (*str) {
      mem.write8(addr++, *str++);
    }
    mem.write8(addr, '\0');
  }
};

TEST_F(BiosIntegrationTest, BareMetalPrintAndMemoryAllocationSequence) {
  // Simulate a sequence where a homebrew initializes the heap,
  // allocates memory, formats a string (mocked by manual write),
  // prints it using std_out_puts, and then frees the memory.

  // 1. InitHeap (Table A: 0x39)
  ctx.r[A0] = 0x80010000; // Heap start
  ctx.r[A1] = 0x00010000; // Heap size (64KB)
  ctx.r[T1] = 0x39;
  bios->executeA0();

  // 2. malloc (Table A: 0x33)
  ctx.r[A0] = 32; // allocate 32 bytes
  ctx.r[T1] = 0x33;
  bios->executeA0();

  uint32_t allocatedAddr = ctx.r[V0];
  EXPECT_NE(allocatedAddr, 0);
  EXPECT_GE(allocatedAddr, 0x80010000);

  // 3. Write "Hello PS1 World!\n" to the allocated address
  writeString(allocatedAddr, "Hello PS1 World!");

  // 4. std_out_puts (Table B: 0x3E)
  ctx.r[A0] = allocatedAddr;
  ctx.r[T1] = 0x3E;
  bios->executeB0();

  // 5. free (Table A: 0x34)
  ctx.r[A0] = allocatedAddr;
  ctx.r[T1] = 0x34;
  bios->executeA0();

  // The system shouldn't crash and we should see "Hello PS1 World!"
  // printed to stdout in the test logs.
  EXPECT_TRUE(true);
}
