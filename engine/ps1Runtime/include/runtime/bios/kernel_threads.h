#pragma once
#include "runtime/cpu_context.h"
#include <cstdint>
#include <memory>

namespace ps1::bios {
// PS1 kernel TCBs stay in guest RAM. Stackful native continuations retain the
// translated call stack across ChangeTh; no guest instructions are interpreted.
class KernelThreads {
public:
  explicit KernelThreads(recomp_context &ctx);
  ~KernelThreads();
  uint32_t open(uint32_t entry, uint32_t sp, uint32_t gp);
  uint32_t close(uint32_t handle);
  void change(uint32_t handle);
  void captureCurrent();
  void scheduleSelected();
  bool active() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace ps1::bios
