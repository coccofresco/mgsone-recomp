#pragma once

#include "runtime/cdrom/virtual_fs.h"
#include "runtime/memory.h"
#include "runtime/overlay_manager.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps1::bios {

struct FileDescriptor {
  bool inUse;
  std::string path;
  std::vector<uint8_t> data;
  uint32_t cursor;
};

class FileIO {
public:
  FileIO(cdrom::VirtualFs &fs, Memory &mem);
  ~FileIO() = default;

  // BIOS API
  int32_t open(const char *path, int32_t mode);
  int32_t close(int32_t fd);
  int32_t read(int32_t fd, uint32_t dstAddr, int32_t length);
  int32_t lseek(int32_t fd, int32_t offset, int32_t whence);

  /// Set overlay manager for load-time overlay activation
  void setOverlayManager(OverlayManager *mgr) { overlayMgr_ = mgr; }

private:
  cdrom::VirtualFs &fs_;
  Memory &mem_;
  OverlayManager *overlayMgr_ = nullptr;

  std::vector<FileDescriptor> fds_;
  static constexpr int32_t MAX_FDS = 16;
};

} // namespace ps1::bios
