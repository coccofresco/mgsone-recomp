#include "runtime/psyq/psyq_state.h"

namespace ps1::psyq {

void PsyqState::reset() {
  vsyncCounter.store(0, std::memory_order_relaxed);
  lastVSyncCounter = 0;
  pendingVblanks.store(0, std::memory_order_relaxed);
  blockingVSyncPacing.store(false, std::memory_order_relaxed);
  blockingVSyncTarget.store(0, std::memory_order_relaxed);
  autonomousClockWaiters.store(0, std::memory_order_relaxed);
  cdClockDemand.store(false, std::memory_order_relaxed);
  cdSyncByte.store(0, std::memory_order_relaxed);
  cdReadyByte.store(0, std::memory_order_relaxed);
  cdRemaining = 0;
  cdDestPtr   = 0;
  cdWordCount = 0;
  cdDataCb    = 0;
  cdNotifyCb  = 0;
  gpuSwapCb   = 0;
  intrMask    = 0;
  for (auto &cb : intrCallback) cb = 0;
  for (auto &cb : dmaCallback)  cb = 0;
  callbacksEnabled = true;
  drawSync = GpuDrawSync{};
}

PsyqState &psyq_state() {
  static PsyqState instance;
  return instance;
}

} // namespace ps1::psyq
