#include "runtime/cdrom/virtual_fs.h"
#include <fmt/core.h>

namespace ps1::cdrom {

VirtualFs::VirtualFs() {}

bool VirtualFs::loadDisc(const std::filesystem::path &imagePath) {
  auto ext = imagePath.extension().string();
  if (ext == ".cue" || ext == ".CUE") {
    return loadCue(imagePath);
  } else if (ext == ".bin" || ext == ".BIN" || ext == ".iso" || ext == ".ISO") {
    binReader_ = std::make_unique<BinReader>();
    if (!binReader_->open(imagePath)) {
      return false;
    }

    isoParser_ = std::make_unique<Iso9660Parser>(*binReader_);
    if (isoParser_->initialize()) {
      fmt::print("[CDROM] Filesystem montado (BIN/ISO) a partir de {}\n",
                 imagePath.filename().string());
      exeExtractor_ = std::make_unique<ExeExtractor>(*binReader_, *isoParser_);
    }
    return true;
  }

  // Unsupported format for now (.chd, .pbp, etc.)
  return false;
}

bool VirtualFs::loadCue(const std::filesystem::path &cuePath) {
  auto parsedTracks = CueParser::parse(cuePath);
  if (!parsedTracks)
    return false;

  tracks_ = *parsedTracks;

  if (tracks_.empty())
    return false;

  fmt::print("[CDROM] {} trilha(s) detectada(s) no CUE.\n", tracks_.size());

  // For simplistic MVP, we assume the first data track contains the ISO9660
  // filesystem and is stored in a referenced BIN file in the same directory as
  // the CUE.
  auto binPath = cuePath.parent_path() / tracks_[0].file;

  binReader_ = std::make_unique<BinReader>();
  if (!binReader_->open(binPath)) {
    return false;
  }

  isoParser_ = std::make_unique<Iso9660Parser>(*binReader_);
  if (isoParser_->initialize()) {
    fmt::print(
        "[CDROM] Filesystem montado a partir da primeira trilha de dados.\n");
    exeExtractor_ = std::make_unique<ExeExtractor>(*binReader_, *isoParser_);
  }

  return true;
}

std::optional<Sector> VirtualFs::readSector(uint32_t lba) {
  if (!binReader_)
    return std::nullopt;
  return binReader_->readSector(lba);
}

std::optional<std::vector<uint8_t>>
VirtualFs::readFile(const std::string &filepath) {
  if (!isoParser_ || !binReader_)
    return std::nullopt;

  auto entryOpt = isoParser_->findFile(filepath);
  if (!entryOpt)
    return std::nullopt;

  std::vector<uint8_t> data;
  data.reserve(entryOpt->data_length);

  uint32_t sectors_to_read =
      (entryOpt->data_length + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;

  for (uint32_t i = 0; i < sectors_to_read; ++i) {
    auto sector = binReader_->readSector(entryOpt->extent_lba + i);
    if (!sector)
      return std::nullopt;

    auto sectorData = sector->getDataMode2Form1();
    data.insert(data.end(), sectorData.begin(), sectorData.end());
  }

  data.resize(entryOpt->data_length);
  return data;
}

std::optional<std::string> VirtualFs::getBootPath() {
  if (!exeExtractor_)
    return std::nullopt;
  auto bootOpt = exeExtractor_->getBootExecutablePath();
  if (bootOpt) {
    fmt::print("[CDROM] EXE encontrado em SYSTEM.CNF: {}\n", *bootOpt);
  }
  return bootOpt;
}

bool VirtualFs::swapDisc(const std::filesystem::path &newImagePath) {
  fmt::print("[CDROM] Swapping disc to: {}\n",
             newImagePath.filename().string());

  // Reset current state
  exeExtractor_.reset();
  isoParser_.reset();
  binReader_.reset();
  tracks_.clear();

  // Track the new disc
  discPaths_.push_back(newImagePath);
  currentDiscIndex_ = discPaths_.size() - 1;

  // Reload with the new image
  return loadDisc(newImagePath);
}

} // namespace ps1::cdrom
