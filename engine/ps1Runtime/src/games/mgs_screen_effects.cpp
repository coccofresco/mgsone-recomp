#include "runtime/games/mgs_screen_effects.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
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
struct EffectRange {
  uint32_t begin, stride, count, firstPart;
  r::MgsScreenEffect effect;
};
std::vector<EffectRange> effectRanges;
} // namespace

void captureScreenEffects(std::span<const uint8_t> ram) {
  captureWaterEffects(ram);
  effectRanges.clear();
  if (!active())
    return;
  const auto valid = [&](uint32_t p, uint32_t n) {
    p &= 0x1fffffffu;
    return p >= 0x10000 && p < ram.size() && n <= ram.size() - p;
  };
  const auto word = [&](uint32_t p) {
    p &= 0x1fffffffu;
    return uint32_t(ram[p]) | uint32_t(ram[p + 1]) << 8 |
           uint32_t(ram[p + 2]) << 16 | uint32_t(ram[p + 3]) << 24;
  };
  using Kind = r::MgsScreenEffectKind;
  // USA ActorList, verified from GV_InitActorSystem and live intro RAM.
  // Each level has start/end GV_ACT sentinels plus two i16 level masks.
  for (uint32_t level = 0; level < 9; ++level) {
    const uint32_t head = 0x800af390 + level * 68, tail = head + 32;
    if (!valid(head, 68))
      continue;
    std::vector<EffectRange> ranges;
    uint32_t previous = head, actor = word(head + 4);
    unsigned guard = 0;
    for (; actor != tail && actor && guard < 1024; ++guard) {
      if (!valid(actor, 32) || word(actor) != previous)
        break;
      const auto name = word(actor + 20);
      const auto named = [&](const char *s) {
        const auto length = std::strlen(s) + 1;
        return word(actor + 8) && valid(name, length) &&
               !std::memcmp(ram.data() + (name & 0x1fffffff), s, length);
      };
      const auto add = [&](uint32_t p, uint32_t stride, uint32_t count,
                           uint32_t firstPart, r::MgsScreenEffect effect) {
        effect.actor = actor & 0x1fffffff;
        ranges.push_back({p & 0x1fffffff, stride, count, firstPart, effect});
      };
      if (level == 2 && named("scn_mask.c") && valid(actor, 44)) {
        metrics::count("mgs.screen_effect.actor.visor");
        const auto p = word(actor + 32);
        if (valid(p, 3648)) {
          for (uint32_t buffer = 0; buffer < 2; ++buffer) {
            const auto tile = p + 16 + buffer * 16;
            const auto color = word(tile + 4);
            bool layout = (color == 0x62287838 || color == 0x62000050) &&
                          word(tile + 8) == 0xff90ff60 &&
                          word(tile + 12) == 0x00e00140;
            for (uint32_t i = 0; i < 112 && layout; ++i) {
              const auto line = p + 64 + (buffer * 112 + i) * 16;
              layout = word(line + 4) == 0x62404040 &&
                       word(line + 8) ==
                           (0xff60u |
                            (uint32_t(uint16_t(-112 + int(i) * 2)) << 16)) &&
                       word(line + 12) == 0x00010140;
            }
            if (!layout)
              continue;
            metrics::count("mgs.screen_effect.layout.visor");
            r::MgsScreenEffect e;
            e.kind =
                color == 0x62287838 ? Kind::NightVision : Kind::ThermalVision;
            e.parts = 113;
            add(tile + 4, 16, 1, 0, e);
            add(p + 68 + buffer * 112 * 16, 16, 112, 1, e);
          }
        }
      } else if (level == 3 && named("fadeio.c") && valid(actor, 60)) {
        const auto p = word(actor + 36);
        if (valid(p, 48)) {
          r::MgsScreenEffect e;
          e.kind = word(actor + 52) & 2 ? Kind::FadeWhite : Kind::FadeBlack;
          add(p + 20, 16, 2, 0, e);
        }
      } else if (level == 7 && named("blurpure.c") && valid(actor, 44)) {
        const auto p = word(actor + 32);
        // USA DR_STP is 12 bytes (not DR_TPAGE's eight bytes).
        if (word(actor + 36) && valid(p, 224)) {
          r::MgsScreenEffect e;
          e.kind = Kind::TemporalBlur;
          e.parts = 2;
          for (uint32_t b = 0; b < 2; ++b) {
            add(p + 116 + b * 20, 20, 1, 1, e);
            add(p + 156 + b * 20, 20, 1, 0, e);
          }
        }
      } else if (level == 3 && named("gas_efct.c") && valid(actor, 104)) {
        metrics::count("mgs.screen_effect.actor.gas_haze");
        const auto p = word(actor + 40);
        if (word(actor + 64) && valid(p, 1168) && word(actor + 92) < 230 &&
            word(actor + 100) > 0 && word(actor + 100) <= 8192) {
          r::MgsScreenEffect e;
          e.kind = Kind::GasHaze;
          e.parts = 16;
          e.parameters[0] = {float(word(actor + 92)),
                             float(word(actor + 100)) / 4096.f, 0, 0};
          for (uint32_t b = 0; b < 2; ++b)
            add(p + 20 + b * 16 * 36, 36, 16, 0, e);
        }
      }
      previous = actor;
      actor = word(actor + 4);
    }
    // Never trust a partial/cyclic list, including recycled actor memory.
    if (actor == tail && word(tail) == previous && word(tail + 4) == 0)
      effectRanges.insert(effectRanges.end(), ranges.begin(), ranges.end());
  }
}

void bindScreenEffect(r::DrawPacket &packet, uint32_t commandAddress) {
  if (!active() || commandAddress == UINT32_MAX || packet.vertexCount != 4 ||
      packet.state.mask.checkMaskBit || packet.originalMesh ||
      packet.originalTexture || !packet.state.blend.enabled)
    return;
  const auto address = commandAddress & 0x1fffffff;
  using Kind = r::MgsScreenEffectKind;
  for (const auto &range : effectRanges) {
    if (address < range.begin ||
        address >= range.begin + range.count * range.stride ||
        (address - range.begin) % range.stride)
      continue;
    auto e = range.effect;
    e.part = range.firstPart + (address - range.begin) / range.stride;
    const auto blend = packet.state.blend.mode;
    if (e.kind == Kind::TemporalBlur) {
      if (packet.opcode != 0x66 || !packet.textured ||
          packet.state.texturePage.depth != r::TextureDepth::Direct15 ||
          blend != r::BlendMode::HalfBackgroundPlusHalfForeground ||
          packet.vertices[1].x - packet.vertices[0].x != 160 ||
          packet.vertices[2].y - packet.vertices[0].y != 160)
        return;
      e.parameters[0][0] = float(packet.vertices[0].color.r) / 256.f;
    } else if (e.kind == Kind::GasHaze) {
      if (packet.opcode != 0x3a || packet.textured)
        return;
      const auto inner = packet.vertices[0].color,
                 outer = packet.vertices[2].color;
      e.parameters[1] = {float(inner.r), float(inner.g), float(inner.b), 0};
      e.parameters[2] = {float(outer.r), float(outer.g), float(outer.b), 0};
      e.parameters[0][2] = float(blend);
    } else {
      if (packet.opcode != 0x62 || packet.textured)
        return;
      if (e.kind == Kind::FadeBlack || e.kind == Kind::FadeWhite) {
        e.part = 0; // each draw buffer's tile is a separate single pass
        if (packet.vertices[1].x - packet.vertices[0].x != 320 ||
            packet.vertices[2].y - packet.vertices[0].y != 224 ||
            blend != (e.kind == Kind::FadeBlack
                          ? r::BlendMode::BackgroundMinusForeground
                          : r::BlendMode::BackgroundPlusForeground))
          return;
        e.parameters[0][0] = float(packet.vertices[0].color.r) / 255.f;
      } else if (blend != (e.part == 0
                               ? r::BlendMode::BackgroundPlusForeground
                               : r::BlendMode::BackgroundMinusForeground)) {
        return;
      }
    }
    packet.screenEffect = e;
    metrics::count("mgs.screen_effect.bound_packets");
    return;
  }
}
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
  if (u.textureState[2] &
      r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenEffect)) {
    const uint32_t x = scissor.x / scale;
    if (u.mgsViewRows[3][1] == float(r::MgsScreenEffectKind::TemporalBlur))
      return ScreenRead{{0, 0, 640, 224}, {x, 24, 320, 160}};
    return ScreenRead{{x, 0, 320, 224}, {x, 0, 320, 224}};
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
namespace {
void prepareActorEffects(r::VulkanReplayBakedBatch &batch) {
  if (!batch.internalScale)
    return;
  using Kind = r::MgsScreenEffectKind;
  using Key = std::tuple<float, float, float, uint32_t>;
  struct Group {
    std::vector<size_t> commands;
    std::set<uint32_t> parts;
    uint32_t expected = 0;
    bool valid = true;
  };
  std::map<Key, Group> groups;
  for (size_t i = 0; i < batch.commands.size(); ++i) {
    const auto *d =
        std::get_if<r::VulkanReplayBakedDraw>(&batch.commands[i].payload);
    if (!d || !d->segmentCount || d->vertexCount != 6 ||
        d->firstVertex > batch.vertices.size() ||
        d->vertexCount > batch.vertices.size() - d->firstVertex)
      continue;
    const auto &u = d->segments[0].uniforms;
    const auto &s = d->segments[0].scissor;
    if (u.mgsPointBytes != std::array<uint32_t, 4>{} ||
        u.mgsViewRows[3][1] < float(Kind::NightVision) ||
        u.mgsViewRows[3][1] > float(Kind::GasHaze) || u.mgsProjection[1] <= 0 ||
        u.mgsViewRows[3][0] < 1 || u.mgsViewRows[3][0] > 113 || s.y != 0 ||
        s.width != 320 * batch.internalScale ||
        s.height != 224 * batch.internalScale ||
        (s.x != 0 && s.x != 320 * batch.internalScale))
      continue;
    auto &g = groups[{u.mgsProjection[1], u.mgsProjection[2],
                      u.mgsViewRows[3][1], s.x}];
    const auto count = uint32_t(u.mgsViewRows[3][0]);
    const auto part = uint32_t(u.mgsProjection[0]);
    g.valid &= (g.expected == 0 || g.expected == count) && part < count &&
               g.parts.insert(part).second;
    g.expected = count;
    g.commands.push_back(i);
  }
  std::set<size_t> removed;
  for (const auto &[key, g] : groups) {
    bool complete = g.valid && g.parts.size() == g.expected;
    // An intervening draw/copy/latch must retain its original effect ordering.
    // Native mirror uploads do not modify the modern color attachment.
    size_t member = 0;
    for (size_t i = g.commands.front(); complete && i <= g.commands.back();
         ++i) {
      if (member < g.commands.size() && i == g.commands[member])
        ++member;
      else if (!std::holds_alternative<r::VulkanReplayBakedNativeUpload>(
                   batch.commands[i].payload))
        complete = false;
    }
    if (!complete) {
      metrics::count("mgs.screen_effect.incomplete_pass");
      continue;
    }
    auto &d = std::get<r::VulkanReplayBakedDraw>(
        batch.commands[g.commands.front()].payload);
    auto &s = d.segments[0];
    auto &u = s.uniforms;
    const auto kind = Kind(uint32_t(u.mgsViewRows[3][1]));
    const auto x = s.scissor.x / batch.internalScale;
    const float top = kind == Kind::TemporalBlur ? 24 : 0;
    const float bottom = kind == Kind::TemporalBlur ? 184 : 224;
    d.firstVertex = uint32_t(batch.vertices.size());
    d.vertexCount = 6;
    d.segmentCount = 1;
    d.route = r::PacketRoute::Screen2D;
    d.depthPath = r::VulkanReplayDrawDepthPath::InvalidateAcceptedFar;
    d.feedbackPlanes = 0;
    s.firstVertex = d.firstVertex;
    s.vertexCount = 6;
    s.destinationSnapshot = {};
    s.snapshotAction = r::VulkanReplayDestinationSnapshotAction::None;
    u.destinationState = {};
    u.depthScaleBias = {0, 1, 0, 1};
    u.textureState = {
        0x100, 0,
        (u.textureState[2] &
         r::replayDrawFlagBit(r::ReplayDrawFlag::ForceMask)) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::Textured) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::RawTexture) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::LimitUvDomain) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenTexture) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenEffect),
        0};
    u.textureWindow = {};
    u.uvDomain = {0, 0, 319, 223};
    u.mgsProjection[1] = float(x);
    d.uniforms = u;
    for (auto p : std::array<std::array<float, 2>, 6>{{{0, top},
                                                       {320, top},
                                                       {0, bottom},
                                                       {320, top},
                                                       {320, bottom},
                                                       {0, bottom}}}) {
      r::vulkan_abi::Vertex v;
      v.positionNative = {float(x) + p[0], p[1]};
      v.uv = p;
      v.color255 = {128, 128, 128};
      batch.vertices.push_back(v);
    }
    for (size_t i = 1; i < g.commands.size(); ++i)
      removed.insert(g.commands[i]);
    constexpr std::array names{"none",       "night_vision", "thermal_vision",
                               "fade_black", "fade_white",   "temporal_blur",
                               "gas_haze"};
    metrics::count(std::string("mgs.screen_effect.pass.") +
                   names[uint32_t(kind)]);
    metrics::count("mgs.screen_effect.replaced_packets", g.commands.size());
  }
  std::vector<r::VulkanReplayBakedCommand> commands;
  commands.reserve(batch.commands.size() - removed.size());
  for (size_t i = 0; i < batch.commands.size(); ++i)
    if (!removed.contains(i))
      commands.push_back(std::move(batch.commands[i]));
  batch.commands = std::move(commands);
}
} // namespace
void prepareScreenReads(r::VulkanReplayBakedBatch &batch,
                        bool replaceActorEffects) {
  if (replaceActorEffects)
    prepareActorEffects(batch);
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
