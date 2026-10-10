#include "runtime/cdrom/virtual_fs.h"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>

namespace ps1::cdrom {

VirtualFs::VirtualFs() {}

void VirtualFs::setUnpacedDataFiles(std::vector<std::string> paths) {
  unpacedDataFiles_ = std::move(paths);
  refreshUnpacedDataExtents();
}

void VirtualFs::refreshUnpacedDataExtents() {
  unpacedDataExtents_.clear();
  if (!isoParser_) return;
  for (const auto &path : unpacedDataFiles_) {
    const auto entry = isoParser_->findFile(path);
    if (!entry || entry->is_directory || entry->data_length == 0) continue;
    const uint64_t sectors =
        (uint64_t(entry->data_length) + SECTOR_SIZE_DATA - 1) / SECTOR_SIZE_DATA;
    unpacedDataExtents_.push_back({entry->extent_lba, entry->extent_lba + sectors, path});
  }
}

bool VirtualFs::allowsUnpacedData(uint32_t lba) const {
  return std::any_of(unpacedDataExtents_.begin(), unpacedDataExtents_.end(),
                    [this, lba](const auto &extent) {
                      return lba >= extent.first && lba < extent.end &&
                             (!unpacedDataGuard_ || unpacedDataGuard_(extent.path));
                    });
}

bool VirtualFs::openImage(const std::filesystem::path &imagePath) {
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
    return exeExtractor_ != nullptr;
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

  return exeExtractor_ != nullptr;
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
    for (char &c : *bootOpt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    fmt::print("[CDROM] EXE encontrado em SYSTEM.CNF: {}\n", *bootOpt);
  }
  return bootOpt;
}

void VirtualFs::adoptMedia(VirtualFs &prepared) {
  // Parsers reference the heap-owned reader, whose address stays stable.
  exeExtractor_.swap(prepared.exeExtractor_);
  isoParser_.swap(prepared.isoParser_);
  binReader_.swap(prepared.binReader_);
  tracks_.swap(prepared.tracks_);
  mediaIdentity_.swap(prepared.mediaIdentity_);
  bootName_.swap(prepared.bootName_);
  // The policy belongs to this runtime, not to the temporary prepared medium.
  refreshUnpacedDataExtents();
}

bool VirtualFs::prepareMedia(const std::filesystem::path &path) {
  if (!openImage(path)) return false;
  auto boot = getBootPath();
  auto pvd = readSector(16);
  if (!pvd) return false;
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t b : pvd->getDataMode2Form1()) hash = (hash ^ b) * 1099511628211ull;
  if (boot) {
    auto executable = readFile(*boot);
    if (!executable) return false;
    for (uint8_t b : *executable) hash = (hash ^ b) * 1099511628211ull;
  } else {
    // Generic data discs and hardware fixtures need not be bootable. Their
    // bootstrap sectors identify them; MGS registration still requires its
    // actual boot serial and cannot select this kind of medium by mistake.
    for (uint32_t lba = 0; lba < 32; ++lba) {
      auto sector = readSector(lba);
      if (!sector) break;
      for (uint8_t b : sector->raw) hash = (hash ^ b) * 1099511628211ull;
    }
  }
  bootName_ = boot.value_or("");
  mediaIdentity_ = fmt::format("{}:{:016x}", boot.value_or("ISO9660"), hash);
  return true;
}

uint32_t VirtualFs::rememberMedia(const std::filesystem::path &path,
                                  const VirtualFs &prepared) {
  const auto it = std::find(discIdentities_.begin(), discIdentities_.end(), prepared.mediaIdentity_);
  if (it != discIdentities_.end()) {
    const auto index = static_cast<uint32_t>(it - discIdentities_.begin());
    discPaths_[index] = path; // same content at a new installation path
    return index;
  }
  discPaths_.push_back(path);
  discIdentities_.push_back(prepared.mediaIdentity_);
  discBoots_.push_back(prepared.bootName_);
  return static_cast<uint32_t>(discPaths_.size() - 1);
}

bool VirtualFs::registerDisc(const std::filesystem::path &path,
                             const std::string &expectedBoot) {
  VirtualFs prepared;
  if (!prepared.prepareMedia(path) ||
      (!expectedBoot.empty() && prepared.bootName_ != expectedBoot)) return false;
  rememberMedia(path, prepared);
  return true;
}

std::string VirtualFs::identityForBoot(const std::string &boot) const {
  std::string result;
  for (size_t i = 0; i < discBoots_.size(); ++i) {
    if (discBoots_[i] != boot) continue;
    if (!result.empty() && result != discIdentities_[i]) return {}; // ambiguous release
    result = discIdentities_[i];
  }
  return result;
}

bool VirtualFs::mountIdentity(const std::string &identity) {
  if (identity == mediaIdentity_) return true;
  // Empty denotes a genuinely unmounted drive in a state, not "keep current".
  if (identity.empty()) {
    VirtualFs empty;
    adoptMedia(empty);
    return true;
  }
  auto it = std::find(discIdentities_.begin(), discIdentities_.end(), identity);
  if (it == discIdentities_.end()) return false;
  const auto index = static_cast<uint32_t>(it - discIdentities_.begin());
  VirtualFs prepared;
  // Revalidate on open: a path may have been removed/replaced since startup.
  if (!prepared.prepareMedia(discPaths_[index]) || prepared.mediaIdentity_ != identity) return false;
  adoptMedia(prepared);
  currentDiscIndex_ = index;
  fmt::print("[CDROM] Mounted medium {}\n", mediaIdentity_);
  return true;
}

bool VirtualFs::loadDisc(const std::filesystem::path &path) {
  VirtualFs prepared;
  if (!prepared.prepareMedia(path)) return false;
  currentDiscIndex_ = rememberMedia(path, prepared);
  adoptMedia(prepared);
  fmt::print("[CDROM] Mounted medium {}\n", mediaIdentity_);
  return true;
}

bool VirtualFs::swapDisc(const std::filesystem::path &path) {
  return loadDisc(path);
}

} // namespace ps1::cdrom
