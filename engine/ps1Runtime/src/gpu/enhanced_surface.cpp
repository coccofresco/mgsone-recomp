#include "runtime/gpu/enhanced_surface.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ps1::gpu {

void EnhancedSurface::configure(uint32_t nativeWidth, uint32_t nativeHeight,
                                uint32_t scale) {
  nativeWidth_ = nativeWidth;
  nativeHeight_ = nativeHeight;
  scale_ = std::clamp(scale, 1u, MAX_SCALE);
  width_ = nativeWidth_ * scale_;
  height_ = nativeHeight_ * scale_;

  const std::size_t count =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  color_.assign(count, Color32{});
  depth_.assign(count, std::numeric_limits<float>::infinity());
  mask_.assign(count, 0);
  depthEpoch_.assign(count, 0);
  activeDepthEpoch_ = 1;
}

void EnhancedSurface::clear(Color32 color) {
  std::fill(color_.begin(), color_.end(), color);
  std::fill(mask_.begin(), mask_.end(), 0);
  clearDepth();
}

void EnhancedSurface::advanceDepthEpoch() {
  if (!enabled())
    return;

  if (activeDepthEpoch_ == std::numeric_limits<DepthEpoch>::max()) {
    // Epoch zero is never active. Erase every old tag before recycling epoch
    // one so no sample from the first generation can become valid again.
    std::fill(depthEpoch_.begin(), depthEpoch_.end(), 0);
    activeDepthEpoch_ = 1;
    return;
  }
  ++activeDepthEpoch_;
}

void EnhancedSurface::clearDepth() {
  advanceDepthEpoch();
}

void EnhancedSurface::clearDepthNativeRect(uint32_t x, uint32_t y,
                                           uint32_t width, uint32_t height) {
  if (!enabled() || x >= nativeWidth_ || y >= nativeHeight_ || width == 0 ||
      height == 0)
    return;
  width = std::min(width, nativeWidth_ - x);
  height = std::min(height, nativeHeight_ - y);
  const uint32_t scaledX = x * scale_;
  const std::size_t scaledWidth = static_cast<std::size_t>(width) * scale_;
  for (uint32_t row = y * scale_; row < (y + height) * scale_; ++row) {
    const std::size_t first = index(scaledX, row);
    std::fill_n(depthEpoch_.begin() + static_cast<std::ptrdiff_t>(first),
                scaledWidth, 0);
  }
}

Color32 EnhancedSurface::expandNativeColor(uint16_t raw) {
  auto expand5 = [](uint16_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
  };
  return Color32{expand5(raw & 0x1Fu), expand5((raw >> 5) & 0x1Fu),
                 expand5((raw >> 10) & 0x1Fu), 255};
}

void EnhancedSurface::mirrorNativePixel(uint32_t x, uint32_t y, uint16_t raw) {
  if (x >= nativeWidth_ || y >= nativeHeight_ || !enabled())
    return;

  const Color32 expanded = expandNativeColor(raw);
  const uint8_t mask = (raw & 0x8000u) != 0 ? 1u : 0u;
  const uint32_t startX = x * scale_;
  const uint32_t startY = y * scale_;
  for (uint32_t sy = 0; sy < scale_; ++sy) {
    const std::size_t row = index(startX, startY + sy);
    for (uint32_t sx = 0; sx < scale_; ++sx) {
      const std::size_t destination = row + sx;
      color_[destination] = expanded;
      mask_[destination] = mask;
      depthEpoch_[destination] = 0;
    }
  }
}

void EnhancedSurface::mirrorNativeBuffer(const uint16_t *pixels,
                                         std::size_t pixelCount) {
  if (pixels == nullptr || !enabled())
    return;
  const std::size_t count = std::min(
      pixelCount, static_cast<std::size_t>(nativeWidth_) * nativeHeight_);
  for (std::size_t i = 0; i < count; ++i) {
    mirrorNativePixel(static_cast<uint32_t>(i % nativeWidth_),
                      static_cast<uint32_t>(i / nativeWidth_), pixels[i]);
  }
}

void EnhancedSurface::copyNativePixel(uint32_t sourceX, uint32_t sourceY,
                                      uint32_t destinationX,
                                      uint32_t destinationY, bool forceMask) {
  if (!enabled() || sourceX >= nativeWidth_ || sourceY >= nativeHeight_ ||
      destinationX >= nativeWidth_ || destinationY >= nativeHeight_)
    return;

  std::array<Color32, MAX_SCALE * MAX_SCALE> sourceColor{};
  std::array<uint8_t, MAX_SCALE * MAX_SCALE> sourceMask{};
  const uint32_t sourceStartX = sourceX * scale_;
  const uint32_t sourceStartY = sourceY * scale_;
  for (uint32_t sy = 0; sy < scale_; ++sy) {
    for (uint32_t sx = 0; sx < scale_; ++sx) {
      const std::size_t temporary = static_cast<std::size_t>(sy) * scale_ + sx;
      const std::size_t source = index(sourceStartX + sx, sourceStartY + sy);
      sourceColor[temporary] = color_[source];
      sourceMask[temporary] = mask_[source];
    }
  }

  const uint32_t destinationStartX = destinationX * scale_;
  const uint32_t destinationStartY = destinationY * scale_;
  for (uint32_t sy = 0; sy < scale_; ++sy) {
    for (uint32_t sx = 0; sx < scale_; ++sx) {
      const std::size_t temporary = static_cast<std::size_t>(sy) * scale_ + sx;
      const std::size_t destination =
          index(destinationStartX + sx, destinationStartY + sy);
      color_[destination] = sourceColor[temporary];
      mask_[destination] = sourceMask[temporary] || forceMask ? 1u : 0u;
      depthEpoch_[destination] = 0;
    }
  }
}

void EnhancedSurface::fillNativeRect(uint32_t x, uint32_t y, uint32_t width,
                                     uint32_t height, Color32 color,
                                     bool mask) {
  if (!enabled() || width == 0 || height == 0)
    return;

  x %= nativeWidth_;
  y %= nativeHeight_;
  width = std::min(width, nativeWidth_);
  height = std::min(height, nativeHeight_);
  color.a = 255;

  auto fillSegment = [&](uint32_t row, uint32_t segmentX,
                         uint32_t segmentWidth) {
    const std::size_t first = index(segmentX * scale_, row);
    const std::size_t count = static_cast<std::size_t>(segmentWidth) * scale_;
    std::fill_n(color_.begin() + static_cast<std::ptrdiff_t>(first), count,
                color);
    std::fill_n(mask_.begin() + static_cast<std::ptrdiff_t>(first), count,
                mask ? 1u : 0u);
    std::fill_n(depthEpoch_.begin() + static_cast<std::ptrdiff_t>(first), count,
                0);
  };

  const uint32_t firstWidth = std::min(width, nativeWidth_ - x);
  const uint32_t secondWidth = width - firstWidth;
  for (uint32_t dy = 0; dy < height; ++dy) {
    const uint32_t nativeRow = (y + dy) % nativeHeight_;
    for (uint32_t sy = 0; sy < scale_; ++sy) {
      const uint32_t scaledRow = nativeRow * scale_ + sy;
      fillSegment(scaledRow, x, firstWidth);
      if (secondWidth != 0)
        fillSegment(scaledRow, 0, secondWidth);
    }
  }
}

Color32 EnhancedSurface::colorAt(uint32_t x, uint32_t y) const {
  if (x >= width_ || y >= height_ || !enabled())
    return {};
  return color_[index(x, y)];
}

bool EnhancedSurface::maskAt(uint32_t x, uint32_t y) const {
  return x < width_ && y < height_ && enabled() && mask_[index(x, y)] != 0;
}

float EnhancedSurface::depthAt(uint32_t x, uint32_t y) const {
  if (x >= width_ || y >= height_ || !enabled())
    return std::numeric_limits<float>::infinity();
  const std::size_t sample = index(x, y);
  return depthEpoch_[sample] == activeDepthEpoch_
             ? depth_[sample]
             : std::numeric_limits<float>::infinity();
}

EnhancedSurface::MutableRow EnhancedSurface::mutableRow(uint32_t y) {
  if (y >= height_ || !enabled())
    return {};
  const std::size_t first = index(0, y);
  return MutableRow{color_.data() + first, depth_.data() + first,
                    mask_.data() + first, depthEpoch_.data() + first,
                    activeDepthEpoch_};
}

bool EnhancedSurface::writeSample(uint32_t x, uint32_t y, Color32 color,
                                  bool sourceMask, bool forceMask,
                                  bool checkMask, float depth, bool useDepth,
                                  bool resetDepth) {
  if (x >= width_ || y >= height_ || !enabled())
    return false;

  const std::size_t destination = index(x, y);
  if (checkMask && mask_[destination] != 0)
    return false;
  if (useDepth) {
    if (!(depth > 0.0f) || !std::isfinite(depth) ||
        (depthEpoch_[destination] == activeDepthEpoch_ &&
         depth > depth_[destination]))
      return false;
    depth_[destination] = depth;
    depthEpoch_[destination] = activeDepthEpoch_;
  } else if (resetDepth) {
    depthEpoch_[destination] = 0;
  }

  color.a = 255;
  color_[destination] = color;
  mask_[destination] = sourceMask || forceMask ? 1u : 0u;
  return true;
}

void EnhancedSurface::copyNativeRegion(uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height,
                                       std::vector<Color32> &output) const {
  if (!enabled() || x >= nativeWidth_ || y >= nativeHeight_) {
    output.clear();
    return;
  }

  width = std::min(width, nativeWidth_ - x);
  height = std::min(height, nativeHeight_ - y);
  const uint32_t outputWidth = width * scale_;
  const uint32_t outputHeight = height * scale_;
  output.resize(static_cast<std::size_t>(outputWidth) * outputHeight);

  const uint32_t sourceX = x * scale_;
  const uint32_t sourceY = y * scale_;
  for (uint32_t row = 0; row < outputHeight; ++row) {
    const std::size_t source = index(sourceX, sourceY + row);
    const std::size_t destination = static_cast<std::size_t>(row) * outputWidth;
    std::copy_n(color_.begin() + static_cast<std::ptrdiff_t>(source),
                outputWidth,
                output.begin() + static_cast<std::ptrdiff_t>(destination));
  }
}

std::size_t EnhancedSurface::index(uint32_t x, uint32_t y) const {
  return static_cast<std::size_t>(y) * width_ + x;
}

} // namespace ps1::gpu
