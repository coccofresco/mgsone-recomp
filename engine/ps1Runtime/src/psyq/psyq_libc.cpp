#include "runtime/psyq/psyq_libc.h"
#include "runtime/bios/bios.h"
#include "runtime/games/crash3_profile.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_registry.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>

namespace ps1::psyq {

namespace {

// Mirror `BIOS_LOG`'s gate so debug printf only fires under
//    PS1_BIOS_DEBUG=1.  Cached in a function-local static so the
//    getenv() call only happens once per process.
bool isBiosDebug() {
  static const bool v = std::getenv("PS1_BIOS_DEBUG") != nullptr;
  return v;
}

// BIOS A0/B0/C0 dispatch helpers (mirrors `psyq_libapi.cpp`)
inline void dispatchA(recomp_context *ctx, uint32_t index) {
  ctx->r[T1] = index;
  ctx->bios->executeA0();
}

// Read a NUL-terminated PS1-RAM string, capped to keep a malformed
//    pointer from looping forever.  PsyQ format strings live in BSS or
//    .rodata so 4 KiB is generous for sane inputs.
std::string readCString(recomp_context *ctx, uint32_t addr,
                        size_t cap = 4096) {
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

// Variadic argument fetcher.  o32 calling convention: $a0..$a3 are the
//    first four args; the rest spill at sp+0x10, sp+0x14, ...  printf-like
//    callers consume `$a0` for `fmt`, so the first variadic lives at $a1.
//    sprintf consumes `$a0=buf`, `$a1=fmt`, so its first variadic is $a2.
//
//    `argStartReg` selects the index (in the GPR file) of the first
//    variadic.  PsyQ always passes the format string in `$a0`/`$a1` and
//    spills any 5th+ arg to the caller's stack frame.
uint32_t fetchArg(recomp_context *ctx, unsigned argStartReg, unsigned i) {
  unsigned absReg = argStartReg + i;
  if (absReg <= A3) return ctx->r[absReg];
  uint32_t sp = ctx->r[SP];
  return ctx->mem->read32(sp + 0x10u + (absReg - A3 - 1u) * 4u);
}

// Format a printf-subset (`%d %i %u %x %X %p %c %s %%`) into `out`.
//    Width/precision/length modifiers are parsed so they survive the
//    consumed-character bookkeeping but their values are ignored -- this
//    matches stub_printf in bios.cpp and `formatFnt` in psyq_font.cpp.
//    Returns the number of characters appended to `out`.
size_t formatVa(recomp_context *ctx, const std::string &fmt,
                unsigned argStartReg, std::string &out) {
  size_t initial = out.size();
  unsigned argIdx = 0;

  for (size_t i = 0; i < fmt.size(); ++i) {
    char c = fmt[i];
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    ++i;
    if (i >= fmt.size()) break;

    // Skip flags/width/precision/length so leftover specifier is the bare
    // conversion character.  Mirrors the bios.cpp scanner so we accept the
    // same inputs other PsyQ code paths already accept.
    while (i < fmt.size() &&
           (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' ||
            fmt[i] == '0' || fmt[i] == '#'))
      ++i;
    while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
      ++i;
    if (i < fmt.size() && fmt[i] == '.') {
      ++i;
      while (i < fmt.size() &&
             std::isdigit(static_cast<unsigned char>(fmt[i])))
        ++i;
    }
    while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h'))
      ++i;
    if (i >= fmt.size()) break;

    char tmp[32];
    char spec = fmt[i];
    switch (spec) {
    case '%':
      out.push_back('%');
      break;
    case 'c':
      out.push_back(static_cast<char>(
          fetchArg(ctx, argStartReg, argIdx++) & 0xFF));
      break;
    case 's': {
      uint32_t addr = fetchArg(ctx, argStartReg, argIdx++);
      if (addr == 0)
        out.append("(null)");
      else
        out.append(readCString(ctx, addr));
      break;
    }
    case 'd':
    case 'i':
      std::snprintf(tmp, sizeof(tmp), "%d",
                    static_cast<int32_t>(fetchArg(ctx, argStartReg, argIdx++)));
      out.append(tmp);
      break;
    case 'u':
      std::snprintf(tmp, sizeof(tmp), "%u",
                    fetchArg(ctx, argStartReg, argIdx++));
      out.append(tmp);
      break;
    case 'x':
      std::snprintf(tmp, sizeof(tmp), "%x",
                    fetchArg(ctx, argStartReg, argIdx++));
      out.append(tmp);
      break;
    case 'X':
      std::snprintf(tmp, sizeof(tmp), "%X",
                    fetchArg(ctx, argStartReg, argIdx++));
      out.append(tmp);
      break;
    case 'p':
      std::snprintf(tmp, sizeof(tmp), "0x%08x",
                    fetchArg(ctx, argStartReg, argIdx++));
      out.append(tmp);
      break;
    default:
      // Unknown specifier -- preserve the raw bytes so the user can spot it.
      out.push_back('%');
      out.push_back(spec);
      break;
    }
  }
  return out.size() - initial;
}

} // namespace

// Memory routines -- single source of truth in bios.cpp's A0 table
void hle_libc_memcpy(recomp_context *ctx)  { dispatchA(ctx, 0x2A); }
void hle_libc_memset(recomp_context *ctx)  { dispatchA(ctx, 0x2B); }
void hle_libc_memmove(recomp_context *ctx) { dispatchA(ctx, 0x2C); }
void hle_libc_memcmp(recomp_context *ctx)  { dispatchA(ctx, 0x2D); }

// String routines (psx-spx/openbios A-table layout)
void hle_libc_strcat(recomp_context *ctx)  { dispatchA(ctx, 0x15); }
void hle_libc_strncat(recomp_context *ctx) { dispatchA(ctx, 0x16); }
void hle_libc_strcmp(recomp_context *ctx)  { dispatchA(ctx, 0x17); }
void hle_libc_strncmp(recomp_context *ctx) { dispatchA(ctx, 0x18); }
void hle_libc_strcpy(recomp_context *ctx)  { dispatchA(ctx, 0x19); }
void hle_libc_strncpy(recomp_context *ctx) { dispatchA(ctx, 0x1A); }
void hle_libc_strlen(recomp_context *ctx)  { dispatchA(ctx, 0x1B); }

// Math / RNG
void hle_libc_abs(recomp_context *ctx)  { dispatchA(ctx, 0x10); }
void hle_libc_labs(recomp_context *ctx) { dispatchA(ctx, 0x11); }
void hle_libc_rand(recomp_context *ctx) { dispatchA(ctx, 0x2F); }
void hle_libc_srand(recomp_context *ctx) { dispatchA(ctx, 0x30); }

//  atoi(s) -- leading whitespace skip, optional sign, decimal digits only.
//  Stops at first non-digit (no overflow detection beyond int32_t wrap, to
//  match the historical PsyQ behaviour).  Returns 0 for empty/garbage input.
void hle_libc_atoi(recomp_context *ctx) {
  std::string s = readCString(ctx, ctx->r[A0]);
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
    ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    neg = (s[i] == '-');
    ++i;
  }
  int32_t value = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    value = value * 10 + (s[i] - '0');
    ++i;
  }
  if (neg) value = -value;
  ctx->r[V0] = static_cast<uint32_t>(value);
}

//  printf(fmt, ...) -- debug stdout, gated on PS1_BIOS_DEBUG.  Returns the
//  formatted-character count regardless of whether the gate suppresses
//  the actual write, so callers that test the return value still observe
//  realistic numbers.  Format args start at $a1 (a0 is the fmt pointer).
void hle_libc_printf(recomp_context *ctx) {
  // Per-game observers receive the raw, immutable call boundary and remain
  // inert unless their exact profile/format address is active. Formatting and
  // all architectural printf behavior below remain platform-owned.
  ps1::games::crash3::observeDebugPrintf(*ctx, ctx->r[A0]);
  std::string fmtStr = readCString(ctx, ctx->r[A0]);
  std::string out;
  size_t n = formatVa(ctx, fmtStr, /*argStartReg=*/A1, out);
  if (isBiosDebug())
    fmt::print("{}", out);
  ctx->r[V0] = static_cast<uint32_t>(n);
}

//  sprintf(buf, fmt, ...) -- write formatted bytes to PS1 RAM at $a0,
//  including the trailing NUL.  Returns the character count *excluding*
//  the NUL (libc convention).  Always runs -- debug gating only suppresses
//  the host stdout side of printf.  Format args start at $a2.
void hle_libc_sprintf(recomp_context *ctx) {
  uint32_t bufAddr = ctx->r[A0];
  std::string fmtStr = readCString(ctx, ctx->r[A1]);
  std::string out;
  formatVa(ctx, fmtStr, /*argStartReg=*/A2, out);

  if (bufAddr != 0) {
    for (size_t i = 0; i < out.size(); ++i)
      ctx->mem->write8(bufAddr + static_cast<uint32_t>(i),
                       static_cast<uint8_t>(out[i]));
    ctx->mem->write8(bufAddr + static_cast<uint32_t>(out.size()), 0);
  }
  ctx->r[V0] = static_cast<uint32_t>(out.size());
}

// Test-only state hooks

void psyq_libc_reset_for_tests() {
  // RNG state belongs to the Bios instance and its savestate; srand uses
  // A0:0x30 -- re-seed via a direct dispatchA when needed in tests.
  // Nothing local to reset here yet.
}

// Registry wiring

namespace {

// Library prefixes a libc function may surface under.  The matcher tags
// each detected function with the .LIB it was extracted from; aliasing
// across all observed prefixes keeps the registry resilient when new
// games introduce new prefixes (e.g. `libsn_memset` from PsyQ 4.0).
constexpr const char *kLibPrefixes[] = {
    "libc", "libapi", "libgpu", "libcd", "libetc", "libgte", "libsn",
};

void registerAllPrefixes(const char *base, PsyqHleFn fn) {
  std::string key;
  key.reserve(24);
  for (const char *prefix : kLibPrefixes) {
    key.assign(prefix);
    key.push_back('_');
    key.append(base);
    psyq_register(key.c_str(), fn);
  }
}

} // namespace

void psyq_register_libc() {
  // Memory
  registerAllPrefixes("memcpy",  &hle_libc_memcpy);
  registerAllPrefixes("memset",  &hle_libc_memset);
  registerAllPrefixes("memmove", &hle_libc_memmove);
  registerAllPrefixes("memcmp",  &hle_libc_memcmp);
  // PsyQ also ships `_memmove` (LIBAPI) -- wire the underscore alias.
  psyq_register("libapi__memmove", &hle_libc_memmove);
  psyq_register("libc__memmove",   &hle_libc_memmove);

  // String
  registerAllPrefixes("strcpy",  &hle_libc_strcpy);
  registerAllPrefixes("strncpy", &hle_libc_strncpy);
  registerAllPrefixes("strcmp",  &hle_libc_strcmp);
  registerAllPrefixes("strncmp", &hle_libc_strncmp);
  registerAllPrefixes("strlen",  &hle_libc_strlen);
  registerAllPrefixes("strcat",  &hle_libc_strcat);

  // Math / RNG
  registerAllPrefixes("abs",   &hle_libc_abs);
  registerAllPrefixes("labs",  &hle_libc_labs);
  registerAllPrefixes("rand",  &hle_libc_rand);
  registerAllPrefixes("srand", &hle_libc_srand);

  // Conversion / formatting
  registerAllPrefixes("atoi",    &hle_libc_atoi);
  registerAllPrefixes("printf",  &hle_libc_printf);
  registerAllPrefixes("sprintf", &hle_libc_sprintf);
}

} // namespace ps1::psyq
