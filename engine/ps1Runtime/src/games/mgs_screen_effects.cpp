#include "runtime/games/mgs_screen_effects.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>

namespace ps1::games::mgs {
namespace r = gpu::replay;
namespace {
struct WaterActor {
  uint32_t actor, primitives, phase;
};
// These functions run on the GPU producer thread, before publishing packets.
std::vector<WaterActor> waterActors;
} // namespace
void captureWaterEffects(std::span<const uint8_t> ram) {
  waterActors.clear();
  if (!active())
    return;
  const auto valid = [&](uint32_t p, uint32_t n) {
    p &= 0x1fffffffu;
    return p < ram.size() && n <= ram.size() - p;
  };
  const auto word = [&](uint32_t p) {
    p &= 0x1fffffffu;
    return uint32_t(ram[p]) | uint32_t(ram[p + 1]) << 8 |
           uint32_t(ram[p + 2]) << 16 | uint32_t(ram[p + 3]) << 24;
  };
  // USA GV_ACTOR_PREV2. GV_ACT is 32 bytes, AList is 68 bytes.
  uint32_t previous = 0x800af45c,
           actor = valid(previous, 8) ? word(previous + 4) : 0;
  for (unsigned i = 0; i < 1024 && actor; ++i) {
    if (!valid(actor, 32) || word(actor) != previous) {
      waterActors.clear();
      return;
    }
    const auto name = word(actor + 20);
    if (valid(name, 10) &&
        !std::memcmp(ram.data() + (name & 0x1fffffff), "wt_view.c", 10) &&
        valid(actor, 76) && word(actor + 64) == 1) {
      const auto prims = word(actor + 56);
      if (valid(prims, 9912))
        waterActors.push_back(
            {actor & 0x1fffffffu, prims & 0x1fffffffu, word(actor + 72) - 1u});
    }
    previous = actor;
    actor = word(actor + 4);
  }
}
void bindWaterEffect(r::DrawPacket &packet, uint32_t commandAddress) {
  if (!active() || packet.opcode != 0x64 || packet.vertexCount != 4 ||
      packet.route != r::PacketRoute::Screen2D || packet.state.blend.enabled ||
      packet.state.mask.checkMaskBit || commandAddress == UINT32_MAX)
    return;
  const uint32_t address = commandAddress & 0x1fffffffu;
  for (const auto &actor : waterActors) {
    // Four SPRT arrays follow four DR_TPAGE and two DR_PRIO packets.
    const uint32_t begin = actor.primitives + 56 + 4;
    if (address >= begin && address < begin + 4 * 122 * 20 &&
        (address - begin) % 20 == 0) {
      packet.waterEffect = {actor.actor, actor.phase};
      metrics::count("mgs.water.bound_strips");
      return;
    }
  }
}
std::optional<ScreenRead> screenRead(const r::VulkanReplayBakedDraw &draw,
                                     const r::VulkanReplayBakedBatch &batch) {
  if (draw.segmentCount == 0 || batch.internalScale == 0 ||
      draw.firstVertex > batch.vertices.size() ||
      draw.vertexCount > batch.vertices.size() - draw.firstVertex ||
      draw.vertexCount == 0)
    return {};
  const auto &u = draw.segments[0].uniforms;
  if (!(u.textureState[2] &
        r::replayDrawFlagBit(r::ReplayDrawFlag::Textured)) ||
      ((u.textureState[0] >> 7) & 3) != 2 || u.mgsTextureOrigin[2] != 0 ||
      u.mgsPointBytes != std::array<uint32_t, 4>{} || u.textureWindow[0] != 0 ||
      u.textureWindow[1] != 0)
    return {};
  // The live draw environment proves the expected buffer layout. Menus or
  // other modes with a different layout keep their existing texture path.
  const auto &scissor = draw.segments[0].scissor;
  const auto scale = batch.internalScale;
  if (scissor.y != 0 || scissor.height != 224 * scale ||
      scissor.width != 320 * scale ||
      (scissor.x != 0 && scissor.x != 320 * scale))
    return {};
  if (u.textureState[2] &
      r::replayDrawFlagBit(r::ReplayDrawFlag::WaterRefraction)) {
    const uint32_t x = scissor.x / scale;
    return ScreenRead{{x, 0, 320, 224}, {x, 6, 320, 212}};
  }
  float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
  float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
  for (uint32_t i = 0; i < draw.vertexCount; ++i) {
    const auto &v = batch.vertices[draw.firstVertex + i];
    if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]) ||
        !std::isfinite(v.positionNative[0]) ||
        !std::isfinite(v.positionNative[1]))
      return {};
    minU = std::min(minU, v.uv[0]);
    maxU = std::max(maxU, v.uv[0]);
    minV = std::min(minV, v.uv[1]);
    maxV = std::max(maxV, v.uv[1]);
    minX = std::min(minX, v.positionNative[0]);
    maxX = std::max(maxX, v.positionNative[0]);
    minY = std::min(minY, v.positionNative[1]);
    maxY = std::max(maxY, v.positionNative[1]);
  }
  if (minU < 0 || minV < 0 || maxU > 256 || maxV > 256 || minX < 0 ||
      minY < 0 || maxX > 640 || maxY > 224 || maxX <= minX || maxY <= minY)
    return {};
  const uint32_t pageX = (u.textureState[0] & 15) * 64;
  const uint32_t pageY = ((u.textureState[0] >> 4) & 1) * 256;
  // Sprite endpoints describe half-open texel spans. A one-texel polygon
  // still reads one whole texel. UV wrapping is deliberately not inferred.
  const uint32_t x = pageX + uint32_t(std::floor(minU));
  const uint32_t y = pageY + uint32_t(std::floor(minV));
  const uint32_t right =
      pageX + uint32_t(std::max(std::floor(minU) + 1.f, std::ceil(maxU)));
  const uint32_t bottom =
      pageY + uint32_t(std::max(std::floor(minV) + 1.f, std::ceil(maxV)));
  if (right > SCREEN_TEXTURE_WIDTH || bottom > SCREEN_TEXTURE_HEIGHT)
    return {};
  return ScreenRead{{x, y, right - x, bottom - y},
                    {uint32_t(std::floor(minX)), uint32_t(std::floor(minY)),
                     uint32_t(std::ceil(maxX) - std::floor(minX)),
                     uint32_t(std::ceil(maxY) - std::floor(minY))}};
}
void prepareScreenReads(r::VulkanReplayBakedBatch &batch) {
  std::set<std::tuple<float, float, float, uint32_t>> waterPasses;
  const auto replace = [&](auto &command) {
    auto *draw = std::get_if<r::VulkanReplayBakedDraw>(&command.payload);
    if (!draw || !screenRead(*draw, batch))
      return false;
    auto &u = draw->segments[0].uniforms;
    if (u.textureState[2] &
        r::replayDrawFlagBit(r::ReplayDrawFlag::WaterRefraction)) {
      const uint32_t x = draw->segments[0].scissor.x / batch.internalScale;
      if (!waterPasses
               .emplace(u.mgsProjection[0], u.mgsProjection[1],
                        u.mgsProjection[2], x)
               .second) {
        metrics::count("mgs.water.replaced_strips");
        return true;
      }
      // One modern continuous pass replaces this actor's complete strip set.
      const auto color = batch.vertices[draw->firstVertex].color255;
      draw->firstVertex = uint32_t(batch.vertices.size());
      draw->vertexCount = 6;
      draw->segmentCount = 1;
      draw->segments[0].firstVertex = draw->firstVertex;
      draw->segments[0].vertexCount = 6;
      for (const auto p : std::array<std::array<float, 2>, 6>{
               {{0, 6}, {320, 6}, {0, 218}, {320, 6}, {320, 218}, {0, 218}}}) {
        r::vulkan_abi::Vertex v;
        v.positionNative = {float(x) + p[0], p[1]};
        v.uv = p;
        v.color255 = color;
        batch.vertices.push_back(v);
      }
      u.mgsProjection[1] = float(x);
      u.mgsProjection[2] = 0;
      u.uvDomain = {0, 0, 319, 223};
      draw->segments[0].destinationSnapshot = {};
      u.destinationState = {};
      metrics::count("mgs.water.modern_pass");
    }
    draw->uniforms.textureState[2] |=
        r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenTexture);
    for (uint32_t s = 0; s < draw->segmentCount; ++s)
      draw->segments[s].uniforms.textureState[2] |=
          r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenTexture);
    draw->uniforms = draw->segments[0].uniforms;
    return false;
  };
  std::vector<r::VulkanReplayBakedCommand> commands;
  commands.reserve(batch.commands.size());
  for (auto &command : batch.commands)
    if (!replace(command))
      commands.push_back(std::move(command));
  batch.commands = std::move(commands);
}
void ScreenReadTracker::invalidate() {
  valid_ = false;
  writes_.clear();
}
bool ScreenReadTracker::needsSnapshot(const ScreenRead &read) {
  const auto &a = read.source;
  const bool refresh =
      !valid_ ||
      std::any_of(writes_.begin(), writes_.end(), [&](const auto &b) {
        return a.x < b.x + b.width && b.x < a.x + a.width &&
               a.y < b.y + b.height && b.y < a.y + a.height;
      });
  if (refresh) {
    writes_.clear();
    valid_ = true;
  }
  writes_.push_back(read.destination);
  return refresh;
}
} // namespace ps1::games::mgs
