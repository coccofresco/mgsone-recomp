#include "runtime/games/mgs_asset_cache.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace ps1::games::mgs {
namespace {
uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(std::string("MGS asset cache: ") + message);
}
uint32_t crc32(std::span<const uint8_t> bytes) {
  static const auto table = [] {
    std::array<uint32_t, 256> result{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
      result[i] = crc;
    }
    return result;
  }();
  uint32_t crc = ~0u;
  for (auto v : bytes) crc = (crc >> 8) ^ table[(crc ^ v) & 255u];
  return ~crc;
}
bool range(uint32_t first, uint32_t count, uint32_t limit) {
  return first <= limit && count <= limit - first;
}
}
AssetCache AssetCache::load(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  check(bool(f), "cannot open file");
  const auto size = f.tellg();
  check(size >= 128 && size <= 1024ll * 1024 * 1024, "invalid file size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  f.seekg(0);
  check(bool(f.read(reinterpret_cast<char *>(bytes.data()), bytes.size())), "short read");
  return decode(std::move(bytes));
}
AssetCache AssetCache::decode(std::vector<uint8_t> bytes) {
  check(bytes.size() >= 128, "short header");
  check(std::memcmp(bytes.data(), "MGSAC001", 8) == 0, "unsupported schema");
  check(u32(bytes.data() + 8) == bytes.size(), "length mismatch");
  check(u32(bytes.data() + 12) == crc32(std::span(bytes).subspan(16)), "checksum mismatch");
  AssetCache result;
  static constexpr const char *tags[] = {"ASST", "MODL", "VRTX", "TEXR", "TEXI", "PLTE", "SRCE"};
  static constexpr uint32_t strides[] = {64, 32, 48, 32, 1, 4, 1};
  uint64_t end = 128;
  for (size_t s = 0; s < Count; ++s) {
    const auto *entry = bytes.data() + 16 + s * 16;
    check(std::memcmp(entry, tags[s], 4) == 0, "wrong section order");
    auto &section = result.sections_[s];
    section = {u32(entry + 4), u32(entry + 8), u32(entry + 12)};
    check(section.stride == strides[s], "invalid section stride");
    check(section.offset % 16 == 0 && section.offset >= end, "overlapping or unaligned section");
    end = uint64_t(section.offset) + section.size();
    check(end <= bytes.size(), "section outside file");
  }
  check(end == bytes.size(), "trailing bytes");
  result.bytes_ = std::move(bytes);
  const auto word = [&](Section s, uint32_t row, uint32_t w) { return result.word(s, row, w); };
  for (uint32_t a = 0; a < result.section(Assets).count; ++a) {
    const uint32_t kind = word(Assets, a, 8);
    check(kind == 'k' || kind == 'p', "invalid asset kind");
    check(range(word(Assets, a, 9), word(Assets, a, 10), result.section(Models).count), "asset model range");
    check(range(word(Assets, a, 12), word(Assets, a, 13), result.section(Source).count), "asset source range");
    check(kind != 'p' || word(Assets, a, 11) < result.section(Textures).count, "asset texture index");
  }
  for (uint32_t m = 0; m < result.section(Models).count; ++m) {
    const uint32_t asset = word(Models, m, 6);
    check(asset < result.section(Assets).count, "model asset index");
    const uint32_t first = word(Assets, asset, 9), count = word(Assets, asset, 10);
    check(m >= first && m - first < count, "model ownership mismatch");
    for (uint32_t w : {1u, 2u}) {
      const uint32_t link = word(Models, m, w);
      check(link == ~0u || (link >= first && link - first < count && link != m), "invalid model relation");
    }
    check(range(word(Models, m, 3), word(Models, m, 4), result.section(Vertices).count) &&
          word(Models, m, 4) % 4 == 0, "model corner range");
  }
  for (uint32_t t = 0; t < result.section(Textures).count; ++t) {
    const uint32_t width = word(Textures, t, 0), height = word(Textures, t, 1);
    check(width > 0 && width <= 4096 && height > 0 && height <= 4096 &&
          width * height == word(Textures, t, 3), "texture dimensions");
    check(range(word(Textures, t, 2), word(Textures, t, 3), result.section(Indices).count), "texture pixels");
    check(range(word(Textures, t, 4), word(Textures, t, 5), result.section(Palettes).count) &&
          word(Textures, t, 5) > 0 && word(Textures, t, 5) <= 256, "texture palette");
    check(word(Textures, t, 7) < result.section(Assets).count, "texture asset index");
  }
  return result;
}
uint32_t AssetCache::word(Section s, uint32_t row, uint32_t column) const {
  const auto &v = sections_[s];
  check(row < v.count && uint64_t(column) * 4 + 4 <= v.stride, "record access outside section");
  return u32(bytes_.data() + v.offset + uint64_t(row) * v.stride + column * 4);
}
std::array<uint8_t, 32> AssetCache::sourceHash(uint32_t asset) const {
  check(asset < section(Assets).count, "asset hash index");
  std::array<uint8_t, 32> result;
  std::copy_n(bytes_.data() + section(Assets).offset + size_t(asset) * 64, 32, result.begin());
  return result;
}
} // namespace ps1::games::mgs
