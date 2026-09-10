#pragma once

// ps1Recomp -- Overlay Handler
// Manages PS1 overlay sections -- code blocks dynamically loaded from CD
// into fixed RAM addresses. Handles address conflicts when multiple
// overlays share the same RAM region.

#include <cstdint>
#include <string>
#include <vector>

namespace ps1recomp {

// Overlay Section

/// Represents a dynamically-loaded code section
struct OverlaySection {
  std::string name;                // "battle", "menu", etc.
  uint32_t romOffset;              // Offset in ROM/CD image
  uint32_t ramBase;                // RAM load address (e.g., 0x80040000)
  uint32_t size;                   // Size in bytes
  std::vector<uint32_t> functions; // Function addresses in this overlay
};

// Overlay Handler

class OverlayHandler {
public:
  /// Load overlay definitions from a TOML config file
  /// @return true on success
  bool loadFromConfig(const std::string &configPath);

  /// Add an overlay section manually
  void addOverlay(OverlaySection section);

  /// Check if an address falls within any overlay region
  bool isOverlayAddress(uint32_t addr) const;

  /// Find the overlay section containing an address
  /// Returns nullptr if address is not in any overlay
  const OverlaySection *findOverlay(uint32_t addr) const;

  /// Find all overlays whose RAM region contains this address
  /// (used to detect conflicts -- multiple overlays at same address)
  std::vector<const OverlaySection *> findConflicts(uint32_t addr) const;

  /// Generate a qualified function name: "overlay_{name}__func_{addr:08X}"
  /// Falls back to "func_{addr:08X}" if address is not in any overlay
  std::string qualifiedName(uint32_t addr) const;

  /// Emit a C++ dispatch table for LOOKUP_FUNC runtime resolution
  std::string emitDispatchTable() const;

  /// Get all overlays
  const std::vector<OverlaySection> &overlays() const { return m_overlays; }

  /// Get overlay count
  size_t overlayCount() const { return m_overlays.size(); }

  /// Get last error message
  const std::string &getError() const { return m_error; }

private:
  std::vector<OverlaySection> m_overlays;
  std::string m_error;
};

} // namespace ps1recomp
