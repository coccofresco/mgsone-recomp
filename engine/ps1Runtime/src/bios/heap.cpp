#include "runtime/bios/heap.h"
#include <fmt/core.h>

namespace ps1::bios {

Heap::Heap(Memory &mem) : mem_(mem) {}

bool Heap::validSnapshot(const Snapshot &s) {
  if (s.baseAddress == 0) return s.totalSize == 0 && s.firstBlock == 0;
  return s.firstBlock == s.baseAddress && s.totalSize > BLOCK_HEADER_SIZE &&
         s.totalSize <= Memory::RAM_SIZE &&
         (s.baseAddress & 0x1fffffffu) <= Memory::RAM_SIZE - s.totalSize;
}

bool Heap::restore(const Snapshot &s) {
  if (!validSnapshot(s)) return false;
  // The block list belongs to restored guest RAM. InitHeap would destroy it.
  baseAddress_ = s.baseAddress;
  totalSize_ = s.totalSize;
  firstBlock_ = s.firstBlock;
  return true;
}

void Heap::initHeap(uint32_t address, uint32_t size) {
  if (size <= BLOCK_HEADER_SIZE) {
    fmt::print("[BIOS] Warning: InitHeap with size too small ({})\n", size);
    return;
  }

  baseAddress_ = address;
  totalSize_ = size;
  firstBlock_ = address;

  Block initialBlock;
  initialBlock.size = size - BLOCK_HEADER_SIZE;
  initialBlock.next = 0;
  initialBlock.prev = 0;
  initialBlock.isAllocated = false;

  writeBlock(firstBlock_, initialBlock);

  fmt::print("[BIOS] InitHeap at 0x{:08X} size 0x{:X}\n", address, size);
}

uint32_t Heap::malloc(uint32_t size) {
  if (baseAddress_ == 0 || size == 0 || size > totalSize_ - BLOCK_HEADER_SIZE ||
      size > UINT32_MAX - 3)
    return 0;

  // Align size to 4 bytes
  size = (size + 3) & ~3;

  uint32_t currAddr = firstBlock_;
  while (currAddr != 0) {
    Block block = readBlock(currAddr);

    if (!block.isAllocated && block.size >= size) {
      // Found a free block. Can we split it?
      if (block.size > size + BLOCK_HEADER_SIZE + 4) {
        uint32_t newBlockAddr = currAddr + BLOCK_HEADER_SIZE + size;
        Block newBlock;
        newBlock.size = block.size - size - BLOCK_HEADER_SIZE;
        newBlock.next = block.next;
        newBlock.prev = currAddr;
        newBlock.isAllocated = false;

        writeBlock(newBlockAddr, newBlock);

        // Update old block
        block.size = size;
        block.next = newBlockAddr;

        // Update next block's prev if needed
        if (newBlock.next != 0) {
          Block nextOfNew = readBlock(newBlock.next);
          nextOfNew.prev = newBlockAddr;
          writeBlock(newBlock.next, nextOfNew);
        }
      }

      block.isAllocated = true;
      writeBlock(currAddr, block);

      fmt::print("[BIOS] malloc(0x{:X}) -> 0x{:08X}\n", size,
                 currAddr + BLOCK_HEADER_SIZE);
      return currAddr + BLOCK_HEADER_SIZE;
    }

    currAddr = block.next;
  }

  fmt::print("[BIOS] malloc(0x{:X}) -> Out of Memory!\n", size);
  return 0; // OOM
}

void Heap::free(uint32_t pointer) {
  if (pointer == 0 || pointer < baseAddress_ + BLOCK_HEADER_SIZE)
    return;

  uint32_t blockAddr = pointer - BLOCK_HEADER_SIZE;
  Block block = readBlock(blockAddr);

  if (!block.isAllocated) {
    fmt::print("[BIOS] Warning: Double free or invalid free at 0x{:08X}\n",
               pointer);
    return;
  }

  block.isAllocated = false;
  fmt::print("[BIOS] free(0x{:08X})\n", pointer);

  // Merge with next if free
  if (block.next != 0) {
    Block nextBlock = readBlock(block.next);
    if (!nextBlock.isAllocated) {
      block.size += BLOCK_HEADER_SIZE + nextBlock.size;
      block.next = nextBlock.next;
      if (block.next != 0) {
        Block newNext = readBlock(block.next);
        newNext.prev = blockAddr;
        writeBlock(block.next, newNext);
      }
    }
  }

  // Merge with prev if free
  if (block.prev != 0) {
    Block prevBlock = readBlock(block.prev);
    if (!prevBlock.isAllocated) {
      prevBlock.size += BLOCK_HEADER_SIZE + block.size;
      prevBlock.next = block.next;
      if (block.next != 0) {
        Block newNext = readBlock(block.next);
        newNext.prev = block.prev;
        writeBlock(block.next, newNext);
      }
      writeBlock(block.prev, prevBlock);
      return; // We merged into prev, so our block struct is handled
    }
  }

  writeBlock(blockAddr, block);
}

uint32_t Heap::allocationSize(uint32_t pointer) {
  if (baseAddress_ == 0 || pointer < baseAddress_ + BLOCK_HEADER_SIZE)
    return 0;

  uint32_t current = firstBlock_;
  uint32_t remainingBlocks = totalSize_ / BLOCK_HEADER_SIZE + 1u;
  while (current != 0 && remainingBlocks-- != 0) {
    const Block block = readBlock(current);
    if (current + BLOCK_HEADER_SIZE == pointer)
      return block.isAllocated ? block.size : 0;
    if (block.next <= current || block.next >= baseAddress_ + totalSize_)
      break;
    current = block.next;
  }
  return 0;
}

Heap::Block Heap::readBlock(uint32_t addr) {
  Block b;
  uint32_t rawSize = mem_.read32(addr);
  b.size = rawSize & 0x7FFFFFFF;
  b.isAllocated = (rawSize & 0x80000000) != 0;
  b.next = mem_.read32(addr + 4);
  b.prev = mem_.read32(addr + 8);
  return b;
}

void Heap::writeBlock(uint32_t addr, const Block &block) {
  uint32_t rawSize = block.size | (block.isAllocated ? 0x80000000 : 0);
  mem_.write32(addr, rawSize);
  mem_.write32(addr + 4, block.next);
  mem_.write32(addr + 8, block.prev);
}

} // namespace ps1::bios
