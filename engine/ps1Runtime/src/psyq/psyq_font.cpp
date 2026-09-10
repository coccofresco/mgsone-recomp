#include "runtime/psyq/psyq_font.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_registry.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fmt/format.h>
#include <string>
#include <unordered_set>

namespace ps1::psyq {

namespace {

// PsyQ supports up to 8 simultaneous streams (FntOpen ids 0..7).  Anything
// past the cap is clamped to the last slot so the caller still gets a
// usable id back instead of -1 (libgpu returns the clamped value too).
constexpr int kMaxSlots = 8;

struct FntSlot {
  bool used = false;
  int16_t x = 0, y = 0;     // top-left of the text window (screen px)
  int16_t w = 0, h = 0;     // window size
  bool isbg = false;        // draw a solid background rect on Flush
  int  capacity = 0;        // n: max characters the stream can hold
  std::string buffer;       // accumulated text (FntPrint)
};

std::array<FntSlot, kMaxSlots> g_slots{};
int g_systemDefault = 0;
int g_nextSlot = 0;

inline void writeGP0(uint32_t w) {
  const auto &cfg = getConfig();
  if (cfg.writeGP0) cfg.writeGP0(w);
}

void warnOnceFor(const char *name) {
  static std::unordered_set<std::string> seen;
  if (seen.insert(name).second)
    fmt::print(stderr,
               "[PSYQ] {} stubbed (no glyph rasterisation) -- buffered text "
               "only\n",
               name);
}

// Read a NUL-terminated C string from PS1 RAM.  Caps at 1 KiB to keep a
// runaway pointer from looping forever; PsyQ format strings are tiny in
// practice.
std::string readCString(recomp_context *ctx, uint32_t addr, size_t cap = 1024) {
  std::string out;
  if (addr == 0) return out;
  out.reserve(64);
  for (size_t i = 0; i < cap; ++i) {
    uint8_t b = ctx->mem->read8(addr + static_cast<uint32_t>(i));
    if (b == 0) break;
    out.push_back(static_cast<char>(b));
  }
  return out;
}

// Pull the i-th printf argument according to the o32 calling convention.
//   i=0 -> $a2, i=1 -> $a3, i>=2 -> caller's stack spill (sp + 0x10 + (i-2)*4).
// FntPrint's $a0=id, $a1=fmt are already consumed by the caller, so the
// "first variadic" lives at $a2.
uint32_t fetchArg(recomp_context *ctx, unsigned i) {
  if (i == 0) return ctx->r[A2];
  if (i == 1) return ctx->r[A3];
  uint32_t sp = ctx->r[SP];
  return ctx->mem->read32(sp + 0x10u + (i - 2u) * 4u);
}

// Tiny printf subset: %s, %d, %u, %x, %X, %c, %%.  Everything else is
// passed through verbatim (so unknown specifiers still produce *something*
// in the buffer rather than silently truncating).  Width/precision flags
// are ignored -- keeps the implementation dependency-free.
std::string formatFnt(recomp_context *ctx, const std::string &fmt) {
  std::string out;
  out.reserve(fmt.size() + 16);
  unsigned argIdx = 0;
  for (size_t i = 0; i < fmt.size(); ++i) {
    char c = fmt[i];
    if (c != '%' || i + 1 >= fmt.size()) {
      out.push_back(c);
      continue;
    }
    char spec = fmt[++i];
    char tmp[32];
    switch (spec) {
    case '%':
      out.push_back('%');
      break;
    case 'c':
      out.push_back(static_cast<char>(fetchArg(ctx, argIdx++) & 0xFF));
      break;
    case 's': {
      uint32_t addr = fetchArg(ctx, argIdx++);
      out.append(readCString(ctx, addr));
      break;
    }
    case 'd':
    case 'i': {
      int32_t v = static_cast<int32_t>(fetchArg(ctx, argIdx++));
      std::snprintf(tmp, sizeof(tmp), "%d", v);
      out.append(tmp);
      break;
    }
    case 'u': {
      uint32_t v = fetchArg(ctx, argIdx++);
      std::snprintf(tmp, sizeof(tmp), "%u", v);
      out.append(tmp);
      break;
    }
    case 'x': {
      uint32_t v = fetchArg(ctx, argIdx++);
      std::snprintf(tmp, sizeof(tmp), "%x", v);
      out.append(tmp);
      break;
    }
    case 'X': {
      uint32_t v = fetchArg(ctx, argIdx++);
      std::snprintf(tmp, sizeof(tmp), "%X", v);
      out.append(tmp);
      break;
    }
    default:
      // Unknown specifier -- keep the raw bytes so the user can tell.
      out.push_back('%');
      out.push_back(spec);
      break;
    }
  }
  return out;
}

inline int normaliseId(int id) {
  if (id < 0) return -1;
  if (id >= kMaxSlots) return kMaxSlots - 1;
  return id;
}

} // namespace

//  FntOpen(x, y, w, h, isbg, n)
//   a0 = x, a1 = y, a2 = w, a3 = h
//   sp+0x10 = isbg, sp+0x14 = n
//   v0 = stream id (0..kMaxSlots-1), or last slot if all are taken
//
//  No PS1-RAM allocation: real PsyQ embeds the font texture and a per-slot
//  primitive buffer in BSS, but we keep all of that in C++ so the recompiled
//  game's BSS layout is irrelevant.  This sidesteps the Crash crash where the
//  PS1-side buffer pointer table dispatches through an uninitialised vtable.
void hle_libgpu_FntOpen(recomp_context *ctx) {
  int16_t x = static_cast<int16_t>(ctx->r[A0]);
  int16_t y = static_cast<int16_t>(ctx->r[A1]);
  int16_t w = static_cast<int16_t>(ctx->r[A2]);
  int16_t h = static_cast<int16_t>(ctx->r[A3]);
  uint32_t sp = ctx->r[SP];
  bool isbg = ctx->mem->read32(sp + 0x10u) != 0;
  int32_t n = static_cast<int32_t>(ctx->mem->read32(sp + 0x14u));

  int id = g_nextSlot;
  if (id >= kMaxSlots) id = kMaxSlots - 1;

  FntSlot &s = g_slots[id];
  s.used     = true;
  s.x        = x;
  s.y        = y;
  s.w        = w;
  s.h        = h;
  s.isbg     = isbg;
  s.capacity = n > 0 ? n : 0;
  s.buffer.clear();

  if (g_nextSlot < kMaxSlots) ++g_nextSlot;

  ctx->r[V0] = static_cast<uint32_t>(id);
}

//  FntPrint(id, fmt, ...)
//   a0 = id (or -1 for "system default")
//   a1 = fmt (PS1 RAM pointer)
//   a2.. = printf args (per o32: a2, a3, then sp+0x10+)
//   v0 = 0 on success, -1 on bad id
//
//  We respect the slot's `capacity` field (PsyQ silently truncates instead
//  of overflowing the per-stream primitive buffer).
void hle_libgpu_FntPrint(recomp_context *ctx) {
  int32_t rawId = static_cast<int32_t>(ctx->r[A0]);
  int id = (rawId < 0) ? g_systemDefault : normaliseId(rawId);
  if (id < 0 || !g_slots[id].used) {
    ctx->r[V0] = static_cast<uint32_t>(-1);
    return;
  }

  uint32_t fmtAddr = ctx->r[A1];
  std::string formatted = formatFnt(ctx, readCString(ctx, fmtAddr));

  FntSlot &s = g_slots[id];
  if (s.capacity > 0 &&
      s.buffer.size() + formatted.size() >
          static_cast<size_t>(s.capacity)) {
    size_t room = static_cast<size_t>(s.capacity) - s.buffer.size();
    if (room > formatted.size()) room = formatted.size();
    s.buffer.append(formatted, 0, room);
  } else {
    s.buffer.append(formatted);
  }

  ctx->r[V0] = 0;
}

//  FntFlush(id)
//   a0 = id (or -1 for "system default")
//   v0 = id processed
//
//  Real PsyQ walks the slot's primitive buffer and DMA-links it onto the
//  active OT.  We don't have glyph primitives; the *crash-safe* behaviour
//  is to (optionally) clear the window with GP0 FillRect, drop the text on
//  the floor, and reset the buffer.  warnOnceFor() makes the reduced
//  fidelity visible in the log.
void hle_libgpu_FntFlush(recomp_context *ctx) {
  int32_t rawId = static_cast<int32_t>(ctx->r[A0]);
  int id = (rawId < 0) ? g_systemDefault : normaliseId(rawId);
  if (id < 0 || !g_slots[id].used) {
    ctx->r[V0] = static_cast<uint32_t>(rawId);
    return;
  }

  FntSlot &s = g_slots[id];
  if (s.isbg && s.w > 0 && s.h > 0) {
    // GP0(0x02): monochrome FillRect -- black with full alpha.  Fast-path
    // background draw matches what FntFlush does on real hardware before
    // it lays down glyphs.
    writeGP0(0x02000000u);
    writeGP0(static_cast<uint32_t>(s.y & 0xFFFF) << 16 |
             static_cast<uint32_t>(s.x & 0xFFFF));
    writeGP0(static_cast<uint32_t>(s.h & 0xFFFF) << 16 |
             static_cast<uint32_t>(s.w & 0xFFFF));
  }

  warnOnceFor("libgpu_FntFlush");
  s.buffer.clear();
  ctx->r[V0] = static_cast<uint32_t>(id);
}

//  FntLoad(tx, ty) -- would upload the 8x8 1bpp font + CLUT to VRAM at
//  (tx, ty).  We don't ship the glyph table, so this is bookkeeping only;
//  warnOnceFor surfaces it in the log if anything actually relies on the
//  texture being present.
void hle_libgpu_FntLoad(recomp_context *ctx) {
  (void)ctx; // tx/ty intentionally unused
  warnOnceFor("libgpu_FntLoad");
}

//  FntSystem(n) -- set the slot id used by FntPrint(-1, ...).
//  v0 = previous default.
//
//  PsyQ exposes this as `SetDumpFnt` on later SDK versions; both names are
//  registered against this handler so games linking either flavour resolve
//  through the registry without an extra alias.
void hle_libgpu_FntSystem(recomp_context *ctx) {
  int prev = g_systemDefault;
  int n = static_cast<int>(static_cast<int32_t>(ctx->r[A0]));
  if (n >= 0 && n < kMaxSlots) g_systemDefault = n;
  ctx->r[V0] = static_cast<uint32_t>(prev);
}

// Test-only state hooks

void psyq_font_reset_for_tests() {
  for (auto &s : g_slots) s = FntSlot{};
  g_systemDefault = 0;
  g_nextSlot = 0;
}

const char *psyq_font_slot_buffer(int id) {
  if (id < 0 || id >= kMaxSlots || !g_slots[id].used)
    return "";
  return g_slots[id].buffer.c_str();
}

// Registry wiring

void psyq_register_libgpu_font() {
  psyq_register("libgpu_FntOpen",   &hle_libgpu_FntOpen);
  psyq_register("libgpu_FntPrint",  &hle_libgpu_FntPrint);
  psyq_register("libgpu_FntFlush",  &hle_libgpu_FntFlush);
  psyq_register("libgpu_FntLoad",   &hle_libgpu_FntLoad);
  psyq_register("libgpu_FntSystem", &hle_libgpu_FntSystem);
  // SetDumpFnt is the post-v3.5 spelling of FntSystem; same behaviour.
  psyq_register("libgpu_SetDumpFnt", &hle_libgpu_FntSystem);
}

} // namespace ps1::psyq
