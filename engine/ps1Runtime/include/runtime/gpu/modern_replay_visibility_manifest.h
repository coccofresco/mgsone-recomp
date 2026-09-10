#pragma once

#include "runtime/gpu/modern_replay_depth_family.h"
#include "runtime/gpu/modern_replay_visibility_audit.h"
#include "runtime/gpu/modern_replay_vulkan_abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps1::gpu::replay {

inline constexpr std::string_view REPLAY_VISIBILITY_MANIFEST_SCHEMA =
    "ps1-visibility-runtime-manifest/v1";

struct ReplayVisibilityDispatchVertex {
  uint64_t modelSource = 0;
  uint64_t modelFlowSource = 0;
  uint64_t modelFlowProgram = 0;
  uint32_t gp0NodeOffset = 0;
  uint8_t index = 0;
  bool modelSourcePresent = false;

  friend bool operator==(const ReplayVisibilityDispatchVertex &,
                         const ReplayVisibilityDispatchVertex &) = default;
};

// Runtime counterpart of visibility_lineage_trace.py's v2 dispatch tuple.
// Numeric transform values, screen coordinates, colour and observed depth are
// deliberately absent. The construction fingerprints are categorical and
// versioned; they never become object identity outside this manifest.
struct ReplayVisibilityDispatchKey {
  uint64_t transformLineage = 0;
  std::array<ReplayVisibilityDispatchVertex, 3> vertices{};
  uint8_t triangleIndex = 0;

  friend bool operator==(const ReplayVisibilityDispatchKey &,
                         const ReplayVisibilityDispatchKey &) = default;
};

struct ReplayVisibilityDispatchKeyHash {
  std::size_t operator()(const ReplayVisibilityDispatchKey &key) const noexcept;
};

std::optional<ReplayVisibilityDispatchKey>
makeReplayVisibilityDispatchKey(
    const ReplayVisibilityAuditOwner &owner) noexcept;

enum class ReplayVisibilityManifestStatus : uint8_t {
  Empty = 0,
  Ready,
  IoError,
  InvalidSyntax,
  InvalidSchema,
  InvalidFingerprintSchema,
  PriorContaminated,
  InvalidComponent,
  InvalidEntry,
  DuplicateComponent,
  DuplicateEntry,
  UnknownComponent,
  CountMismatch,
  InvalidConstructionSeed,
  DuplicateConstructionSeed,
};

const char *
replayVisibilityManifestStatusName(ReplayVisibilityManifestStatus status);

struct ReplayVisibilityManifestComponent {
  std::string sha256;
  ReplayDepthGauge presentationMultiplier{};
};

struct ReplayVisibilityManifestEntry {
  ReplayVisibilityDispatchKey key{};
  std::size_t componentIndex = 0;
};

struct ReplayVisibilityConstructionSeed {
  uint32_t pc = 0;
  uint32_t rawInstruction = 0;
  uint32_t sourceAddress = 0;
  uint8_t sourceRegister = 0;

  friend bool operator==(const ReplayVisibilityConstructionSeed &,
                         const ReplayVisibilityConstructionSeed &) = default;
};

struct ReplayVisibilityManifestSnapshot {
  ReplayVisibilityManifestStatus status = ReplayVisibilityManifestStatus::Empty;
  std::vector<ReplayVisibilityManifestComponent> components;
  std::vector<ReplayVisibilityManifestEntry> entries;
  std::vector<ReplayVisibilityConstructionSeed> constructionSeeds;
  std::unordered_map<ReplayVisibilityDispatchKey, std::size_t,
                     ReplayVisibilityDispatchKeyHash>
      dispatch;
  std::string error;
  bool hypothesis = false;
  bool familyCountPriorUsed = false;
  bool integerOrTenthPriorUsed = false;

  bool ready() const noexcept {
    return status == ReplayVisibilityManifestStatus::Ready;
  }
};

struct ReplayVisibilityCandidateLookup {
  ReplayDepthGauge presentationMultiplier{};
  float shaderMultiplier = 0.0f;
  std::string_view componentSha256;
  bool hypothesis = false;
};

// A presentation manifest is deliberately advisory. Only an exact dispatch
// hit whose three submitted W values remain inside the shared D32 domain may
// alter the live replay; every other status means "keep the existing path".
enum class ReplayVisibilityPresentationStatus : uint8_t {
  Apply = 0,
  IneligibleOwner,
  UnmappedOwner,
  InvalidVertexSpan,
  InvalidMultiplier,
  ProjectiveWOutOfDomain,
};

struct ReplayVisibilityPresentationDecision {
  ReplayVisibilityPresentationStatus status =
      ReplayVisibilityPresentationStatus::IneligibleOwner;
  float depthMultiplier = 1.0f;
  std::string_view componentSha256;

  bool applies() const noexcept {
    return status == ReplayVisibilityPresentationStatus::Apply;
  }
};

ReplayVisibilityManifestSnapshot
parseReplayVisibilityManifest(std::string_view text);

class ReplayVisibilityManifestFile {
public:
  explicit ReplayVisibilityManifestFile(std::filesystem::path path = {});

  void setPath(std::filesystem::path path);
  bool reload();
  bool reloadIfChanged();

  const ReplayVisibilityManifestSnapshot &snapshot() const noexcept {
    return snapshot_;
  }
  uint64_t generation() const noexcept { return generation_; }

  std::optional<ReplayVisibilityCandidateLookup>
  lookup(const ReplayVisibilityAuditOwner &owner) const noexcept;

private:
  std::filesystem::path path_;
  ReplayVisibilityManifestSnapshot snapshot_{};
  std::filesystem::file_time_type lastWriteTime_{};
  std::uintmax_t lastFileSize_ = 0;
  uint64_t generation_ = 0;
  bool observed_ = false;
};

ReplayVisibilityPresentationDecision planReplayVisibilityPresentation(
    const ReplayVisibilityManifestFile &manifest,
    const ReplayVisibilityAuditOwner &owner,
    std::span<const vulkan_abi::Vertex> vertices) noexcept;

} // namespace ps1::gpu::replay
