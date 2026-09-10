#pragma once

#include "runtime/memory.h"
#include <cstdlib>
#include <fmt/format.h>
#include <string>

namespace ps1::bios::detail {

inline bool biosVerbose() {
  static const bool v = (std::getenv("PS1_BIOS_DEBUG") != nullptr);
  return v;
}

inline std::string readString(Memory &mem, uint32_t addr) {
  std::string s;
  char c;
  while ((c = mem.read8(addr++)) != '\0') {
    s += c;
  }
  return s;
}

} // namespace ps1::bios::detail

#define BIOS_LOG(...)                                                          \
  do {                                                                         \
    if (::ps1::bios::detail::biosVerbose())                                    \
      fmt::print(__VA_ARGS__);                                                 \
  } while (0)
