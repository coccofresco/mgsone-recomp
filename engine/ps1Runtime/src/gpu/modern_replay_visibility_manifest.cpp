#include "runtime/gpu/modern_replay_visibility_manifest.h"

#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_set>

namespace ps1::gpu::replay {
namespace {

constexpr uint32_t kGuestRamAddressMask = 0x1fffffu;
constexpr uint64_t kHashOffset = 14695981039346656037ull;
constexpr uint64_t kHashPrime = 1099511628211ull;

template <typename T> void hashInteger(uint64_t &hash, T value) noexcept {
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(bits); ++index) {
    hash ^= static_cast<uint8_t>(bits & 0xffu);
    hash *= kHashPrime;
    bits >>= 8u;
  }
}

ReplayVisibilityManifestSnapshot
failure(ReplayVisibilityManifestStatus status, std::string error) {
  ReplayVisibilityManifestSnapshot result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

bool lowercaseHex(std::string_view value, std::size_t width) {
  if (value.size() != width)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

template <typename T>
bool parseUnsigned(std::string_view text, T &result, int base = 10) {
  if (text.empty())
    return false;
  T parsed{};
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto converted = std::from_chars(begin, end, parsed, base);
  if (converted.ec != std::errc{} || converted.ptr != end)
    return false;
  result = parsed;
  return true;
}

bool parseHex64(std::string_view text, uint64_t &result) {
  return lowercaseHex(text, 16) && parseUnsigned(text, result, 16);
}

bool parseHex32(std::string_view text, uint32_t &result) {
  return lowercaseHex(text, 8) && parseUnsigned(text, result, 16);
}

std::vector<std::string_view> splitWords(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t offset = 0;
  while (offset < line.size()) {
    while (offset < line.size() &&
           (line[offset] == ' ' || line[offset] == '\t' ||
            line[offset] == '\r')) {
      ++offset;
    }
    if (offset == line.size() || line[offset] == '#')
      break;
    const std::size_t begin = offset;
    while (offset < line.size() && line[offset] != ' ' &&
           line[offset] != '\t' && line[offset] != '\r' &&
           line[offset] != '#') {
      ++offset;
    }
    words.push_back(line.substr(begin, offset - begin));
    if (offset < line.size() && line[offset] == '#')
      break;
  }
  return words;
}

std::string lineError(std::size_t line, std::string_view message) {
  return "line " + std::to_string(line) + ": " + std::string(message);
}

} // namespace

std::size_t ReplayVisibilityDispatchKeyHash::operator()(
    const ReplayVisibilityDispatchKey &key) const noexcept {
  uint64_t hash = kHashOffset;
  hashInteger(hash, key.transformLineage);
  hashInteger(hash, key.triangleIndex);
  for (const ReplayVisibilityDispatchVertex &vertex : key.vertices) {
    hashInteger(hash, vertex.index);
    hashInteger(hash, static_cast<uint8_t>(vertex.modelSourcePresent));
    hashInteger(hash, vertex.modelSource);
    hashInteger(hash, vertex.modelFlowSource);
    hashInteger(hash, vertex.modelFlowProgram);
    hashInteger(hash, vertex.gp0NodeOffset);
  }
  if constexpr (sizeof(std::size_t) < sizeof(hash))
    return static_cast<std::size_t>(hash ^ (hash >> 32u));
  return static_cast<std::size_t>(hash);
}

std::optional<ReplayVisibilityDispatchKey>
makeReplayVisibilityDispatchKey(
    const ReplayVisibilityAuditOwner &owner) noexcept {
  const ReplayTriangleProvenance &triangle = owner.provenance;
  if (!owner.comparable() || !triangle.validTopology ||
      !triangle.coherentTransformLineage() ||
      !triangle.coherentDmaTraversal()) {
    return std::nullopt;
  }

  ReplayVisibilityDispatchKey key;
  key.transformLineage = triangle.vertices[0].transformLineage;
  key.triangleIndex = owner.triangleIndex;
  for (std::size_t index = 0; index < key.vertices.size(); ++index) {
    const ReplayVertexProvenance &source = triangle.vertices[index];
    constexpr uint8_t required = ReplayVertexGp0WordSourceExact |
                                 ReplayVertexDmaTraversalExact |
                                 ReplayVertexModelValueFlowExact;
    if (!source.has(required) || source.gp0WordSource == 0 ||
        source.dmaNodeSource == 0 ||
        source.modelValueFlowSourceFingerprint == 0 ||
        source.modelValueFlowProgramFingerprint == 0) {
      return std::nullopt;
    }
    const bool modelSourcePresent =
        source.has(ReplayVertexModelSourceExact);
    if (modelSourcePresent != (source.modelVertexSource != 0))
      return std::nullopt;
    if (triangle.vertexIndices[index] > 3u)
      return std::nullopt;

    const uint32_t wordAddress = static_cast<uint32_t>(
        (source.gp0WordSource - 1u) & kGuestRamAddressMask);
    const uint32_t nodeAddress = static_cast<uint32_t>(
        (source.dmaNodeSource - 1u) & kGuestRamAddressMask);
    ReplayVisibilityDispatchVertex &destination = key.vertices[index];
    destination.index = triangle.vertexIndices[index];
    destination.modelSourcePresent = modelSourcePresent;
    destination.modelSource = modelSourcePresent ? source.modelVertexSource : 0;
    destination.modelFlowSource =
        source.modelValueFlowSourceFingerprint;
    destination.modelFlowProgram =
        source.modelValueFlowProgramFingerprint;
    destination.gp0NodeOffset =
        (wordAddress - nodeAddress) & kGuestRamAddressMask;
  }
  if (key.vertices[0].index == key.vertices[1].index ||
      key.vertices[0].index == key.vertices[2].index ||
      key.vertices[1].index == key.vertices[2].index) {
    return std::nullopt;
  }
  return key;
}

const char *replayVisibilityManifestStatusName(
    ReplayVisibilityManifestStatus status) {
  switch (status) {
  case ReplayVisibilityManifestStatus::Empty:
    return "empty";
  case ReplayVisibilityManifestStatus::Ready:
    return "ready";
  case ReplayVisibilityManifestStatus::IoError:
    return "io-error";
  case ReplayVisibilityManifestStatus::InvalidSyntax:
    return "invalid-syntax";
  case ReplayVisibilityManifestStatus::InvalidSchema:
    return "invalid-schema";
  case ReplayVisibilityManifestStatus::InvalidFingerprintSchema:
    return "invalid-fingerprint-schema";
  case ReplayVisibilityManifestStatus::PriorContaminated:
    return "prior-contaminated";
  case ReplayVisibilityManifestStatus::InvalidComponent:
    return "invalid-component";
  case ReplayVisibilityManifestStatus::InvalidEntry:
    return "invalid-entry";
  case ReplayVisibilityManifestStatus::DuplicateComponent:
    return "duplicate-component";
  case ReplayVisibilityManifestStatus::DuplicateEntry:
    return "duplicate-entry";
  case ReplayVisibilityManifestStatus::UnknownComponent:
    return "unknown-component";
  case ReplayVisibilityManifestStatus::CountMismatch:
    return "count-mismatch";
  case ReplayVisibilityManifestStatus::InvalidConstructionSeed:
    return "invalid-construction-seed";
  case ReplayVisibilityManifestStatus::DuplicateConstructionSeed:
    return "duplicate-construction-seed";
  }
  return "unknown";
}

ReplayVisibilityManifestSnapshot
parseReplayVisibilityManifest(std::string_view text) {
  ReplayVisibilityManifestSnapshot result;
  bool sawSchema = false;
  bool sawFingerprintSchema = false;
  bool sawScope = false;
  bool sawFamilyPrior = false;
  bool sawTenthsPrior = false;
  bool sawConstructionSeedCount = false;
  bool sawEnd = false;
  std::size_t footerComponents = 0;
  std::size_t footerEntries = 0;
  std::size_t expectedConstructionSeeds = 0;
  std::unordered_map<std::string, std::size_t> componentIndices;
  std::unordered_set<std::size_t> referencedComponents;
  std::set<std::tuple<uint32_t, uint32_t, uint8_t, uint32_t>>
      constructionSeedKeys;

  std::size_t lineNumber = 0;
  for (std::size_t begin = 0; begin <= text.size();) {
    const std::size_t end = text.find('\n', begin);
    const std::string_view line = text.substr(
        begin, end == std::string_view::npos ? text.size() - begin
                                             : end - begin);
    ++lineNumber;
    const std::vector<std::string_view> words = splitWords(line);
    if (!words.empty()) {
      if (sawEnd) {
        return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                       lineError(lineNumber, "content follows end record"));
      }
      const std::string_view directive = words[0];
      if (directive == "schema") {
        if (sawSchema || words.size() != 2)
          return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                         lineError(lineNumber, "invalid schema record"));
        sawSchema = true;
        if (words[1] != REPLAY_VISIBILITY_MANIFEST_SCHEMA)
          return failure(ReplayVisibilityManifestStatus::InvalidSchema,
                         lineError(lineNumber, "unsupported manifest schema"));
      } else if (directive == "fingerprint_schema") {
        uint32_t schema = 0;
        if (sawFingerprintSchema || words.size() != 2 ||
            !parseUnsigned(words[1], schema)) {
          return failure(
              ReplayVisibilityManifestStatus::InvalidFingerprintSchema,
              lineError(lineNumber, "invalid fingerprint schema record"));
        }
        sawFingerprintSchema = true;
        if (schema != 1u)
          return failure(
              ReplayVisibilityManifestStatus::InvalidFingerprintSchema,
              lineError(lineNumber, "unsupported fingerprint schema"));
      } else if (directive == "scope") {
        if (sawScope || words.size() != 2 || words[1] != "hypothesis")
          return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                         lineError(lineNumber, "scope must be hypothesis"));
        sawScope = true;
        result.hypothesis = true;
      } else if (directive == "family_count_prior_used" ||
                 directive == "integer_or_tenth_prior_used") {
        uint32_t value = 0;
        bool &seen = directive == "family_count_prior_used" ? sawFamilyPrior
                                                             : sawTenthsPrior;
        if (seen || words.size() != 2 || !parseUnsigned(words[1], value) ||
            value > 1u) {
          return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                         lineError(lineNumber, "invalid prior record"));
        }
        seen = true;
        if (directive == "family_count_prior_used")
          result.familyCountPriorUsed = value != 0;
        else
          result.integerOrTenthPriorUsed = value != 0;
        if (value != 0)
          return failure(ReplayVisibilityManifestStatus::PriorContaminated,
                         lineError(lineNumber, "candidate used an advisory prior"));
      } else if (directive == "component") {
        if (words.size() != 4 || !lowercaseHex(words[1], 64))
          return failure(ReplayVisibilityManifestStatus::InvalidComponent,
                         lineError(lineNumber, "invalid component record"));
        uint64_t numerator = 0;
        uint64_t denominator = 0;
        if (!parseUnsigned(words[2], numerator) ||
            !parseUnsigned(words[3], denominator) || numerator == 0 ||
            denominator == 0 || std::gcd(numerator, denominator) != 1u) {
          return failure(
              ReplayVisibilityManifestStatus::InvalidComponent,
              lineError(lineNumber,
                        "component multiplier must be a reduced positive rational"));
        }
        const double multiplier = static_cast<double>(numerator) /
                                  static_cast<double>(denominator);
        if (!std::isfinite(multiplier) || multiplier <= 0.0 ||
            multiplier > std::numeric_limits<float>::max()) {
          return failure(
              ReplayVisibilityManifestStatus::InvalidComponent,
              lineError(lineNumber, "component multiplier is not finite float"));
        }
        const std::string sha(words[1]);
        if (componentIndices.contains(sha))
          return failure(ReplayVisibilityManifestStatus::DuplicateComponent,
                         lineError(lineNumber, "duplicate component"));
        const std::size_t index = result.components.size();
        componentIndices.emplace(sha, index);
        result.components.push_back({sha, {numerator, denominator}});
      } else if (directive == "construction_seeds") {
        if (sawConstructionSeedCount || words.size() != 2 ||
            !parseUnsigned(words[1], expectedConstructionSeeds)) {
          return failure(
              ReplayVisibilityManifestStatus::InvalidConstructionSeed,
              lineError(lineNumber, "invalid construction seed count"));
        }
        sawConstructionSeedCount = true;
      } else if (directive == "construction_seed") {
        ReplayVisibilityConstructionSeed seed;
        uint32_t sourceRegister = 0;
        if (words.size() != 5 || !parseHex32(words[1], seed.pc) ||
            !parseHex32(words[2], seed.rawInstruction) ||
            !parseUnsigned(words[3], sourceRegister) || sourceRegister == 0 ||
            sourceRegister >= 32 || !parseHex32(words[4], seed.sourceAddress) ||
            seed.pc == 0 || (seed.pc & 3u) != 0 ||
            seed.sourceAddress == 0 || seed.sourceAddress > 0x1fffffffu) {
          return failure(
              ReplayVisibilityManifestStatus::InvalidConstructionSeed,
              lineError(lineNumber, "invalid construction seed record"));
        }
        seed.sourceRegister = static_cast<uint8_t>(sourceRegister);
        const auto key = std::tuple(seed.pc, seed.rawInstruction,
                                    seed.sourceRegister, seed.sourceAddress);
        if (!constructionSeedKeys.insert(key).second) {
          return failure(
              ReplayVisibilityManifestStatus::DuplicateConstructionSeed,
              lineError(lineNumber, "duplicate construction seed"));
        }
        result.constructionSeeds.push_back(seed);
      } else if (directive == "entry") {
        if (words.size() != 22 || !lowercaseHex(words[1], 64))
          return failure(ReplayVisibilityManifestStatus::InvalidEntry,
                         lineError(lineNumber, "invalid entry record"));
        const auto component = componentIndices.find(std::string(words[1]));
        if (component == componentIndices.end())
          return failure(ReplayVisibilityManifestStatus::UnknownComponent,
                         lineError(lineNumber, "entry references unknown component"));
        ReplayVisibilityDispatchKey key;
        uint32_t triangleIndex = 0;
        if (!parseHex64(words[2], key.transformLineage) ||
            !parseUnsigned(words[3], triangleIndex) || triangleIndex > 255u) {
          return failure(ReplayVisibilityManifestStatus::InvalidEntry,
                         lineError(lineNumber, "invalid entry header"));
        }
        key.triangleIndex = static_cast<uint8_t>(triangleIndex);
        std::size_t cursor = 4;
        for (ReplayVisibilityDispatchVertex &vertex : key.vertices) {
          uint32_t vertexIndex = 0;
          uint32_t modelSourcePresent = 0;
          if (!parseUnsigned(words[cursor++], vertexIndex) ||
              vertexIndex > 3u ||
              !parseUnsigned(words[cursor++], modelSourcePresent) ||
              modelSourcePresent > 1u ||
              !parseHex64(words[cursor++], vertex.modelSource) ||
              !parseHex64(words[cursor++], vertex.modelFlowSource) ||
              !parseHex64(words[cursor++], vertex.modelFlowProgram) ||
              !parseUnsigned(words[cursor++], vertex.gp0NodeOffset) ||
              vertex.gp0NodeOffset > kGuestRamAddressMask ||
              vertex.modelFlowSource == 0 || vertex.modelFlowProgram == 0 ||
              (modelSourcePresent == 0 && vertex.modelSource != 0) ||
              (modelSourcePresent != 0 && vertex.modelSource == 0)) {
            return failure(ReplayVisibilityManifestStatus::InvalidEntry,
                           lineError(lineNumber, "invalid entry vertex"));
          }
          vertex.index = static_cast<uint8_t>(vertexIndex);
          vertex.modelSourcePresent = modelSourcePresent != 0;
        }
        if (key.vertices[0].index == key.vertices[1].index ||
            key.vertices[0].index == key.vertices[2].index ||
            key.vertices[1].index == key.vertices[2].index) {
          return failure(ReplayVisibilityManifestStatus::InvalidEntry,
                         lineError(lineNumber, "entry vertex indices repeat"));
        }
        if (result.dispatch.contains(key))
          return failure(ReplayVisibilityManifestStatus::DuplicateEntry,
                         lineError(lineNumber, "duplicate dispatch key"));
        const std::size_t componentIndex = component->second;
        result.dispatch.emplace(key, componentIndex);
        result.entries.push_back({key, componentIndex});
        referencedComponents.insert(componentIndex);
      } else if (directive == "end") {
        if (words.size() != 3 ||
            !parseUnsigned(words[1], footerComponents) ||
            !parseUnsigned(words[2], footerEntries)) {
          return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                         lineError(lineNumber, "invalid end record"));
        }
        sawEnd = true;
      } else {
        return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                       lineError(lineNumber, "unknown directive"));
      }
    }
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }

  if (!sawSchema || !sawFingerprintSchema || !sawScope || !sawFamilyPrior ||
      !sawTenthsPrior || !sawEnd) {
    return failure(ReplayVisibilityManifestStatus::InvalidSyntax,
                   "manifest is missing a required header or footer record");
  }
  if (result.components.empty() || result.entries.empty()) {
    return failure(ReplayVisibilityManifestStatus::CountMismatch,
                   "manifest must contain components and dispatch entries");
  }
  if (footerComponents != result.components.size() ||
      footerEntries != result.entries.size() ||
      referencedComponents.size() != result.components.size()) {
    return failure(ReplayVisibilityManifestStatus::CountMismatch,
                   "footer counts or component coverage do not match contents");
  }
  if ((!result.constructionSeeds.empty() && !sawConstructionSeedCount) ||
      (sawConstructionSeedCount &&
       expectedConstructionSeeds != result.constructionSeeds.size())) {
    return failure(ReplayVisibilityManifestStatus::CountMismatch,
                   "construction seed count does not match contents");
  }
  result.status = ReplayVisibilityManifestStatus::Ready;
  return result;
}

ReplayVisibilityManifestFile::ReplayVisibilityManifestFile(
    std::filesystem::path path)
    : path_(std::move(path)) {}

void ReplayVisibilityManifestFile::setPath(std::filesystem::path path) {
  path_ = std::move(path);
  snapshot_ = {};
  lastWriteTime_ = {};
  lastFileSize_ = 0;
  generation_ = 0;
  observed_ = false;
}

bool ReplayVisibilityManifestFile::reload() {
  ++generation_;
  std::error_code error;
  const auto writeTime = std::filesystem::last_write_time(path_, error);
  if (error) {
    snapshot_ = failure(ReplayVisibilityManifestStatus::IoError,
                        "cannot stat manifest: " + error.message());
    observed_ = true;
    lastWriteTime_ = {};
    lastFileSize_ = 0;
    return false;
  }
  const std::uintmax_t fileSize = std::filesystem::file_size(path_, error);
  if (error) {
    snapshot_ = failure(ReplayVisibilityManifestStatus::IoError,
                        "cannot size manifest: " + error.message());
    observed_ = true;
    lastWriteTime_ = writeTime;
    lastFileSize_ = 0;
    return false;
  }
  std::ifstream stream(path_, std::ios::binary);
  if (!stream) {
    snapshot_ = failure(ReplayVisibilityManifestStatus::IoError,
                        "cannot open manifest");
    observed_ = true;
    lastWriteTime_ = writeTime;
    lastFileSize_ = fileSize;
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (!stream.good() && !stream.eof()) {
    snapshot_ = failure(ReplayVisibilityManifestStatus::IoError,
                        "cannot read manifest");
  } else {
    snapshot_ = parseReplayVisibilityManifest(buffer.str());
  }
  observed_ = true;
  lastWriteTime_ = writeTime;
  lastFileSize_ = fileSize;
  return snapshot_.ready();
}

bool ReplayVisibilityManifestFile::reloadIfChanged() {
  std::error_code error;
  const auto writeTime = std::filesystem::last_write_time(path_, error);
  if (error)
    return observed_ && snapshot_.status == ReplayVisibilityManifestStatus::IoError
               ? false
               : reload();
  const std::uintmax_t fileSize = std::filesystem::file_size(path_, error);
  if (error)
    return observed_ && snapshot_.status == ReplayVisibilityManifestStatus::IoError
               ? false
               : reload();
  if (!observed_ || writeTime != lastWriteTime_ || fileSize != lastFileSize_)
    return reload();
  return false;
}

std::optional<ReplayVisibilityCandidateLookup>
ReplayVisibilityManifestFile::lookup(
    const ReplayVisibilityAuditOwner &owner) const noexcept {
  if (!snapshot_.ready())
    return std::nullopt;
  const auto key = makeReplayVisibilityDispatchKey(owner);
  if (!key.has_value())
    return std::nullopt;
  const auto found = snapshot_.dispatch.find(*key);
  if (found == snapshot_.dispatch.end() ||
      found->second >= snapshot_.components.size()) {
    return std::nullopt;
  }
  const ReplayVisibilityManifestComponent &component =
      snapshot_.components[found->second];
  const double multiplier =
      static_cast<double>(component.presentationMultiplier.numerator) /
      static_cast<double>(component.presentationMultiplier.denominator);
  if (!std::isfinite(multiplier) || multiplier <= 0.0 ||
      multiplier > std::numeric_limits<float>::max()) {
    return std::nullopt;
  }
  return ReplayVisibilityCandidateLookup{
      component.presentationMultiplier, static_cast<float>(multiplier),
      component.sha256, snapshot_.hypothesis};
}

ReplayVisibilityPresentationDecision planReplayVisibilityPresentation(
    const ReplayVisibilityManifestFile &manifest,
    const ReplayVisibilityAuditOwner &owner,
    std::span<const vulkan_abi::Vertex> vertices) noexcept {
  if (!owner.comparable()) {
    return {ReplayVisibilityPresentationStatus::IneligibleOwner, 1.0f, {}};
  }
  if (vertices.size() != 3u) {
    return {ReplayVisibilityPresentationStatus::InvalidVertexSpan, 1.0f, {}};
  }

  const std::optional<ReplayVisibilityCandidateLookup> candidate =
      manifest.lookup(owner);
  if (!candidate.has_value()) {
    return {ReplayVisibilityPresentationStatus::UnmappedOwner, 1.0f, {}};
  }
  if (!std::isfinite(candidate->shaderMultiplier) ||
      candidate->shaderMultiplier <= 0.0f) {
    return {ReplayVisibilityPresentationStatus::InvalidMultiplier, 1.0f,
            candidate->componentSha256};
  }

  for (const vulkan_abi::Vertex &vertex : vertices) {
    const double corrected =
        static_cast<double>(vertex.projectiveW) *
        static_cast<double>(candidate->shaderMultiplier);
    if (!vulkan_abi::isProjectiveWInDepthDomain(corrected)) {
      return {ReplayVisibilityPresentationStatus::ProjectiveWOutOfDomain,
              1.0f, candidate->componentSha256};
    }
  }
  return {ReplayVisibilityPresentationStatus::Apply,
          candidate->shaderMultiplier, candidate->componentSha256};
}

} // namespace ps1::gpu::replay
