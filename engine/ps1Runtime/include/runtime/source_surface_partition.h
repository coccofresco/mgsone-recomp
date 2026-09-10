#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <span>

namespace ps1::surface_partition {

using Point = std::array<int32_t, 3>;
using Point2 = std::array<int64_t, 2>;
struct Surface {
  std::array<int64_t, 4> plane{};
  std::array<Point2, 4> polygon{};
  uint32_t count = 0;
  int64_t twiceArea = 0;
};
inline int64_t cross(Point2 a, Point2 b, Point2 p) noexcept {
  return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
}

// Bounded exact integer geometry. Reject unsupported input rather than
// rounding a nearby plane into equality. No game, texture or camera policy.
inline std::optional<Surface> makeSurface(std::span<const Point> vertices) {
  if (vertices.size() < 3 || vertices.size() > 4)
    return std::nullopt;
  for (const auto &p : vertices)
    for (int32_t x : p)
      if (x < -32767 || x > 32767)
        return std::nullopt;
  for (uint32_t i = 0; i < vertices.size(); ++i)
    for (uint32_t j = 0; j < i; ++j)
      if (vertices[i] == vertices[j])
        return std::nullopt;
  std::array<int64_t, 3> u{}, v{}, n{};
  for (uint32_t i = 0; i < 3; ++i) {
    u[i] = int64_t{vertices[1][i]} - vertices[0][i];
    v[i] = int64_t{vertices[2][i]} - vertices[0][i];
  }
  n = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
       u[0] * v[1] - u[1] * v[0]};
  int64_t divisor = std::gcd(std::gcd(n[0], n[1]), n[2]);
  if (divisor == 0)
    return std::nullopt;
  for (int64_t x : n)
    if (x != 0) {
      if (x < 0)
        divisor = -divisor;
      break;
    }
  Surface result;
  uint32_t drop = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    n[i] /= divisor;
    result.plane[i] = n[i];
    result.plane[3] += n[i] * vertices[0][i];
    if (std::abs(n[i]) > std::abs(n[drop]))
      drop = i;
  }
  result.count = static_cast<uint32_t>(vertices.size());
  for (uint32_t j = 0; j < result.count; ++j) {
    int64_t dot = 0;
    uint32_t axis = 0;
    for (uint32_t i = 0; i < 3; ++i) {
      dot += n[i] * vertices[j][i];
      if (i != drop)
        result.polygon[j][axis++] = vertices[j][i];
    }
    if (dot != result.plane[3])
      return std::nullopt;
  }
  for (uint32_t i = 0; i < result.count; ++i) {
    const auto a = result.polygon[i],
               b = result.polygon[(i + 1) % result.count];
    result.twiceArea += a[0] * b[1] - a[1] * b[0];
  }
  if (result.twiceArea == 0)
    return std::nullopt;
  if (result.twiceArea < 0) {
    result.twiceArea = -result.twiceArea;
    std::reverse(result.polygon.begin(), result.polygon.begin() + result.count);
  }
  for (uint32_t i = 0; i < result.count; ++i)
    if (cross(result.polygon[i], result.polygon[(i + 1) % result.count],
              result.polygon[(i + 2) % result.count]) < 0)
      return std::nullopt;
  return result;
}

inline bool contains(const Surface &parent, const Surface &child) noexcept {
  if (parent.count < 3 || parent.count > 4 || child.count < 3 ||
      child.count > 4 || parent.plane != child.plane)
    return false;
  for (uint32_t i = 0; i < parent.count; ++i)
    for (uint32_t j = 0; j < child.count; ++j)
      if (cross(parent.polygon[i], parent.polygon[(i + 1) % parent.count],
                child.polygon[j]) < 0)
        return false;
  return true;
}

inline bool interiorsOverlap(const Surface &a, const Surface &b) noexcept {
  const auto separated = [](const Surface &p, const Surface &q) {
    for (uint32_t i = 0; i < p.count; ++i) {
      bool outside = true;
      for (uint32_t j = 0; j < q.count; ++j)
        outside &= cross(p.polygon[i], p.polygon[(i + 1) % p.count],
                         q.polygon[j]) <= 0;
      if (outside)
        return true;
    }
    return false;
  };
  return !separated(a, b) && !separated(b, a);
}

inline bool certify(const Surface &parent, std::span<const Surface> children) {
  if (children.size() < 2 || children.size() > 6144)
    return false;
  int64_t area = 0;
  for (uint32_t i = 0; i < children.size(); ++i) {
    const auto &child = children[i];
    if (child.twiceArea <= 0 || child.twiceArea >= parent.twiceArea ||
        !contains(parent, child))
      return false;
    area += child.twiceArea;
    if (area > parent.twiceArea)
      return false;
    for (uint32_t j = 0; j < i; ++j)
      if (interiorsOverlap(child, children[j]))
        return false;
  }
  return area == parent.twiceArea;
}
} // namespace ps1::surface_partition
