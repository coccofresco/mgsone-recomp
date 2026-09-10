#pragma once

#include "runtime/cdrom/virtual_fs.h"
#include "runtime/memory.h"
#include "runtime/overlay_manager.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>
namespace ps1::input { class InputController; class MemoryCard; }

namespace ps1::bios {

struct FileDescriptor {
  bool inUse;
  std::string path;
  std::vector<uint8_t> data;
  uint32_t cursor;
  input::MemoryCard *card = nullptr;
  unsigned firstBlock = 0;
  unsigned cardSlot = 0;
};

class FileIO {
public:
  FileIO(cdrom::VirtualFs &fs, Memory &mem);
  ~FileIO() = default;

  void setInputController(input::InputController* input) { input_ = input; }
  void setCardCompletion(std::function<void(unsigned, unsigned, bool)> callback) { cardCompletion_ = std::move(callback); }
  int32_t write(int32_t fd, uint32_t source, int32_t size);
  uint32_t firstFile(const std::string &pattern, uint32_t output);
  uint32_t nextFile(uint32_t output);
  bool erase(const std::string &path);
  bool format(const std::string &path);
  // BIOS API
  int32_t open(const char *path, int32_t mode);
  int32_t close(int32_t fd);
  int32_t read(int32_t fd, uint32_t dstAddr, int32_t length);
  int32_t lseek(int32_t fd, int32_t offset, int32_t whence);

  /// Set overlay manager for load-time overlay activation
  void setOverlayManager(OverlayManager *mgr) { overlayMgr_ = mgr; }

private:
  input::InputController* input_ = nullptr;
  std::function<void(unsigned, unsigned, bool)> cardCompletion_;
  std::vector<std::pair<std::string, uint32_t>> directory_;
  unsigned directoryIndex_ = 0;
  cdrom::VirtualFs &fs_;
  Memory &mem_;
  OverlayManager *overlayMgr_ = nullptr;

  std::vector<FileDescriptor> fds_;
  static constexpr int32_t MAX_FDS = 16;
};

} // namespace ps1::bios
