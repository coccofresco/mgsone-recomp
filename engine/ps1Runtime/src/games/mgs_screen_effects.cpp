#include "runtime/games/mgs_screen_effects.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/games/mgs_surface_effects.h"
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
  bool furnace = false;
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
  captureSurfaceEffects(ram);
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
  // DMO sequences reuse goggleir/scn_mask for a red cinematic grade. The
  // actor name and even STATE_THERMG do not establish a thermal camera:
  // the intro's red lighting must not turn characters into hot silhouettes.
  const bool cinematic =
      valid(0x800adb3c, 4) && (word(0x800adb3c) & 0x80000000u);
  // USA gvd.c Act (0x80014c30): GV_Time is gp+76, gp=0x800ad9fc.
  // Capture guest time, never presentation/submission IDs or a host clock.
  // Paused sensors use a deterministic stationary pattern. Restoring a state
  // restores the phase too, without maintaining a second host-side timeline.
  const float sensorFrame =
      valid(0x800ada48, 4) && valid(0x800ae0a0, 4) && word(0x800ae0a0) == 0
          ? float(word(0x800ada48) & 0xffffu)
          : 0.f;
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
        effect.parameters[2][3] = sensorFrame;
        ranges.push_back({p & 0x1fffffff, stride, count, firstPart, effect});
      };
      if (named("menuman.c") && valid(actor, 36)) {
        // radio.c helper15 emits two adjacent rolling-band SPRTs AFTER all
        // Codec artwork and BEFORE draw_radio_message in the reversed OT.
        // Their live arena, exact color/layout and direct framebuffer pages
        // provide a composition boundary even when no dialogue is displayed.
        const auto arena = word(actor + 32);
        if (valid(arena, 20)) {
          const auto next = word(arena), end = word(arena + 8);
          for (unsigned b = 0; b < 2; ++b) {
            const auto begin = word(arena + 12 + b * 4);
            if (next < begin || next > end || next - begin > 65536 ||
                !valid(begin, next - begin)) continue;
            for (auto q = begin; q + 40 <= next; q += 4) {
              if (word(q) >> 24 != 4 || word(q + 20) >> 24 != 4 ||
                  word(q + 4) != 0x66181818 || word(q + 24) != 0x66181818)
                continue;
              const auto xy = word(q + 8), uv = word(q + 12) & 65535u;
              const auto wh = word(q + 16), width = wh & 65535u;
              const auto y = xy >> 16;
              if ((width != 192 && width != 256) || (xy & 65535u) != 0 ||
                  y >= 128 || wh >> 16 != 8 ||
                  uv != ((width == 192 ? 64u : 0u) | (y << 8)) ||
                  word(q + 28) != (width | (y << 16)) ||
                  (word(q + 32) & 65535u) != y << 8 ||
                  word(q + 36) != ((320 - width) | (8u << 16))) continue;
              r::MgsScreenEffect e;
              e.kind = Kind::CodecScreen;
              e.parts = 2;
              e.parameters[0][0] = float(width);
              add(q + 4, 20, 2, 0, e);
            }
          }
        }
      }
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
            e.kind = color == 0x62287838 ? Kind::NightVision
                     : cinematic         ? Kind::CinematicRedTint
                                         : Kind::ThermalVision;
            if (e.kind == Kind::CinematicRedTint)
              e.parameters[0][0] = float(color & 255u) / 255.f;
            // A DMO camera feed deliberately uses CRT bandwidth/bloom; the
            // same green mask during gameplay remains a night-vision optic.
            e.parameters[0][1] = cinematic ? 1.f : 0.f;
            e.parts = 113;
            add(tile + 4, 16, 1, 0, e);
            add(p + 68 + buffer * 112 * 16, 16, 112, 1, e);
          }
        }
      } else if (level == 3 && (named("stgfd_io.c") || named("red_alrt.c")) &&
                 valid(actor, 36)) {
        // Both own a DR_TPAGE[2] + TILE[2] allocation. Use the actor AND
        // exact layout; a red packet alone never identifies an effect.
        const auto p = word(actor + 32);
        if (valid(p, 48)) {
          for (uint32_t b = 0; b < 2; ++b) {
            const auto tile = p + 16 + b * 16;
            if ((word(tile + 4) >> 24) != 0x62 ||
                word(tile + 8) != 0xff90ff60 || word(tile + 12) != 0x00e00140)
              continue;
            r::MgsScreenEffect e;
            e.kind = named("stgfd_io.c") ? Kind::StunFlash : Kind::AlertTint;
            add(tile + 4, 16, 1, 0, e);
          }
        }
      } else if (level == 7 && named("blur.c") && valid(actor, 120)) {
        // Four feedback quadrants become one continuous radial shutter.
        // f54 selects the original zoom/distortion variant; f60 is a delay.
        if (word(actor + 96) == 0 && word(actor + 84) <= 3) {
          for (uint32_t b = 0; b < 2; ++b) {
            const auto p = word(actor + 36 + b * 16);
            if (!valid(p, 4 * 40))
              continue;
            bool layout = true;
            for (unsigned k = 0; k < 4; ++k)
              layout &= (word(p + k * 40 + 4) >> 24) == 0x2e;
            if (!layout)
              continue;
            r::MgsScreenEffect e;
            e.kind = Kind::ZoomBlur;
            e.parts = 4;
            e.parameters[0] = {
                float(word(actor + 84)), float(word(actor + 88) != 0),
                float(word(actor + 104)), 224.f - float(word(actor + 108))};
            add(p + 4, 40, 4, 0, e);
          }
        }
      } else if (level == 7 && named("focus.c") && valid(actor, 64)) {
        const auto p = word(actor+32);
        if (valid(p,208) && word(actor+52)>0 && word(actor+60)<65536) {
          r::MgsScreenEffect e;
          e.kind = Kind::Focus;
          e.parts = 2;
          e.parameters[0] = {float(word(actor+52)), float(word(actor+60)), 0, 0};
          for (uint32_t b=0;b<2;++b) {
            add(p+116+b*20,20,1,0,e);
            add(p+172+b*20,20,1,1,e);
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
  bindSurfaceEffect(packet, commandAddress);
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
    if (e.kind == Kind::CodecScreen) {
      const float width = e.part == 0 ? e.parameters[0][0] : 320 - e.parameters[0][0];
      const float left = e.part == 0 ? 0 : e.parameters[0][0];
      if (packet.opcode != 0x66 || !packet.textured ||
          packet.state.texturePage.depth != r::TextureDepth::Direct15 ||
          blend != r::BlendMode::BackgroundPlusForeground ||
          packet.vertices[1].x - packet.vertices[0].x != width ||
          packet.vertices[2].y - packet.vertices[0].y != 8 ||
          packet.vertices[0].x - packet.state.drawOffset.x != left)
        return;
    } else if (e.kind == Kind::ZoomBlur) {
      if (packet.opcode != 0x2e || !packet.textured ||
          packet.state.texturePage.depth != r::TextureDepth::Direct15 ||
          (blend != r::BlendMode::HalfBackgroundPlusHalfForeground &&
           blend != r::BlendMode::BackgroundPlusQuarterForeground))
        return;
    } else if (e.kind == Kind::Focus) {
      if (packet.opcode != 0x66 || !packet.textured ||
          packet.state.texturePage.depth != r::TextureDepth::Direct15 ||
          blend != r::BlendMode::HalfBackgroundPlusHalfForeground ||
          packet.vertices[1].x-packet.vertices[0].x != 160 ||
          packet.vertices[2].y-packet.vertices[0].y != 224) return;
    } else if (e.kind == Kind::TemporalBlur) {
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
      if (e.kind == Kind::StunFlash || e.kind == Kind::AlertTint) {
        if (packet.vertices[1].x - packet.vertices[0].x != 320 ||
            packet.vertices[2].y - packet.vertices[0].y != 224 ||
            (e.kind == Kind::StunFlash &&
             blend != r::BlendMode::BackgroundPlusForeground))
          return;
        const auto c = packet.vertices[0].color;
        e.parameters[0] = {float(c.r) / 255.f, float(c.g) / 255.f,
                           float(c.b) / 255.f, float(blend)};
      } else if (e.kind == Kind::FadeBlack || e.kind == Kind::FadeWhite) {
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
  // NewFurnace and WaterView share name/map/bounds/color. Verify this complete
  // ownership tuple rather than calling every red water volume a furnace.
  std::vector<uint32_t> furnaces;
  const uint32_t furnaceHead = 0x800af390 + 5 * 68;
  if (valid(furnaceHead, 68)) {
    uint32_t prev = furnaceHead, current = word(furnaceHead + 4);
    for (unsigned guard = 0;
         current != furnaceHead + 32 && current && guard < 1024; ++guard) {
      if (!valid(current, 64) || word(current) != prev)
        break;
      const uint32_t name = word(current + 20);
      if (word(current + 8) && valid(name, 10) &&
          !std::memcmp(ram.data() + (name & 0x1fffffffu), "furnace.c", 10))
        furnaces.push_back(current);
      prev = current;
      current = word(current + 4);
    }
    if (current != furnaceHead + 32 || word(current) != prev ||
        word(current + 4))
      furnaces.clear();
  }
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
      if (valid(prims, 9912)) {
        const bool furnace =
            std::any_of(furnaces.begin(), furnaces.end(), [&](uint32_t f) {
              return !std::memcmp(ram.data() + ((f + 32) & 0x1fffffffu),
                                  ram.data() + ((actor + 32) & 0x1fffffffu),
                                  24) &&
                     (word(f + 56) & 0xffffffu) ==
                         (word(actor + 60) & 0xffffffu);
            });
        waterActors.push_back({actor & 0x1fffffffu, prims & 0x1fffffffu,
                               word(actor + 72) - 1u, furnace});
      }
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
      packet.furnaceHeat = actor.furnace;
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
  const auto &area = draw.segments[0].scissor;
  const uint32_t scale = batch.internalScale;
  const auto surface = r::MgsSurfaceKind(uint32_t(u.mgsSurfaceEffect[0][0]));
  const bool localOptics = (surface >= r::MgsSurfaceKind::Smoke &&
                            surface <= r::MgsSurfaceKind::Star) ||
                           (surface >= r::MgsSurfaceKind::Bubble &&
                            surface <= r::MgsSurfaceKind::Liquid) ||
                           surface==r::MgsSurfaceKind::Camo ||
                           surface==r::MgsSurfaceKind::Glass ||
                           surface==r::MgsSurfaceKind::Water ||
                           surface==r::MgsSurfaceKind::Footprint ||
                           surface==r::MgsSurfaceKind::Shadow ||
                           surface==r::MgsSurfaceKind::Wind ||
                           surface==r::MgsSurfaceKind::Beam ||
                           surface==r::MgsSurfaceKind::Snow ||
                           surface==r::MgsSurfaceKind::Ripple ||
                           surface==r::MgsSurfaceKind::Mosaic ||
                           surface==r::MgsSurfaceKind::SnowStorm ||
                           surface==r::MgsSurfaceKind::Droplet;
  if (localOptics && u.mgsSurfaceEffect[2][3]>0 && area.y==0 &&
      area.width==320*scale && area.height==224*scale &&
      (area.x==0 || area.x==320*scale))
    return ScreenRead{{0,0,640,224}, {area.x/scale,0,320,224},
                       // Adjacent bubbles refract the same scene. Their
                       // alpha composes on LIVE color, so actor boundaries
                       // need neither recursive refraction nor full 4K copies.
                       surface==r::MgsSurfaceKind::Bubble
                           ? (0x8000000fu | ((area.x/scale/320u)<<8))
                           : uint32_t(u.mgsSurfaceEffect[2][3]),
                       !(surface==r::MgsSurfaceKind::Glow && u.mgsSurfaceEffect[2][2]!=0)};
  if (!(u.textureState[2] &
        r::replayDrawFlagBit(r::ReplayDrawFlag::Textured)) ||
      ((u.textureState[0] >> 7) & 3) != 2 || u.mgsTextureOrigin[2] != 0 ||
      u.mgsPointBytes != std::array<uint32_t, 4>{} || u.textureWindow[0] != 0 ||
      u.textureWindow[1] != 0)
    return {};
  // The live draw environment proves the expected buffer layout. Menus or
  // other modes with a different layout keep their existing texture path.
  const auto &scissor = draw.segments[0].scissor;
  if (scissor.y != 0 || scissor.height != 224 * scale ||
      scissor.width != 320 * scale ||
      (scissor.x != 0 && scissor.x != 320 * scale))
    return {};
  if (u.textureState[2] &
      r::replayDrawFlagBit(r::ReplayDrawFlag::WaterRefraction)) {
    const uint32_t x = scissor.x / scale;
    // Underwater extinction uses physical scene depth. Furnace shimmer
    // shares this actor but needs color only, with no underwater grading.
    return ScreenRead{{x, 0, 320, 224}, {x, 6, 320, 212}, 0,
                      u.mgsViewRows[4][0] == 0.0f};
  }
  if (u.textureState[2] &
      r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenEffect)) {
    const uint32_t x = scissor.x / scale;
    if (u.mgsViewRows[3][1] == float(r::MgsScreenEffectKind::Focus))
      return ScreenRead{{x,0,320,224}, {x,0,320,224}, 0, true};
    if (u.mgsViewRows[3][1] == float(r::MgsScreenEffectKind::ZoomBlur))
      return ScreenRead{{0, 0, 640, 224}, {x, 0, 320, 224}, 0, true};
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
  if (minU < 0 || minV < 0 || maxU > 256 || maxV > 256)
    return {};
  // A screen-read sprite may straddle the viewport. The intro's two 160px
  // feedback halves start at X=-2/158. Rejecting the first half mixed native
  // RGB555 with the modern scene, visibly cutting bodies/effects at X=158.
  // Track only rasterized destinations; source bounds retain the full UV span.
  minX = std::max(minX, float(scissor.x) / scale);
  minY = std::max(minY, float(scissor.y) / scale);
  maxX = std::min(maxX, float(scissor.x + scissor.width) / scale);
  maxY = std::min(maxY, float(scissor.y + scissor.height) / scale);
  if (maxX <= minX || maxY <= minY)
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
        u.mgsViewRows[3][1] > float(Kind::CodecScreen) ||
        u.mgsProjection[1] <= 0 || u.mgsViewRows[3][0] < 1 ||
        u.mgsViewRows[3][0] > 113 || s.y != 0 ||
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
  std::map<size_t, r::VulkanReplayBakedCommand> inserted;
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
    auto replacement = batch.commands[g.commands.front()];
    auto &d = std::get<r::VulkanReplayBakedDraw>(replacement.payload);
    auto &s = d.segments[0];
    auto &u = s.uniforms;
    const auto kind = Kind(uint32_t(u.mgsViewRows[3][1]));
    const auto x = s.scissor.x / batch.internalScale;
    const float top = kind == Kind::TemporalBlur ? 24
                      : kind == Kind::ZoomBlur
                          ? std::clamp(u.mgsViewRows[0][2], 0.f, 223.f)
                          : 0;
    const float bottom = kind == Kind::TemporalBlur ? 184
                         : kind == Kind::ZoomBlur
                             ? std::clamp(u.mgsViewRows[0][3], top + 1.f, 224.f)
                             : 224;
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
         (r::replayDrawFlagBit(r::ReplayDrawFlag::ForceMask) |
          r::replayDrawFlagBit(r::ReplayDrawFlag::ModernColor))) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::Textured) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::RawTexture) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::LimitUvDomain) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenTexture) |
            r::replayDrawFlagBit(r::ReplayDrawFlag::ScreenEffect),
        0};
    u.textureWindow = {};
    u.uvDomain = {0, 0, 319, 223};
    u.mgsProjection[1] = float(x);
    u.mgsSurfaceEffect = {};
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
    if (kind == Kind::CodecScreen) {
      // Keep the native rolling band and insert one tube reconstruction after
      // it. Subtitle/save-dialog sprites retain their original later order.
      inserted.emplace(g.commands.back(), std::move(replacement));
    } else {
      batch.commands[g.commands.front()] = std::move(replacement);
      for (size_t i = 1; i < g.commands.size(); ++i)
        removed.insert(g.commands[i]);
    }
    constexpr std::array names{
        "none",       "night_vision",  "thermal_vision", "fade_black",
        "fade_white", "temporal_blur", "gas_haze",       "cinematic_red_tint",
        "zoom_blur",  "stun_flash",    "alert_tint", "focus", "codec_screen"};
    metrics::count(std::string("mgs.screen_effect.pass.") +
                   names[uint32_t(kind)]);
    metrics::count("mgs.screen_effect.replaced_packets", g.commands.size());
  }
  std::vector<r::VulkanReplayBakedCommand> commands;
  commands.reserve(batch.commands.size() - removed.size() + inserted.size());
  for (size_t i = 0; i < batch.commands.size(); ++i) {
    if (!removed.contains(i))
      commands.push_back(std::move(batch.commands[i]));
    if (auto it = inserted.find(i); it != inserted.end())
      commands.push_back(std::move(it->second));
  }
  batch.commands = std::move(commands);
}
} // namespace
void prepareColorPresentation(r::VulkanReplayBakedBatch &batch) {
  // This function is entered only by the MGS presenter. Color precision is
  // a presentation policy, not a mutation of recorded/native GPU commands.
  for (auto &command : batch.commands)
    if (auto *draw = std::get_if<r::VulkanReplayBakedDraw>(&command.payload)) {
      draw->uniforms.textureState[2] |=
          r::replayDrawFlagBit(r::ReplayDrawFlag::ModernColor);
      for (uint32_t i = 0; i < draw->segmentCount; ++i)
        draw->segments[i].uniforms.textureState[2] |=
            r::replayDrawFlagBit(r::ReplayDrawFlag::ModernColor);
    }
}
void prepareScreenReads(r::VulkanReplayBakedBatch &batch,
                        bool replaceActorEffects) {
  if (replaceActorEffects)
    prepareActorEffects(batch);
  std::set<std::tuple<float, float, float, uint32_t>> waterPasses;
  std::map<std::tuple<float,float,uint32_t>,uint32_t> rippleParts;
  const auto replace = [&](auto &command) {
    auto *draw = std::get_if<r::VulkanReplayBakedDraw>(&command.payload);
    if (!draw || !screenRead(*draw, batch))
      return false;
    auto &u = draw->segments[0].uniforms;
    if(u.mgsSurfaceEffect[0][0]==float(r::MgsSurfaceKind::Ripple)) {
      // All eight native wedges carry the same complete source-space ring.
      // The first visible wedge supplies its original composition position;
      // repetitions of a part start a new submission, never erase a new pass.
      const auto key=std::tuple{u.mgsSurfaceEffect[2][3],u.mgsSurfaceEffect[0][1],
                                 draw->segments[0].scissor.x};
      const auto part=uint32_t(u.mgsSurfaceEffect[0][3]);
      if(part<8) {
        auto &seen=rippleParts[key];const uint32_t bit=1u<<part;
        if(seen&bit) seen=0;
        const bool duplicate=seen!=0;seen|=bit;
        if(duplicate) { metrics::count("mgs.effects.ripple.replaced_wedges");return true; }
        metrics::count("mgs.effects.ripple.pass");
      }
    }
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
      metrics::count(u.mgsViewRows[4][0] != 0.f
                         ? "mgs.screen_effect.pass.furnace_heat"
                         : "mgs.screen_effect.pass.water_view");
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
  group_ = 0;
  writes_.clear();
}
bool ScreenReadTracker::needsSnapshot(const ScreenRead &read) {
  // An optical group (one actor, or a consecutive bubble plume) reads the
  // scene before the group, not recursively refracted preceding particles.
  if (read.group) {
    const bool refresh = !valid_ || group_ != read.group;
    valid_ = true;
    group_ = read.group;
    writes_.clear();
    writes_.push_back(read.destination);
    return refresh;
  }
  group_ = 0;
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
