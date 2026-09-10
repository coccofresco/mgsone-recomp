#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ps1::games::mgs {
// Immutable, little-endian std430 storage. Indexed source KMD/ZMD bytes remain
// alongside expanded corners, so parent seam indices never become weights.
struct AssetSection {
  uint32_t offset = 0, count = 0, stride = 0;
  uint64_t size() const { return uint64_t(count) * stride; }
};
class AssetCache {
public:
  enum Section : size_t { Assets, Models, Vertices, Textures, Indices, Palettes, Source, Count };
  static AssetCache load(const std::filesystem::path &path);
  static AssetCache decode(std::vector<uint8_t> bytes);
  const AssetSection &section(Section s) const { return sections_[s]; }
  std::span<const uint8_t> bytes() const { return bytes_; }
  uint32_t word(Section s, uint32_t row, uint32_t word) const;
  std::array<uint8_t, 32> sourceHash(uint32_t asset) const;
private:
  std::array<AssetSection, Count> sections_{};
  std::vector<uint8_t> bytes_;
};
} // namespace ps1::games::mgs
