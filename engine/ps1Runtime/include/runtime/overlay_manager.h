#pragma once

// ps1Runtime -- Overlay Manager
// Manages dynamically-loaded code overlays at runtime.
// When the game loads code from CD into RAM (via BIOS FileRead or DMA),
// the overlay manager activates pre-recompiled overlay functions
// in the dispatch table. When a different overlay is loaded into
// the same RAM region, it deactivates the old one first.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps1 {

// Overlay Entry

struct OverlayEntry {
  std::string name;       // "battle", "menu", etc.
  uint32_t ramBase;       // RAM load address
  uint32_t size;          // Size in bytes
  int index;              // Overlay index (matches recompiler output)
  bool active = false;    // Currently loaded in RAM?
};

// Overlay dispatch hook

/// Callback type to add/remove functions from dispatch table
/// When setActive is true: register all overlay functions for this region
/// When setActive is false: unregister them
using OverlayDispatchHook =
    std::function<void(int overlayIndex, bool setActive)>;

// Overlay Manager

class OverlayManager {
public:
  OverlayManager() = default;

  /// Register an overlay (called during init from recomp overlay table)
  void registerOverlay(const std::string &name, uint32_t ramBase,
                       uint32_t size, int index);

  /// Set the dispatch hook callback
  void setDispatchHook(OverlayDispatchHook hook) {
    hook_ = std::move(hook);
  }

  /// Notify that data has been written to RAM at [addr, addr+size).
  /// This checks if the write overlaps any known overlay region
  /// and activates/deactivates overlays accordingly.
  void notifyMemWrite(uint32_t addr, uint32_t size);

  /// Force-activate a specific overlay by name
  bool activateOverlay(const std::string &name);

  /// Force-deactivate a specific overlay by name
  bool deactivateOverlay(const std::string &name);

  /// Check if a specific overlay is currently active
  bool isActive(const std::string &name) const;

  /// Get all registered overlays
  const std::vector<OverlayEntry> &overlays() const { return overlays_; }

  /// Get count of registered overlays
  size_t overlayCount() const { return overlays_.size(); }

  /// Get count of active overlays
  size_t activeCount() const;

private:
  std::vector<OverlayEntry> overlays_;
  OverlayDispatchHook hook_;

  /// Find all overlays that overlap with [addr, addr+size)
  std::vector<int> findOverlapping(uint32_t addr, uint32_t size) const;
};

} // namespace ps1
