#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps1::gpu {

struct Color32 {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;

  bool operator==(const Color32 &) const = default;
};

static_assert(sizeof(Color32) == 4);

// Host-side companion to native PS1 VRAM.
//
// Native VRAM remains authoritative for guest reads, texture feedback, mask
// semantics and VRAM copies. This surface is a higher-resolution presentation
// target with independent RGBA8 color, mask and floating-point depth planes.
// Legacy/2D commands mirror their native pixel into an SxS block; precise 3D
// commands may rasterize directly into individual high-resolution samples.
class EnhancedSurface {
public:
  static constexpr uint32_t MAX_SCALE = 8;
  using DepthEpoch = uint32_t;

  // Stable until the next configure(). Rasterizers use one row view per
  // scanline so the inner loop can apply the same color/depth/mask rules
  // without three bounds checks and an out-of-line call per sample.
  struct MutableRow {
    Color32 *color = nullptr;
    float *depth = nullptr;
    uint8_t *mask = nullptr;
    DepthEpoch *depthEpoch = nullptr;
    DepthEpoch activeDepthEpoch = 0;

    explicit operator bool() const {
      return color != nullptr && depth != nullptr && mask != nullptr &&
             depthEpoch != nullptr && activeDepthEpoch != 0;
    }
  };

  void configure(uint32_t nativeWidth, uint32_t nativeHeight, uint32_t scale);

  bool enabled() const { return !color_.empty(); }
  uint32_t scale() const { return scale_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t nativeWidth() const { return nativeWidth_; }
  uint32_t nativeHeight() const { return nativeHeight_; }

  void clear(Color32 color = {});
  // Start a logically empty depth plane without touching every sample. Epoch
  // zero is reserved for individually invalidated painter/native samples.
  // The only linear-time case is the practically unreachable generation wrap,
  // where all tags are reset before generation values can be reused.
  void advanceDepthEpoch();
  void clearDepth();
  void clearDepthNativeRect(uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height);

  static Color32 expandNativeColor(uint16_t raw);

  // Mirror an authoritative native pixel. This deliberately resets depth for
  // the covered samples: ordered 2D/legacy writes form a new painter layer.
  void mirrorNativePixel(uint32_t x, uint32_t y, uint16_t raw);
  void mirrorNativeBuffer(const uint16_t *pixels, std::size_t pixelCount);
  void copyNativePixel(uint32_t sourceX, uint32_t sourceY,
                       uint32_t destinationX, uint32_t destinationY,
                       bool forceMask);
  void fillNativeRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      Color32 color, bool mask = false);

  Color32 colorAt(uint32_t x, uint32_t y) const;
  bool maskAt(uint32_t x, uint32_t y) const;
  float depthAt(uint32_t x, uint32_t y) const;
  MutableRow mutableRow(uint32_t y);

  // Atomically apply mask/depth acceptance and write one host sample.
  // Smaller positive depth is nearer. LEQUAL preserves submission order for
  // coplanar polygons while making opaque 3D independent of OT ordering.
  bool writeSample(uint32_t x, uint32_t y, Color32 color, bool sourceMask,
                   bool forceMask, bool checkMask, float depth, bool useDepth,
                   bool resetDepth = false);

  // Copy a native-coordinate rectangle at internal resolution. The output is
  // tightly packed RGBA8 and suitable for offline oracle comparison.
  void copyNativeRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        std::vector<Color32> &output) const;

private:
  std::size_t index(uint32_t x, uint32_t y) const;

  uint32_t nativeWidth_ = 0;
  uint32_t nativeHeight_ = 0;
  uint32_t scale_ = 1;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<Color32> color_;
  std::vector<float> depth_;
  std::vector<uint8_t> mask_;
  std::vector<DepthEpoch> depthEpoch_;
  DepthEpoch activeDepthEpoch_ = 1;
};

} // namespace ps1::gpu
