// ps1Recomp -- Overlay Handler Implementation
// Manages PS1 overlay sections with address conflict detection

#include "ps1recomp/overlay_handler.h"
#include <algorithm>
#include <fmt/format.h>
#include <fstream>
#include <toml.hpp>

namespace ps1recomp {

// Config Loading

bool OverlayHandler::loadFromConfig(const std::string &configPath) {
  try {
    auto config = toml::parse(configPath);

    if (!config.contains("overlays")) {
      // No overlays section -- valid config, just empty
      return true;
    }

    const auto &overlays =
        toml::find<std::vector<toml::value>>(config, "overlays");

    for (const auto &ov : overlays) {
      OverlaySection section;
      section.name = toml::find<std::string>(ov, "name");
      section.romOffset =
          static_cast<uint32_t>(toml::find<int64_t>(ov, "rom_offset"));
      section.ramBase =
          static_cast<uint32_t>(toml::find<int64_t>(ov, "ram_base"));
      section.size = static_cast<uint32_t>(toml::find<int64_t>(ov, "size"));

      // Functions list is optional
      if (ov.contains("functions")) {
        auto funcs = toml::find<std::vector<int64_t>>(ov, "functions");
        for (auto f : funcs) {
          section.functions.push_back(static_cast<uint32_t>(f));
        }
      }

      m_overlays.push_back(std::move(section));
    }

    return true;
  } catch (const std::exception &e) {
    m_error = fmt::format("Failed to load overlay config: {}", e.what());
    return false;
  }
}

// Overlay Management

void OverlayHandler::addOverlay(OverlaySection section) {
  m_overlays.push_back(std::move(section));
}

// Address Lookup

bool OverlayHandler::isOverlayAddress(uint32_t addr) const {
  return findOverlay(addr) != nullptr;
}

const OverlaySection *OverlayHandler::findOverlay(uint32_t addr) const {
  for (const auto &ov : m_overlays) {
    if (addr >= ov.ramBase && addr < ov.ramBase + ov.size) {
      return &ov;
    }
  }
  return nullptr;
}

std::vector<const OverlaySection *>
OverlayHandler::findConflicts(uint32_t addr) const {
  std::vector<const OverlaySection *> result;
  for (const auto &ov : m_overlays) {
    if (addr >= ov.ramBase && addr < ov.ramBase + ov.size) {
      result.push_back(&ov);
    }
  }
  return result;
}

// Naming

std::string OverlayHandler::qualifiedName(uint32_t addr) const {
  auto conflicts = findConflicts(addr);

  if (conflicts.empty()) {
    return fmt::format("func_{:08X}", addr);
  }

  if (conflicts.size() == 1) {
    // Single overlay -- still prefix for clarity
    return fmt::format("overlay_{}__{:08X}", conflicts[0]->name, addr);
  }

  // Multiple overlays at same address -- caller must disambiguate
  // Return first match with prefix
  return fmt::format("overlay_{}__{:08X}", conflicts[0]->name, addr);
}

// Dispatch Table Emission

std::string OverlayHandler::emitDispatchTable() const {
  if (m_overlays.empty()) {
    return "// No overlays -- dispatch table not needed\n";
  }

  std::string result;
  result += "// Auto-generated overlay dispatch table\n";
  result +=
      "void* lookup_overlay_func(uint32_t addr, uint32_t active_overlay) {\n";
  result += "    switch (active_overlay) {\n";

  for (size_t i = 0; i < m_overlays.size(); ++i) {
    const auto &ov = m_overlays[i];
    result += fmt::format("        case {}: // {}\n", i, ov.name);
    result += "            switch (addr) {\n";

    for (uint32_t func : ov.functions) {
      auto name = fmt::format("overlay_{}__{:08X}", ov.name, func);
      result += fmt::format(
          "                case 0x{:08X}: return (void*){};\n", func, name);
    }

    result += "            }\n";
    result += "            break;\n";
  }

  result += "    }\n";
  result += "    return nullptr;\n";
  result += "}\n";

  return result;
}

} // namespace ps1recomp
