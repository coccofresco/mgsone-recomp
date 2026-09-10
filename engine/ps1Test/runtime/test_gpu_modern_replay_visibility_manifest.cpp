#include "runtime/gpu/modern_replay_visibility_manifest.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ps1::gpu::replay;

namespace {

constexpr std::string_view componentA =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

ReplayVisibilityAuditOwner dispatchOwner(uint64_t transformLineage = 0x44u) {
  ReplayVisibilityAuditOwner owner;
  owner.triangleIndex = 1;
  owner.route = PacketRoute::Projected3D;
  owner.opaque = true;
  owner.fullyPrecise = true;
  owner.authenticDepth = true;
  owner.provenance.validTopology = true;
  owner.provenance.vertexIndices = {0, 2, 3};
  for (std::size_t index = 0; index < 3; ++index) {
    ReplayVertexProvenance &vertex = owner.provenance.vertices[index];
    vertex.transformLineage = transformLineage;
    vertex.modelVertexSource = index == 1 ? 0 : 0x101u + index;
    vertex.modelValueFlowSourceFingerprint = 0x201u + index;
    vertex.modelValueFlowProgramFingerprint = 0x301u + index;
    vertex.gp0WordSource = 0x1001u + index * 4u;
    vertex.dmaListSource = 0x801u;
    vertex.dmaNodeSource = 0x1001u;
    vertex.dmaTraversalOrdinal = 9;
    vertex.evidence = ReplayVertexTransformLineageExact |
                      ReplayVertexGp0WordSourceExact |
                      ReplayVertexDmaTraversalExact |
                      ReplayVertexModelValueFlowExact;
    if (vertex.modelVertexSource != 0)
      vertex.evidence |= ReplayVertexModelSourceExact;
  }
  return owner;
}

std::string hex64(uint64_t value) {
  std::ostringstream result;
  result << std::hex << std::setfill('0') << std::setw(16) << value;
  return result.str();
}

std::string componentSha(std::size_t index) {
  std::ostringstream result;
  result << std::hex << std::setfill('0') << std::setw(64) << index + 1u;
  return result.str();
}

std::string entry(std::string_view component,
                  const ReplayVisibilityAuditOwner &owner) {
  const auto key = makeReplayVisibilityDispatchKey(owner);
  EXPECT_TRUE(key.has_value());
  if (!key.has_value())
    return {};
  std::ostringstream result;
  result << "entry " << component << ' ' << hex64(key->transformLineage) << ' '
         << static_cast<uint32_t>(key->triangleIndex);
  for (const ReplayVisibilityDispatchVertex &vertex : key->vertices) {
    result << ' ' << static_cast<uint32_t>(vertex.index) << ' '
           << static_cast<uint32_t>(vertex.modelSourcePresent) << ' '
           << hex64(vertex.modelSource) << ' ' << hex64(vertex.modelFlowSource)
           << ' ' << hex64(vertex.modelFlowProgram) << ' '
           << vertex.gp0NodeOffset;
  }
  result << '\n';
  return result.str();
}

std::string manifest(uint64_t numerator = 31, uint64_t denominator = 1) {
  return "schema ps1-visibility-runtime-manifest/v1\n"
         "fingerprint_schema 1\n"
         "scope hypothesis\n"
         "family_count_prior_used 0\n"
         "integer_or_tenth_prior_used 0\n"
         "component " +
         std::string(componentA) + " " + std::to_string(numerator) + " " +
         std::to_string(denominator) + "\n" + entry(componentA, dispatchOwner()) +
         "end 1 1\n";
}

class GpuModernReplayVisibilityManifestFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    path = std::filesystem::temp_directory_path() /
           ("ps1_visibility_manifest_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".txt");
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  void write(std::string_view value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.good());
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    ASSERT_TRUE(stream.good());
  }

  std::filesystem::path path;
};

} // namespace

TEST(GpuModernReplayVisibilityManifestTest,
     BuildsTheExactValueIndependentRuntimeKey) {
  const ReplayVisibilityAuditOwner owner = dispatchOwner();
  const auto key = makeReplayVisibilityDispatchKey(owner);

  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->transformLineage, 0x44u);
  EXPECT_EQ(key->triangleIndex, 1u);
  EXPECT_EQ(key->vertices[0].index, 0u);
  EXPECT_TRUE(key->vertices[0].modelSourcePresent);
  EXPECT_EQ(key->vertices[0].gp0NodeOffset, 0u);
  EXPECT_FALSE(key->vertices[1].modelSourcePresent);
  EXPECT_EQ(key->vertices[1].modelSource, 0u);
  EXPECT_EQ(key->vertices[1].gp0NodeOffset, 4u);
  EXPECT_EQ(key->vertices[2].gp0NodeOffset, 8u);

  ReplayVisibilityAuditOwner incomplete = owner;
  incomplete.provenance.vertices[2].evidence &=
      ~ReplayVertexModelValueFlowExact;
  EXPECT_FALSE(makeReplayVisibilityDispatchKey(incomplete).has_value());

  ReplayVisibilityAuditOwner differentFlow = owner;
  ++differentFlow.provenance.vertices[2].modelValueFlowProgramFingerprint;
  EXPECT_NE(*makeReplayVisibilityDispatchKey(owner),
            *makeReplayVisibilityDispatchKey(differentFlow));
}

TEST(GpuModernReplayVisibilityManifestTest,
     ParsesAndLooksUpAnExplicitHypothesis) {
  const ReplayVisibilityManifestSnapshot parsed =
      parseReplayVisibilityManifest(manifest(31, 2));

  ASSERT_EQ(parsed.status, ReplayVisibilityManifestStatus::Ready)
      << parsed.error;
  ASSERT_EQ(parsed.components.size(), 1u);
  ASSERT_EQ(parsed.entries.size(), 1u);
  EXPECT_TRUE(parsed.hypothesis);
  EXPECT_FALSE(parsed.familyCountPriorUsed);
  EXPECT_FALSE(parsed.integerOrTenthPriorUsed);
  EXPECT_EQ(parsed.components[0].presentationMultiplier,
            (ReplayDepthGauge{31, 2}));
}

TEST(GpuModernReplayVisibilityManifestTest,
     ParsesExactConstructionAdmissionSeeds) {
  std::string seeded = manifest();
  const std::size_t component = seeded.find("component ");
  ASSERT_NE(component, std::string::npos);
  seeded.insert(component,
                "construction_seeds 2\n"
                "construction_seed 8003f814 3c091fe0 20 000c2f90\n"
                "construction_seed 8003f814 3c091fe0 20 0012c114\n");

  const ReplayVisibilityManifestSnapshot parsed =
      parseReplayVisibilityManifest(seeded);
  ASSERT_EQ(parsed.status, ReplayVisibilityManifestStatus::Ready)
      << parsed.error;
  ASSERT_EQ(parsed.constructionSeeds.size(), 2u);
  EXPECT_EQ(parsed.constructionSeeds[0].pc, 0x8003f814u);
  EXPECT_EQ(parsed.constructionSeeds[0].rawInstruction, 0x3c091fe0u);
  EXPECT_EQ(parsed.constructionSeeds[0].sourceRegister, 20u);
  EXPECT_EQ(parsed.constructionSeeds[0].sourceAddress, 0x000c2f90u);

  std::string badCount = seeded;
  badCount.replace(badCount.find("construction_seeds 2"),
                   std::string("construction_seeds 2").size(),
                   "construction_seeds 1");
  EXPECT_EQ(parseReplayVisibilityManifest(badCount).status,
            ReplayVisibilityManifestStatus::CountMismatch);

  std::string duplicate = seeded;
  const std::string duplicateLine =
      "construction_seed 8003f814 3c091fe0 20 000c2f90\n";
  duplicate.insert(duplicate.find("component "), duplicateLine);
  duplicate.replace(duplicate.find("construction_seeds 2"),
                    std::string("construction_seeds 2").size(),
                    "construction_seeds 3");
  EXPECT_EQ(parseReplayVisibilityManifest(duplicate).status,
            ReplayVisibilityManifestStatus::DuplicateConstructionSeed);
}

TEST(GpuModernReplayVisibilityManifestTest,
     RejectsContaminatedMalformedOrIncompleteManifests) {
  std::string contaminated = manifest();
  contaminated.replace(contaminated.find("family_count_prior_used 0"),
                       std::string("family_count_prior_used 0").size(),
                       "family_count_prior_used 1");
  EXPECT_EQ(parseReplayVisibilityManifest(contaminated).status,
            ReplayVisibilityManifestStatus::PriorContaminated);

  std::string unreduced = manifest(2, 2);
  EXPECT_EQ(parseReplayVisibilityManifest(unreduced).status,
            ReplayVisibilityManifestStatus::InvalidComponent);

  std::string unknown = manifest();
  unknown.replace(unknown.find(componentA, unknown.find("entry")), 64,
                  componentSha(8));
  EXPECT_EQ(parseReplayVisibilityManifest(unknown).status,
            ReplayVisibilityManifestStatus::UnknownComponent);

  std::string duplicate = manifest();
  duplicate.insert(duplicate.find("end"), entry(componentA, dispatchOwner()));
  duplicate.replace(duplicate.find("end 1 1"), 7, "end 1 2");
  EXPECT_EQ(parseReplayVisibilityManifest(duplicate).status,
            ReplayVisibilityManifestStatus::DuplicateEntry);

  std::string badCount = manifest();
  badCount.replace(badCount.find("end 1 1"), 7, "end 1 2");
  EXPECT_EQ(parseReplayVisibilityManifest(badCount).status,
            ReplayVisibilityManifestStatus::CountMismatch);
}

TEST(GpuModernReplayVisibilityManifestTest,
     DoesNotImposeTheAdvisoryFourComponentPrior) {
  std::string value =
      "schema ps1-visibility-runtime-manifest/v1\n"
      "fingerprint_schema 1\n"
      "scope hypothesis\n"
      "family_count_prior_used 0\n"
      "integer_or_tenth_prior_used 0\n";
  for (std::size_t index = 0; index < 5; ++index)
    value += "component " + componentSha(index) + " " +
             std::to_string(index + 1) + " 1\n";
  for (std::size_t index = 0; index < 5; ++index)
    value += entry(componentSha(index), dispatchOwner(0x100u + index));
  value += "end 5 5\n";

  const ReplayVisibilityManifestSnapshot parsed =
      parseReplayVisibilityManifest(value);
  ASSERT_EQ(parsed.status, ReplayVisibilityManifestStatus::Ready)
      << parsed.error;
  EXPECT_EQ(parsed.components.size(), 5u);
}

TEST_F(GpuModernReplayVisibilityManifestFileTest,
       ReloadsChangedCandidatesAndFailsClosedOnInvalidReplacement) {
  write(manifest(31));
  ReplayVisibilityManifestFile file(path);
  ASSERT_TRUE(file.reload());
  auto found = file.lookup(dispatchOwner());
  ASSERT_TRUE(found.has_value());
  EXPECT_FLOAT_EQ(found->shaderMultiplier, 31.0f);
  EXPECT_TRUE(found->hypothesis);
  EXPECT_FALSE(file.lookup(dispatchOwner(0x99u)).has_value());

  write(manifest(100));
  ASSERT_TRUE(file.reloadIfChanged());
  found = file.lookup(dispatchOwner());
  ASSERT_TRUE(found.has_value());
  EXPECT_FLOAT_EQ(found->shaderMultiplier, 100.0f);

  write("schema broken\n");
  EXPECT_FALSE(file.reloadIfChanged());
  EXPECT_FALSE(file.snapshot().ready());
  EXPECT_FALSE(file.lookup(dispatchOwner()).has_value());
}

TEST_F(GpuModernReplayVisibilityManifestFileTest,
       PresentationPlanAppliesOnlyToSafeExactDispatchHits) {
  write(manifest(31));
  ReplayVisibilityManifestFile file(path);
  ASSERT_TRUE(file.reload());

  std::array<vulkan_abi::Vertex, 3> vertices{};
  for (vulkan_abi::Vertex &vertex : vertices)
    vertex.projectiveW = 32768.0f;

  const ReplayVisibilityPresentationDecision applied =
      planReplayVisibilityPresentation(file, dispatchOwner(), vertices);
  EXPECT_TRUE(applied.applies());
  EXPECT_FLOAT_EQ(applied.depthMultiplier, 31.0f);
  EXPECT_EQ(applied.componentSha256, componentA);

  ReplayVisibilityAuditOwner ineligible = dispatchOwner();
  ineligible.opaque = false;
  EXPECT_EQ(planReplayVisibilityPresentation(file, ineligible, vertices).status,
            ReplayVisibilityPresentationStatus::IneligibleOwner);
  EXPECT_EQ(
      planReplayVisibilityPresentation(file, dispatchOwner(0x99u), vertices)
          .status,
      ReplayVisibilityPresentationStatus::UnmappedOwner);
  EXPECT_EQ(planReplayVisibilityPresentation(
                file, dispatchOwner(),
                std::span<const vulkan_abi::Vertex>(vertices.data(), 2u))
                .status,
            ReplayVisibilityPresentationStatus::InvalidVertexSpan);

  vertices[2].projectiveW = 1.0e9f;
  const ReplayVisibilityPresentationDecision overflow =
      planReplayVisibilityPresentation(file, dispatchOwner(), vertices);
  EXPECT_EQ(overflow.status,
            ReplayVisibilityPresentationStatus::ProjectiveWOutOfDomain);
  EXPECT_FLOAT_EQ(overflow.depthMultiplier, 1.0f);
}
