#include "runtime/overlay_manager.h"
#include <algorithm>
#include <fmt/format.h>

namespace ps1 {

void OverlayManager::registerOverlay(const std::string &name, uint32_t ramBase,
                                     uint32_t size, int index) {
  OverlayEntry entry;
  entry.name = name;
  entry.ramBase = ramBase;
  entry.size = size;
  entry.index = index;
  entry.active = false;
  overlays_.push_back(std::move(entry));
  fmt::print("[Overlay] Registered: {} at 0x{:08X}-0x{:08X} (idx={})\n", name,
             ramBase, ramBase + size, index);
}

std::vector<int> OverlayManager::findOverlapping(uint32_t addr,
                                                  uint32_t size) const {
  std::vector<int> result;
  uint32_t end = addr + size;

  for (size_t i = 0; i < overlays_.size(); ++i) {
    uint32_t ovlStart = overlays_[i].ramBase;
    uint32_t ovlEnd = ovlStart + overlays_[i].size;

    // Check for overlap
    if (addr < ovlEnd && end > ovlStart) {
      result.push_back(static_cast<int>(i));
    }
  }
  return result;
}

void OverlayManager::notifyMemWrite(uint32_t addr, uint32_t writeSize) {
  auto overlapping = findOverlapping(addr, writeSize);

  for (int idx : overlapping) {
    auto &ovl = overlays_[idx];

    // Deactivate any other overlay that occupies the same RAM region
    // (they conflict -- only one can be active at a time)
    for (auto &other : overlays_) {
      if (&other == &ovl)
        continue;
      if (!other.active)
        continue;

      // Check if 'other' overlaps with this overlay's region
      uint32_t oStart = other.ramBase;
      uint32_t oEnd = oStart + other.size;
      uint32_t nStart = ovl.ramBase;
      uint32_t nEnd = nStart + ovl.size;

      if (oStart < nEnd && oEnd > nStart) {
        // Conflict -- deactivate the old one
        fmt::print("[Overlay] Deactivating '{}' (conflict with '{}')\n",
                   other.name, ovl.name);
        other.active = false;
        if (hook_)
          hook_(other.index, false);
      }
    }

    // Activate this overlay
    if (!ovl.active) {
      fmt::print("[Overlay] Activating '{}' (RAM write to 0x{:08X}+{})\n",
                 ovl.name, addr, writeSize);
      ovl.active = true;
      if (hook_)
        hook_(ovl.index, true);
      break; // Only activate one overlay per write notification
    }
  }
}

bool OverlayManager::activateOverlay(const std::string &name) {
  for (auto &ovl : overlays_) {
    if (ovl.name == name) {
      if (!ovl.active) {
        ovl.active = true;
        if (hook_)
          hook_(ovl.index, true);
        fmt::print("[Overlay] Force-activated '{}'\n", name);
      }
      return true;
    }
  }
  return false;
}

bool OverlayManager::deactivateOverlay(const std::string &name) {
  for (auto &ovl : overlays_) {
    if (ovl.name == name) {
      if (ovl.active) {
        ovl.active = false;
        if (hook_)
          hook_(ovl.index, false);
        fmt::print("[Overlay] Force-deactivated '{}'\n", name);
      }
      return true;
    }
  }
  return false;
}

bool OverlayManager::isActive(const std::string &name) const {
  for (const auto &ovl : overlays_) {
    if (ovl.name == name)
      return ovl.active;
  }
  return false;
}

size_t OverlayManager::activeCount() const {
  return std::count_if(overlays_.begin(), overlays_.end(),
                       [](const OverlayEntry &e) { return e.active; });
}

} // namespace ps1
