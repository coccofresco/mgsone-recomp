#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ps1::games::mgs {
// Immutable, little-endian std430 storage. Version 3 retains original indexed
// KMD/ZMD streams without the expanded corners present in legacy containers.
struct AssetSection {
  uint32_t offset = 0, count = 0, stride = 0;
  uint64_t size() const { return uint64_t(count) * stride; }
};
// Original KMD/ZMD layout, pointing into the immutable source file bytes in
// GPU storage. Zero offsets denote absent optional streams. No projected
// vertices or native packet allocation; every original variant is retained.
struct SourceModelView {
  uint32_t asset = 0, descriptorByte = 0;
  uint32_t flags = 0, faces = 0, vertices = 0, normals = 0;
  int32_t parent = -1, extend = -1;
  uint32_t positionsByte = 0, vertexIndicesByte = 0, normalsByte = 0;
  uint32_t normalIndicesByte = 0, uvByte = 0, textureIdsByte = 0;
};
class AssetCache {
public:
  enum Section : size_t {
    Assets,
    Models,
    Vertices,
    Textures,
    Indices,
    Palettes,
    Source,
    Names,
    Templates,
    Count
  };
  static AssetCache load(const std::filesystem::path &path);
  static AssetCache decode(std::vector<uint8_t> bytes);
  const AssetSection &section(Section s) const { return sections_[s]; }
  std::span<const uint8_t> bytes() const { return bytes_; }
  uint32_t word(Section s, uint32_t row, uint32_t word) const;
  std::array<uint8_t, 32> sourceHash(uint32_t asset) const;
  std::optional<SourceModelView> sourceModel(uint32_t model) const;

private:
  std::array<AssetSection, Count> sections_{};
  std::vector<uint8_t> bytes_;
};
} // namespace ps1::games::mgs
