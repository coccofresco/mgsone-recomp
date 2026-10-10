#pragma once

#include "runtime/memory.h"
#include <cstdint>

namespace ps1::bios {

class Heap {
public:
  Heap(Memory &mem);
  ~Heap() = default;

  // Table A BIOS functions
  void initHeap(uint32_t address, uint32_t size);
  uint32_t malloc(uint32_t size);
  void free(uint32_t pointer);
  uint32_t allocationSize(uint32_t pointer);

  struct Snapshot {
    uint32_t baseAddress = 0, totalSize = 0, firstBlock = 0;
  };
  Snapshot snapshot() const { return {baseAddress_, totalSize_, firstBlock_}; }
  static bool validSnapshot(const Snapshot &state);
  bool restore(const Snapshot &state);

private:
  struct Block {
    uint32_t size;    // excluding header size. In PS1, usually MSB indicates
                      // allocation status
    uint32_t next;    // Address of the next block header
    uint32_t prev;    // Address of the previous block header
    bool isAllocated; // To keep it simpler than mingling with size MSB for the
                      // emulation initially
  };

  Memory &mem_;
  uint32_t baseAddress_ = 0;
  uint32_t totalSize_ = 0;
  uint32_t firstBlock_ = 0;

  // Helper functions
  Block readBlock(uint32_t addr);
  void writeBlock(uint32_t addr, const Block &block);

  static constexpr uint32_t BLOCK_HEADER_SIZE =
      12; // size(4) + next(4) + prev(4)
};

} // namespace ps1::bios
