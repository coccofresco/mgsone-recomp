#pragma once

// ps1Runtime -- Save State System
// Serializes/deserializes complete PS1 hardware state to/from file

#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/cpu_context.h"
#include "runtime/dma/dma.h"
#include "runtime/gpu/gpu.h"
#include "runtime/input/input.h"
#include "runtime/mdec/mdec.h"
#include "runtime/memory.h"
#include "runtime/spu/spu.h"
#include "runtime/timers/timers.h"
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ps1 {

namespace bios {
class Bios;
}

// Save state file format:
//   [Header: 16 bytes]
//   [CPU Context: ~300 bytes]
//   [RAM: 2MB]
//   [Scratchpad: 1KB]
//   [GPU VRAM: 1MB]
//   [GPU State: ~64 bytes]
//   [SPU Sound RAM: 512KB]
//   [SPU Registers: ~2KB]
//   [CDROM State: ~128 bytes]
//   [DMA State: ~256 bytes]
//   [Timers State: ~64 bytes]
//   [IRQ State: ~8 bytes]
//   [Input/SIO State + Memory Cards: ~256KB]

struct SaveStateHeader {
  char magic[4] = {'P', 'S', '1', 'S'}; // "PS1S"
  uint32_t version = 12;
  uint32_t flags = 0;
  uint32_t checksum = 0; // v6+: FNV-1a of full payload; v1-v5: RAM-only XOR
};

// Loading replaces the guest timeline. Unwinding to main_host before
// dispatching the restored PC prevents the old native recompilation call
// stack from continuing over the newly restored RAM/register image.
struct SaveStateTimelineReload final {};

class SaveStateCoordinator {
public:
  enum class Action : uint8_t { Save, Load };
  struct Request {
    Action action = Action::Save;
    std::string path;
  };

  // Host thread: enqueue one operation. A second hotkey cannot overwrite an
  // operation which is already waiting for the next guest checkpoint.
  bool request(Action action, std::string path);

  // Game thread: called at a resumable HLE boundary (currently end-of-VSync).
  // The resume PC is persisted in the CPU context before the host sees it.
  void checkpoint(recomp_context &ctx, uint32_t resumePc);

  // Host thread: run an acknowledged operation while the game thread is
  // parked. Returns false when no checkpoint is ready yet.
  bool service(const std::function<bool(const Request &)> &operation);

  bool pending() const;
  // A host pause uses the same resumable boundary as save/load. While held,
  // operations can complete without letting any guest code run between them.
  void setHostPaused(bool paused);
  bool hostPaused() const;
  void cancel();

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable resumed_;
  std::optional<Request> request_;
  bool gameThreadParked_ = false;
  bool operationComplete_ = false;
  bool reloadTimeline_ = false;
  bool cancelled_ = false;
  bool hostPauseRequested_ = false;
};

class SaveState {
public:
  struct Info {
    bool exists = false, supported = false;
    uint32_t version = 0;
    uintmax_t bytes = 0;
    std::string savedAt;
  };
  // Header/date only for UI; full validation still happens on load.
  static Info inspect(const std::string &path);
  // Save entire system state to file
  static bool save(const std::string &path, const recomp_context &ctx,
                   const Memory &mem, const gpu::GPU &gpu, const spu::SPU &spu,
                   const DMA &dma, const cdrom::CdromController &cdrom,
                   const Timers &timers, const InterruptController &irq,
                   const input::InputController &input,
                   const mdec::MDEC *mdec = nullptr,
                   const bios::Bios *bios = nullptr);

  // Load entire system state from file
  static bool load(const std::string &path, recomp_context &ctx, Memory &mem,
                   gpu::GPU &gpu, spu::SPU &spu, DMA &dma,
                   cdrom::CdromController &cdrom, Timers &timers,
                   InterruptController &irq, input::InputController &input,
                   mdec::MDEC *mdec = nullptr, bios::Bios *bios = nullptr);

  // Player-facing recovery only. Diagnostics/TAS keep strict load semantics.
  // Unsupported newer states and unavailable media never trigger rollback.
  static bool loadRecovering(const std::string &path, recomp_context &ctx, Memory &mem,
                   gpu::GPU &gpu, spu::SPU &spu, DMA &dma,
                   cdrom::CdromController &cdrom, Timers &timers,
                   InterruptController &irq, input::InputController &input,
                   mdec::MDEC *mdec, bios::Bios *bios, std::string &recoverySource);

  // Check if file is a valid save state
  static bool isValid(const std::string &path);

private:
  // Helpers for binary I/O
  static void writeBytes(std::ofstream &f, const void *data, size_t size);
  static bool readBytes(std::ifstream &f, void *data, size_t size);
  static uint32_t computeChecksum(const void *data, size_t size);
};

} // namespace ps1
