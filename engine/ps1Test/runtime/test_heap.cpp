#include "runtime/bios/heap.h"
#include "runtime/memory.h"
#include <fmt/core.h>
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class HeapTest : public ::testing::Test {
protected:
  void SetUp() override {
    mem.reset();
    heap = std::make_unique<Heap>(mem);
  }

  Memory mem;
  std::unique_ptr<Heap> heap;
};

TEST_F(HeapTest, InitHeapCreatesFirstBlock) {
  uint32_t heapBase = 0x80100000;
  uint32_t heapSize = 0x10000; // 64KB

  heap->initHeap(heapBase, heapSize);

  // Read the block directly from memory to verify
  uint32_t rawSize = mem.read32(heapBase);
  uint32_t next = mem.read32(heapBase + 4);
  uint32_t prev = mem.read32(heapBase + 8);

  EXPECT_EQ(rawSize & 0x7FFFFFFF, heapSize - 12);
  EXPECT_EQ(rawSize & 0x80000000, 0); // Not allocated
  EXPECT_EQ(next, 0);
  EXPECT_EQ(prev, 0);
}

TEST_F(HeapTest, MallocReturnsValidAddress) {
  uint32_t heapBase = 0x80100000;
  uint32_t heapSize = 0x10000;
  heap->initHeap(heapBase, heapSize);

  uint32_t ptr = heap->malloc(0x100);
  EXPECT_EQ(ptr, heapBase + 12); // First block data starts after 12-byte header

  uint32_t rawSize = mem.read32(heapBase);
  EXPECT_TRUE((rawSize & 0x80000000) != 0); // Allocated flag set

  // Size should be 4-byte aligned
  uint32_t actualSize = rawSize & 0x7FFFFFFF;
  EXPECT_EQ(actualSize, 0x100);

  // Next block should exist
  uint32_t nextBlockAddr = mem.read32(heapBase + 4);
  EXPECT_EQ(nextBlockAddr, heapBase + 12 + 0x100);
  EXPECT_EQ(heap->allocationSize(ptr), 0x100u);
}

TEST_F(HeapTest, AllocationSizeRejectsFreedAndInteriorPointers) {
  constexpr uint32_t heapBase = 0x80100000u;
  heap->initHeap(heapBase, 0x10000u);
  const uint32_t ptr = heap->malloc(0x101u);

  EXPECT_EQ(heap->allocationSize(ptr), 0x104u);
  EXPECT_EQ(heap->allocationSize(ptr + 4u), 0u);
  EXPECT_EQ(heap->allocationSize(0x80001000u), 0u);

  heap->free(ptr);
  EXPECT_EQ(heap->allocationSize(ptr), 0u);
}

TEST_F(HeapTest, FreeMergesBlocksCorrectly) {
  uint32_t heapBase = 0x80100000;
  uint32_t heapSize = 0x10000;
  heap->initHeap(heapBase, heapSize);

  uint32_t ptr1 = heap->malloc(0x100);
  uint32_t ptr2 = heap->malloc(0x200);
  uint32_t ptr3 = heap->malloc(0x300);

  fmt::print("ptr1 addr: {:X}\n", ptr1);
  fmt::print("ptr2 addr: {:X}\n", ptr2);
  fmt::print("ptr3 addr: {:X}\n", ptr3);

  // Free middle block
  heap->free(ptr2);

  // The block at ptr2 should now be marked as free
  uint32_t block2Addr = ptr2 - 12;
  uint32_t rawSize2 = mem.read32(block2Addr);
  EXPECT_EQ(rawSize2 & 0x80000000, 0); // Not allocated

  // Free first block, it should merge with the middle block
  heap->free(ptr1);

  uint32_t block1Addr = ptr1 - 12;
  uint32_t rawSize1 = mem.read32(block1Addr);
  EXPECT_EQ(rawSize1 & 0x80000000, 0); // Not allocated

  uint32_t actualSize1 = rawSize1 & 0x7FFFFFFF;
  EXPECT_EQ(actualSize1, 0x100 + 12 + 0x200); // Merged size
}
