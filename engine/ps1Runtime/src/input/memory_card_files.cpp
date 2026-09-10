// Memory-card directory/chain algorithms adapted from RecompOne (MIT).
// Copyright (c) 2026 flaffy. See third_party/recompone/LICENSE and provenance.
#include "runtime/input/input.h"
#include <algorithm>
#include <filesystem>

namespace ps1::input {
void MemoryCard::fixChecksum(unsigned offset) {
  uint8_t sum = 0;
  for (unsigned i = 0; i < 127; ++i)
    sum ^= data_[offset + i];
  data_[offset + 127] = sum;
}
bool MemoryCard::attachFile(const std::string &path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error || (exists && !loadFromFile(path)))
    return false;
  if (!exists && !saveToFile(path))
    return false;
  backingPath_ = path;
  return true;
}
bool MemoryCard::flush() const {
  return backingPath_.empty() || saveToFile(backingPath_);
}
std::string MemoryCard::fileName(unsigned block) const {
  if (block < 1 || block > NUM_BLOCKS)
    return {};
  const unsigned offset = block * SECTOR_SIZE + 10;
  unsigned length = 0;
  while (length < 20 && data_[offset + length])
    ++length;
  return {reinterpret_cast<const char *>(data_.data() + offset), length};
}
int MemoryCard::findFile(const std::string &name) const {
  for (unsigned b = 1; b <= NUM_BLOCKS; ++b)
    if (data_[b * SECTOR_SIZE] == 0x51 && fileName(b) == name)
      return b;
  return 0;
}
uint32_t MemoryCard::fileSize(unsigned block) const {
  if (block < 1 || block > NUM_BLOCKS)
    return 0;
  const unsigned o = block * SECTOR_SIZE + 4;
  return uint32_t(data_[o]) | uint32_t(data_[o + 1]) << 8 |
         uint32_t(data_[o + 2]) << 16 | uint32_t(data_[o + 3]) << 24;
}
std::vector<unsigned> MemoryCard::fileChain(unsigned first) const {
  std::vector<unsigned> result;
  while (first >= 1 && first <= NUM_BLOCKS) {
    if (std::find(result.begin(), result.end(), first) != result.end())
      return {};
    result.push_back(first);
    const unsigned offset = first * SECTOR_SIZE;
    const unsigned next = data_[offset + 8] | unsigned(data_[offset + 9]) << 8;
    if (next == 0xffff)
      return result;
    first = next + 1;
  }
  return {}; // malformed chain, do not expose partial files
}
int MemoryCard::createFile(const std::string &name, unsigned blocks) {
  if (name.empty() || name.size() > 20 || findFile(name))
    return 0;
  blocks = std::max(1u, blocks);
  if (blocks > NUM_BLOCKS)
    return 0;
  std::vector<unsigned> free;
  for (unsigned b = 1; b <= NUM_BLOCKS && free.size() < blocks; ++b)
    if ((data_[b * SECTOR_SIZE] & 0xf0) == 0xa0)
      free.push_back(b);
  if (free.size() != blocks)
    return 0;
  const auto old = data_;
  for (unsigned i = 0; i < blocks; ++i) {
    const unsigned b = free[i], offset = b * SECTOR_SIZE;
    std::fill_n(data_.begin() + offset, SECTOR_SIZE, 0);
    data_[offset] = i == 0 ? 0x51 : (i + 1 == blocks ? 0x53 : 0x52);
    const unsigned next = i + 1 == blocks ? 0xffff : free[i + 1] - 1;
    data_[offset + 8] = next;
    data_[offset + 9] = next >> 8;
    if (i == 0) {
      const uint32_t size = blocks * BLOCK_SIZE;
      for (unsigned j = 0; j < 4; ++j)
        data_[offset + 4 + j] = size >> (j * 8);
      std::copy(name.begin(), name.end(), data_.begin() + offset + 10);
    }
    fixChecksum(offset);
    std::fill_n(data_.begin() + b * BLOCK_SIZE, BLOCK_SIZE, 0);
  }
  if (!flush()) {
    data_ = old;
    return 0;
  }
  return free[0];
}
bool MemoryCard::eraseFile(const std::string &name) {
  const auto chain = fileChain(findFile(name));
  if (chain.empty())
    return false;
  const auto old = data_;
  for (auto b : chain) {
    data_[b * SECTOR_SIZE] = 0xa0;
    fixChecksum(b * SECTOR_SIZE);
  }
  if (!flush()) {
    data_ = old;
    return false;
  }
  return true;
}
int MemoryCard::readFile(unsigned first, unsigned offset, uint8_t *out,
                         unsigned size) const {
  const auto chain = fileChain(first);
  if (chain.empty() || fileSize(first) > chain.size() * BLOCK_SIZE)
    return -1;
  if (offset >= fileSize(first))
    return 0;
  size = std::min(size, fileSize(first) - offset);
  for (unsigned i = 0; i < size; ++i) {
    const unsigned pos = offset + i;
    out[i] = data_[chain[pos / BLOCK_SIZE] * BLOCK_SIZE + pos % BLOCK_SIZE];
  }
  return size;
}
int MemoryCard::writeFile(unsigned first, unsigned offset, const uint8_t *input,
                          unsigned size) {
  const auto chain = fileChain(first);
  if (chain.empty() || fileSize(first) > chain.size() * BLOCK_SIZE)
    return -1;
  if (offset >= fileSize(first))
    return 0;
  size = std::min(size, fileSize(first) - offset);
  const auto old = data_;
  for (unsigned i = 0; i < size; ++i) {
    const unsigned pos = offset + i;
    data_[chain[pos / BLOCK_SIZE] * BLOCK_SIZE + pos % BLOCK_SIZE] = input[i];
  }
  if (!flush()) {
    data_ = old;
    return -1;
  }
  return size;
}
} // namespace ps1::input
