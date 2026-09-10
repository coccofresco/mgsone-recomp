#include "runtime/bios/file_io.h"
#include <cstdint>
#include <fmt/core.h>
#include <string>

namespace ps1::bios {

FileIO::FileIO(cdrom::VirtualFs &fs, Memory &mem) : fs_(fs), mem_(mem) {
  fds_.resize(MAX_FDS);
  for (auto &fd : fds_) {
    fd.inUse = false;
    fd.cursor = 0;
  }
}

int32_t FileIO::open(const char *path, int32_t mode) {
  std::string pathStr(path);
  fmt::print("[BIOS] open('{}', {})\n", pathStr, mode);

  int32_t fdIdx = -1;
  for (int32_t i = 0; i < MAX_FDS; i++) {
    if (!fds_[i].inUse) {
      fdIdx = i;
      break;
    }
  }

  if (fdIdx == -1) {
    fmt::print("[BIOS] open failed: Too many open files\n");
    return -1;
  }

  // In actual PS1, "cdrom:" specifies the device.
  // Usually paths look like "cdrom:\\SYSTEM.CNF;1"
  std::string fsPath = pathStr;

  // Rudimentary stripping of cdrom prefix and version endings
  if (fsPath.find("cdrom:") == 0) {
    fsPath = fsPath.substr(6);
  }

  // Remove leading slashes/backslashes
  while (!fsPath.empty() && (fsPath[0] == '\\' || fsPath[0] == '/')) {
    fsPath = fsPath.substr(1);
  }

  // Remove version suffix like ";1"
  size_t semPos = fsPath.find(';');
  if (semPos != std::string::npos) {
    fsPath = fsPath.substr(0, semPos);
  }

  // Read the file from the CD-ROM filesystem
  auto fileData = fs_.readFile(fsPath);
  if (!fileData) {
    fmt::print("[BIOS] open failed: File not found '{}'\n", fsPath);
    return -1;
  }

  fds_[fdIdx].inUse = true;
  fds_[fdIdx].path = fsPath;
  fds_[fdIdx].data = std::move(*fileData);
  fds_[fdIdx].cursor = 0;

  return fdIdx; // Small numbers returned to the game
}

int32_t FileIO::close(int32_t fd) {
  fmt::print("[BIOS] close({})\n", fd);
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse)
    return -1;

  fds_[fd].inUse = false;
  fds_[fd].data.clear();
  fds_[fd].path.clear();
  fds_[fd].cursor = 0;

  return fd;
}

int32_t FileIO::read(int32_t fd, uint32_t dstAddr, int32_t length) {
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse)
    return -1;

  auto &fDesc = fds_[fd];
  int32_t bytesToRead = length;
  int32_t available = (int32_t)fDesc.data.size() - fDesc.cursor;

  if (bytesToRead > available) {
    bytesToRead = available;
  }

  if (bytesToRead <= 0)
    return 0; // EOF

  // Copy to emulated memory
  for (int32_t i = 0; i < bytesToRead; i++) {
    mem_.write8(dstAddr + i, fDesc.data[fDesc.cursor + i]);
  }

  fDesc.cursor += bytesToRead;

  fmt::print("[BIOS] read(fd: {}, dst: 0x{:08X}, len: {}) -> read {}\n", fd,
             dstAddr, length, bytesToRead);

  // Notify overlay manager that data was written to RAM
  if (overlayMgr_) {
    overlayMgr_->notifyMemWrite(dstAddr, static_cast<uint32_t>(bytesToRead));
  }

  return bytesToRead;
}

int32_t FileIO::lseek(int32_t fd, int32_t offset, int32_t whence) {
  if (fd < 0 || fd >= MAX_FDS || !fds_[fd].inUse)
    return -1;

  auto &fDesc = fds_[fd];
  int32_t newCursor = fDesc.cursor;

  switch (whence) {
  case 0: // SEEK_SET
    newCursor = offset;
    break;
  case 1: // SEEK_CUR
    newCursor += offset;
    break;
  case 2: // SEEK_END
    newCursor = fDesc.data.size() + offset;
    break;
  default:
    return -1;
  }

  if (newCursor < 0)
    newCursor = 0;
  if (newCursor > (int32_t)fDesc.data.size())
    newCursor = fDesc.data.size();

  fDesc.cursor = newCursor;

  fmt::print("[BIOS] lseek(fd: {}, off: {}, whence: {}) -> pos {}\n", fd,
             offset, whence, newCursor);

  return newCursor;
}

} // namespace ps1::bios
