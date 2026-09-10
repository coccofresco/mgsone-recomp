#pragma once
#include "runtime/cpu_context.h"
#include <cstdint>
#include <array>
#include <memory>

namespace ps1::bios {
// PS1 kernel TCBs stay in guest RAM. Stackful native continuations retain the
// translated call stack across ChangeTh; no guest instructions are interpreted.
// Snapshots retain only guest TCBs/resume PCs; loading builds new host stacks.
class KernelThreads {
public:
  explicit KernelThreads(recomp_context &ctx);
  ~KernelThreads();
  uint32_t open(uint32_t entry, uint32_t sp, uint32_t gp);
  uint32_t close(uint32_t handle);
  void change(uint32_t handle);
  struct Snapshot {
    uint32_t currentTcb = 0;
    std::array<uint32_t, 64> resumePcs{};
  };
  void captureCurrent(uint32_t resumePc = 0);
  void scheduleSelected(uint32_t resumePc = 0);
  bool canSnapshot() const;
  Snapshot snapshot() const;
  void restart(const Snapshot &state);
  bool active() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace ps1::bios
