#pragma once
#include <cstdint>
namespace ps1::gpu::replay {
// Accelerated replay only. The guest-authoritative raster remains 1x and
// EnhancedSurface retains its independent software diagnostic limit.
inline constexpr uint32_t MAX_INTERNAL_SCALE = 12;
}
