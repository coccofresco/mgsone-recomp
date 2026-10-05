#pragma once
#include "runtime/gpu/modern_replay.h"
#include <span>

namespace ps1::games::mgs {
// Read-only snapshot of validated retail actor ownership and original data.
void captureSurfaceEffects(std::span<const uint8_t> ram);
void bindSurfaceEffect(gpu::replay::DrawPacket &packet, uint32_t commandAddress);
void bindInterfaceGeometry(gpu::replay::DrawPacket &packet, uint32_t commandAddress);
bool hasSourceSurfaceGeometry(uint32_t commandAddress);
gpu::replay::MgsSurfaceEffect surfaceEffectForObject(uint32_t objects);
// Producer-only event: take once when sealing the exact shutter frame.
uint64_t consumePhotoCaptureEvent();
} // namespace ps1::games::mgs
