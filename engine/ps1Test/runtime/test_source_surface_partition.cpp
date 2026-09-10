#include "runtime/games/crash3_scenery_detail.h"
#include "runtime/games/crash3_scenery_refinement.h"
#include "runtime/source_surface_cover.h"
#include "runtime/source_surface_partition.h"
#include "runtime/source_surface_patch.h"
#include <gtest/gtest.h>
#include <random>

namespace {
using namespace ps1::surface_partition;
Surface rectangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  int32_t z = 0) {
  return *makeSurface(std::array<Point, 4>{
      {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}}});
}
TEST(SourceSurfacePartition,
     ExactCoverageNeedsNeitherMatchingIndicesNorDiagonal) {
  const auto parent = rectangle(0, 0, 4, 4);
  const auto a =
      makeSurface(std::array<Point, 3>{{{2, 0, 0}, {4, 4, 0}, {4, 0, 0}}});
  const auto b =
      makeSurface(std::array<Point, 3>{{{2, 0, 0}, {2, 4, 0}, {4, 4, 0}}});
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_TRUE(certify(parent, std::array{rectangle(0, 0, 2, 4), *a, *b}));
}
TEST(SourceSurfacePartition, AreaEqualityDoesNotHideAnOverlapAndHole) {
  EXPECT_FALSE(
      certify(rectangle(0, 0, 4, 4),
              std::array{rectangle(0, 0, 2, 4), rectangle(0, 0, 2, 4)}));
}
TEST(SourceSurfacePatch, CurvedFineMeshRetainsExactSourceAnchorsAndOneDomain) {
  const std::array<Point,4> boundary{{{0,0,0},{4,0,0},{4,4,0},{0,4,0}}};
  const Point center{2,2,1};
  std::vector<Triangle> mesh;
  for (uint32_t i = 0; i < 4; ++i)
    mesh.push_back({boundary[i],boundary[(i+1)%4],center});
  EXPECT_TRUE(certifyAnchoredPatch(boundary,mesh));
  auto incomplete = mesh;
  incomplete.pop_back();
  EXPECT_FALSE(certifyAnchoredPatch(boundary,incomplete));
  auto layered = mesh;
  layered.push_back(mesh[0]);
  EXPECT_FALSE(certifyAnchoredPatch(boundary,layered));
  for (auto &triangle : mesh)
    for (auto &point : triangle)
      ++point[2];
  EXPECT_FALSE(certifyAnchoredPatch(boundary,mesh)); // A displaced ceiling.
}
TEST(SourceSurfacePartition, PlaneBoundaryAndFullCoverageAreExact) {
  const auto p = rectangle(0, 0, 4, 4);
  EXPECT_FALSE(
      certify(p, std::array{rectangle(0, 0, 2, 4), rectangle(2, 0, 4, 4, 1)}));
  EXPECT_FALSE(
      certify(p, std::array{rectangle(0, 0, 2, 4), rectangle(2, 0, 4, 3)}));
  EXPECT_FALSE(
      certify(p, std::array{rectangle(-1, 0, 1, 4), rectangle(1, 0, 3, 4)}));
  EXPECT_FALSE(certify(p, std::array{p}));
  EXPECT_FALSE(makeSurface(
      std::array<Point, 4>{{{0, 0, 0}, {4, 0, 0}, {4, 4, 1}, {0, 4, 0}}}));
  EXPECT_FALSE(makeSurface(
      std::array<Point, 4>{{{0, 0, 0}, {4, 4, 0}, {4, 0, 0}, {0, 4, 0}}}));
  EXPECT_FALSE(
      makeSurface(std::array<Point, 3>{{{0, 0, 0}, {40000, 0, 0}, {0, 4, 0}}}));
  EXPECT_FALSE(makeSurface(
      std::array<Point, 4>{{{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {4, 4, 0}}}));
}

TEST(SourceSurfaceCover, CrossBoundaryCoverageAndTJunctionsAreExact) {
  const auto parent = rectangle(0, 0, 4, 4);
  EXPECT_TRUE(certifyCover(parent, std::array{rectangle(-1, -1, 2, 5),
                                              rectangle(2, -1, 5, 2),
                                              rectangle(2, 2, 5, 5)}));
  EXPECT_FALSE(certify(parent, std::array{rectangle(-1, -1, 2, 5),
                                          rectangle(2, -1, 5, 2),
                                          rectangle(2, 2, 5, 5)}));
  EXPECT_FALSE(certifyCover(parent, std::array{rectangle(-1, -1, 2, 5),
                                               rectangle(2, -1, 5, 1),
                                               rectangle(2, 2, 5, 5)}));
  EXPECT_FALSE(certifyCover(
      parent, std::array{rectangle(-1, -1, 3, 5), rectangle(2, -1, 5, 5)}));
}
TEST(SourceSurfaceCover, InteriorHolesAndExteriorTouchingDoNotProveCoverage) {
  const auto parent = rectangle(0, 0, 4, 4);
  EXPECT_FALSE(certifyCover(
      parent, std::array{rectangle(0, 0, 4, 1), rectangle(0, 3, 4, 4),
                         rectangle(0, 1, 1, 3), rectangle(3, 1, 4, 3)}));
  EXPECT_FALSE(certifyCover(
      parent, std::array{rectangle(-2, 0, 0, 4), rectangle(4, 0, 6, 4)}));
  // A boundary through exact parent corners still enters its interior.
  const auto diagonal =
      *makeSurface(std::array<Point, 3>{{{-1, -1, 0}, {5, 5, 0}, {-1, 5, 0}}});
  EXPECT_FALSE(
      certifyCover(parent, std::array{diagonal, rectangle(-3, -1, -1, 5)}));
}
TEST(SourceSurfaceCover, LargeExactRatioBoundsDoNotNeedWideIntegerProducts) {
  using cover_detail::Ratio;
  EXPECT_TRUE(cover_detail::less(Ratio{8589410305, 8589410306},
                                 Ratio{8589410306, 8589410307}));
  EXPECT_FALSE(cover_detail::less(Ratio{1, 2}, Ratio{2, 4}));
  EXPECT_TRUE(cover_detail::less(Ratio{0, 1}, Ratio{1, 1}));
  EXPECT_FALSE(cover_detail::less(Ratio{1, 1}, Ratio{0, 1}));
  EXPECT_TRUE(certifyCover(rectangle(-32760, -32760, 32760, 32760),
                           std::array{rectangle(-32767, -32767, 0, 32767),
                                      rectangle(0, -32767, 32767, 32767)}));
}
std::vector<Triangle> supportedMesh() {
  return {
      {{{0, 0, 0}, {1, 0, 0}, {0, 4, 0}}}, {{{1, 0, 0}, {1, 4, 0}, {0, 4, 0}}},
      {{{3, 0, 0}, {4, 0, 0}, {3, 4, 0}}}, {{{4, 0, 0}, {4, 4, 0}, {3, 4, 0}}},
      {{{1, 0, 0}, {2, 0, 1}, {1, 4, 0}}}, {{{2, 0, 1}, {2, 4, 1}, {1, 4, 0}}},
      {{{2, 0, 1}, {3, 0, 0}, {2, 4, 1}}}, {{{3, 0, 0}, {3, 4, 0}, {2, 4, 1}}}};
}
TEST(SourceSurfaceCover, SharedSlopingSurfaceClosesTheExactSourceSeam) {
  const auto mesh = supportedMesh();
  auto result =
      connectedCover(rectangle(0, 0, 4, 4), mesh, std::array{0u, 1u, 2u, 3u},
                     std::array{4u, 5u, 6u, 7u});
  ASSERT_TRUE(result);
  auto expected = *result;
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(expected, (std::vector<uint32_t>{4, 5, 6, 7}));
  EXPECT_FALSE(connectedCover(rectangle(0, 0, 4, 4), mesh,
                              std::array{0u, 1u, 2u, 3u},
                              std::array{4u, 5u, 6u}));
  EXPECT_FALSE(connectedCover(rectangle(0, 0, 4, 4), mesh,
                              std::array{0u, 1u, 2u, 3u},
                              std::array{4u, 4u, 6u, 7u}));
}
TEST(SourceSurfaceCover,
     ProjectionAloneDoesNotAttachDisconnectedOrAmbiguousMeshes) {
  auto mesh = supportedMesh();
  for (uint32_t i = 4; i < mesh.size(); ++i)
    for (auto &point : mesh[i])
      ++point[2];
  EXPECT_FALSE(connectedCover(rectangle(0, 0, 4, 4), mesh,
                              std::array{0u, 1u, 2u, 3u},
                              std::array{4u, 5u, 6u, 7u}));
  mesh = supportedMesh();
  auto other = mesh[4];
  other[1][2] = 2;
  mesh.push_back(other);
  EXPECT_FALSE(connectedCover(rectangle(0, 0, 4, 4), mesh,
                              std::array{0u, 1u, 2u, 3u},
                              std::array{4u, 5u, 6u, 7u, 8u}));
}

TEST(SourceSurfaceCover, AffineGridPropertyCasesRejectMissingAndOverlappingCells) {
  std::mt19937 random(0x5345414d);
  for (uint32_t iteration = 0; iteration < 128; ++iteration) {
    const int w = 2+random()%5, h = 2+random()%5;
    const int shear = 1+random()%7, scale = 2+random()%9;
    const auto point = [&](int x, int y) -> Point {
      return {scale*x+shear*y-1000,-x+scale*y+800,0};
    };
    const auto parent = *makeSurface(std::array<Point,3>{
        point(1,1),point(4*w-1,1),point(1,4*h-1)});
    std::vector<Surface> pieces;
    uint32_t firstCellPieces = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      const auto a = point(4*x,4*y), b = point(4*x+4,4*y);
      const auto c = point(4*x+4,4*y+4), d = point(4*x,4*y+4);
      if (random()%2) {
        pieces.push_back(*makeSurface(std::array{a,b,c}));
        pieces.push_back(*makeSurface(std::array{a,c,d}));
      } else pieces.push_back(*makeSurface(std::array{a,b,c,d}));
      if (x == 0 && y == 0) firstCellPieces = pieces.size();
    }
    ASSERT_TRUE(certifyCover(parent,pieces)) << iteration;
    auto incomplete = pieces;
    incomplete.erase(incomplete.begin(),incomplete.begin()+firstCellPieces);
    EXPECT_FALSE(certifyCover(parent,incomplete)) << iteration;
    pieces.push_back(pieces.front());
    EXPECT_FALSE(certifyCover(parent,pieces)) << iteration;
  }
}

struct Fixture {
  using Vertex = ps1::games::crash3::ResidentGpuRawVertex;
  using Face = ps1::games::crash3::ResidentGpuTopologyFace;
  using Material = ps1::games::crash3::ResidentPresentationMaterial;
  std::vector<Vertex> vertices;
  std::vector<Face> faces;
  std::vector<Material> materials;
  std::vector<std::vector<uint16_t>> states{{0x1800}, {0x1801, 0x1802}};
  Fixture() {
    for (const auto &bounds :
         {std::array{0u, 4u}, std::array{0u, 2u}, std::array{2u, 4u}}) {
      const uint32_t base = static_cast<uint32_t>(vertices.size());
      for (auto xy : {std::array{bounds[0], 0u}, std::array{bounds[1], 0u},
                      std::array{bounds[0], 4u}, std::array{bounds[1], 4u}})
        vertices.push_back(
            {(xy[0] << 4) | (xy[1] << 20), 0, true, {128, 128, 128}});
      Material m;
      m.decoded = m.textured = true;
      m.sourceToken = m.resolvedToken = 1;
      m.texturePage = 0x60;
      m.packetVertexCount = 4;
      m.packetUv = {{{0, 0}, {31, 0}, {0, 31}, {31, 31}}};
      for (uint8_t half : {1, 2}) {
        Face f;
        f.resourceFaceOrdinal = static_cast<uint32_t>(faces.size());
        f.half = half;
        f.completeSourceVertexIndex = {base + 1, base, base + 2, base + 3};
        f.sourceVertexIndex = half == 1
                                  ? std::array{base, base + 1, base + 2}
                                  : std::array{base + 1, base + 2, base + 3};
        faces.push_back(f);
        materials.push_back(m);
      }
    }
  }
  auto run() {
    const std::array<ps1::games::crash3::SceneryPathStates, 1> paths{states};
    return ps1::games::crash3::findScenerySurfaceRefinements(
        faces, vertices, materials, 0, 0, 3, paths);
  }
};
TEST(Crash3SurfaceRefinement, NativeProgramCertifiesManyFaceAlternative) {
  Fixture f;
  const auto result = f.run();
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].lowerFaces, (std::vector<uint32_t>{0, 1}));
  EXPECT_EQ(result[0].higherFaces, (std::vector<uint32_t>{2, 3, 4, 5}));
}
TEST(Crash3SurfaceRefinement, ProgramBindingsAreNotWholeScenePlacement) {
  using namespace ps1::games::crash3;
  std::array<ResidentPresentationResource, 2> resources{};
  for (uint32_t i = 0; i < resources.size(); ++i) {
    resources[i].resourceId = 3 + 2*i;
    resources[i].geometryFingerprint = 101 + i;
    resources[i].identityValid = true;
    resources[i].quadCount = 3;
    // No placement/cohort approval: it must not veto source alternatives.
  }
  EXPECT_TRUE(sceneryVariantBindingsReady(resources));
  resources[1].identityValid = false;
  EXPECT_FALSE(sceneryVariantBindingsReady(resources));
  resources[1].identityValid = true;
  resources[1].quadCount = 2049;
  EXPECT_FALSE(sceneryVariantBindingsReady(resources));
  resources[1].quadCount = 3;
  resources[1].resourceId = resources[0].resourceId;
  EXPECT_FALSE(sceneryVariantBindingsReady(resources));
  EXPECT_FALSE(sceneryVariantBindingsReady({}));
}
TEST(Crash3SurfaceRefinement, FullOpaqueHandoffIsNotAnIndependentLayer) {
  using namespace ps1::games::crash3;
  Fixture f;
  f.states.insert(f.states.begin()+1, {0x1800, 0x1801, 0x1802});
  auto result = f.run();
  ASSERT_EQ(result.size(), 1u);
  EXPECT_FALSE(scenerySurfaceRefinementContradicted(
      result[0], 0, 0, std::array<SceneryPathStates, 1>{f.states}));
  f.states[1] = {0x1800, 0x1801}; // Partial overdraw is NOT a full handoff.
  auto anchored = f.run();
  ASSERT_EQ(anchored.size(), 1u);
  EXPECT_TRUE(anchored[0].anchoredSourcePatch); // Stronger whole-source proof.
  EXPECT_TRUE(scenerySurfaceRefinementContradicted(
      result[0], 0, 0, std::array<SceneryPathStates, 1>{f.states}));
  f.states = {{0x1800}, {0x1800, 0x1801, 0x1802}};
  EXPECT_TRUE(f.run().empty()); // No independent fine-only native witness.
}
TEST(Crash3SurfaceRefinement, BoundSectionsCanOwnOppositeDetailBranches) {
  using namespace ps1::games::crash3;
  Fixture f;
  std::vector<std::vector<uint16_t>> far{{0x1800}};
  std::vector<std::vector<uint16_t>> near{{0x1801, 0x1802}};
  const auto result = findScenerySurfaceRefinements(
      f.faces, f.vertices, f.materials, 0, 0, 3,
      std::array<SceneryPathStates, 2>{far, near});
  ASSERT_EQ(result.size(), 1u);
}
TEST(Crash3SurfaceRefinement, NativeCullingOfOneCoarseHalfDoesNotHideItsBranch) {
  Fixture f;
  f.states[0] = {0x800};
  auto result = f.run();
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].lowerFaces, (std::vector<uint32_t>{0, 1}));
  EXPECT_EQ(result[0].higherFaces, (std::vector<uint32_t>{2, 3, 4, 5}));
  f.states[1] = {0x801, 0x1802};
  EXPECT_TRUE(f.run().empty()); // Finer side still owes both halves.
}
TEST(Crash3SurfaceRefinement, NativeFineSubdivisionCanCrossCoarsePolygonEdges) {
  using namespace ps1::games::crash3;
  Fixture f;
  const std::array<std::array<uint32_t, 2>, 3> bounds{{{1,7},{0,4},{4,8}}};
  for (uint32_t polygon = 0; polygon < 3; ++polygon)
    for (uint32_t corner = 0; corner < 4; ++corner) {
      auto &xy = f.vertices[4*polygon+corner].rawXy;
      xy = (xy & ~0xfff0u) | (bounds[polygon][corner & 1] << 4);
    }
  auto result = findScenerySurfaceRefinements(
      f.faces, f.vertices, f.materials, 0, 0, 3,
      std::array<SceneryPathStates, 1>{f.states}, false);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].lowerFaces, (std::vector<uint32_t>{0, 1}));
  EXPECT_TRUE(result[0].supportFaces.empty());
}
TEST(Crash3SurfaceRefinement, DirectDistanceRemapsHaveStaticNearestGeometryFacts) {
  using namespace ps1::games::crash3;
  Fixture f;
  for (auto &m : f.materials) {
    m.sourceToken = 0x803;
    m.resolvedToken = 15;
    m.dynamicTexture = true;
  }
  EXPECT_TRUE(f.run().empty());
  const uint32_t descriptor = 0x8000000d;
  for (auto &m : f.materials) {
    auto nearest = m;
    nearest.resolvedToken = 13;
    const auto canonical = sceneryRefinementMaterial(m, descriptor, &nearest, 29);
    ASSERT_TRUE(canonical);
    EXPECT_TRUE(m.dynamicTexture); // Certificate view cannot mutate either lane.
    EXPECT_TRUE(nearest.dynamicTexture);
    m = *canonical;
  }
  ASSERT_EQ(f.run().size(), 1u);
}
TEST(Crash3SurfaceRefinement, AnimatedOrUnprovenNearestMaterialsStayExcluded) {
  using namespace ps1::games::crash3;
  auto native = Fixture{}.materials[0];
  native.sourceToken = 0x803;
  native.dynamicTexture = true;
  auto nearest = native;
  nearest.resolvedToken = 13;
  EXPECT_FALSE(sceneryRefinementMaterial(native, 13, &nearest, 29));
  EXPECT_FALSE(sceneryRefinementMaterial(native, 0x8000000d, nullptr, 29));
  EXPECT_FALSE(sceneryRefinementMaterial(native, 0x8000000e, &nearest, 29));
  nearest.sourceToken = 0x805;
  EXPECT_FALSE(sceneryRefinementMaterial(native, 0x8000000d, &nearest, 29));
  nearest.sourceToken = native.sourceToken;
  nearest.semiTransparent = true;
  EXPECT_FALSE(sceneryRefinementMaterial(native, 0x8000000d, &nearest, 29));
  nearest.semiTransparent = false;
  nearest.decoded = false;
  EXPECT_FALSE(sceneryRefinementMaterial(native, 0x8000000d, &nearest, 29));
}
TEST(Crash3SurfaceRefinement,
     CoSelectedLayersAndIncompleteQuadAreNotAlternatives) {
  Fixture f;
  f.states[0].push_back(0x1001); // No exclusive coarse branch remains.
  EXPECT_TRUE(f.run().empty());
  f.states = {{0x1800}, {0x800 | 1, 0x1802}};
  EXPECT_TRUE(f.run().empty());
}
TEST(Crash3SurfaceRefinement, AuthoredCurvedFinePatchIsNotForcedOntoCoarsePlane) {
  using namespace ps1::games::crash3;
  Fixture f;
  for (uint32_t i : {5u,7u,8u,10u})
    f.vertices[i].rawZ = 0x10;
  f.states.insert(f.states.begin()+1, {0x800,0x1801});
  const auto result = f.run();
  ASSERT_EQ(result.size(),1u);
  EXPECT_TRUE(result[0].anchoredSourcePatch);
  EXPECT_EQ(result[0].higherFaces,(std::vector<uint32_t>{2,3,4,5}));
  EXPECT_FALSE(scenerySurfaceRefinementContradicted(
      result[0],0,0,std::array<SceneryPathStates,1>{f.states}));
  f.vertices[4].rawZ = 0x10; // Lost one source anchor: no height tolerance.
  EXPECT_TRUE(f.run().empty());
}
TEST(Crash3SurfaceRefinement, NoWorseTexturesAndNoRestPoseDeformationGuess) {
  Fixture f;
  f.materials[2].packetUv = {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
  EXPECT_TRUE(f.run().empty());
  f = Fixture{};
  f.vertices[4].rawXy |= 1u << 18;
  EXPECT_TRUE(f.run().empty());
  f = Fixture{};
  f.materials[2].semiTransparent = true;
  EXPECT_TRUE(f.run().empty());
  f = Fixture{};
  f.materials[2].dynamicTexture = true;
  EXPECT_TRUE(f.run().empty());
}
TEST(Crash3SurfaceRefinement, LaterProgramCanVetoButAbsentEvidenceIsNotAVeto) {
  using namespace ps1::games::crash3;
  Fixture f;
  const auto families = f.run();
  ASSERT_EQ(families.size(), 1u);
  f.states = {{0x1800}};
  EXPECT_FALSE(scenerySurfaceRefinementContradicted(
      families[0], 0, 0, std::array<SceneryPathStates, 1>{f.states}));
  f.states = {{0x800, 0x1001}};
  EXPECT_TRUE(scenerySurfaceRefinementContradicted(
      families[0], 0, 0, std::array<SceneryPathStates, 1>{f.states}));
}
TEST(Crash3SurfaceRefinement, LookupChecksIdentityAndBothHalvesWithoutAnAlias) {
  using namespace ps1::games::crash3;
  Fixture f;
  SceneryVariantPlan plan;
  plan.resourceId = 42;
  plan.geometryFingerprint = 123;
  plan.refinements = f.run();
  const auto *first = scenerySurfaceRefinement(plan, 42, 123, 0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 42, 123, 1), first);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 42, 123, 2), nullptr);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 43, 123, 0), nullptr);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 42, 124, 0), nullptr);
  EXPECT_FALSE(sceneryVariantReplacement(plan, 42, 123, 0).has_value());
  plan.refinements = {ScenerySurfaceRefinement{}};
  EXPECT_EQ(scenerySurfaceRefinement(plan, 42, 123, 0), nullptr);
}

struct SupportFixture {
  std::vector<ps1::games::crash3::ResidentGpuRawVertex> vertices;
  std::vector<ps1::games::crash3::ResidentGpuTopologyFace> faces;
  std::vector<ps1::games::crash3::ResidentPresentationMaterial> materials;
  std::vector<std::vector<uint16_t>> states{{0x1800, 4, 5, 6, 7},
                                            {0, 1, 2, 3, 4, 5, 6, 7}};
  SupportFixture() {
    using namespace ps1::games::crash3;
    const auto vertex = [&](Point point) {
      const auto index = static_cast<uint32_t>(vertices.size());
      vertices.push_back({(static_cast<uint32_t>(point[0]) << 4) |
                              (static_cast<uint32_t>(point[1]) << 20),
                          static_cast<uint16_t>(point[2] << 4),
                          true,
                          {128, 128, 128}});
      return index;
    };
    ResidentPresentationMaterial material;
    material.decoded = material.textured = true;
    material.sourceToken = material.resolvedToken = 1;
    material.texturePage = 0x60;
    material.packetVertexCount = 3;
    material.packetUv = {{{0, 0}, {31, 0}, {0, 31}, {31, 31}}};
    for (const auto &triangle : supportedMesh()) {
      ResidentGpuTopologyFace face;
      face.resourceFaceOrdinal = faces.size();
      for (uint32_t i = 0; i < 3; ++i)
        face.sourceVertexIndex[i] = vertex(triangle[i]);
      faces.push_back(face);
      materials.push_back(material);
    }
    const auto b = vertex({0, 0, 0}), a = vertex({4, 0, 0});
    const auto c = vertex({0, 4, 0}), d = vertex({4, 4, 0});
    material.packetVertexCount = 4;
    for (uint8_t half : {1, 2}) {
      ResidentGpuTopologyFace face;
      face.resourceFaceOrdinal = faces.size();
      face.half = half;
      face.completeSourceVertexIndex = {a, b, c, d};
      face.sourceVertexIndex =
          half == 1 ? std::array{b, a, c} : std::array{a, c, d};
      faces.push_back(face);
      materials.push_back(material);
    }
  }
  auto run(bool allow = true) {
    using namespace ps1::games::crash3;
    return findScenerySurfaceRefinements(
        faces, vertices, materials, 0, 8, 1,
        std::array<SceneryPathStates, 1>{states}, allow);
  }
};
TEST(Crash3SurfaceRefinement,
     SharedNativeStructureIsNotAnExclusiveReplacement) {
  using namespace ps1::games::crash3;
  SupportFixture f;
  EXPECT_TRUE(f.run(false).empty());
  const auto families = f.run();
  ASSERT_EQ(families.size(), 1u);
  EXPECT_EQ(families[0].lowerFaces, (std::vector<uint32_t>{8, 9}));
  EXPECT_EQ(families[0].higherFaces, (std::vector<uint32_t>{0, 1, 2, 3}));
  EXPECT_EQ(families[0].supportFaces, (std::vector<uint32_t>{4, 5, 6, 7}));
  EXPECT_FALSE(scenerySurfaceRefinementContradicted(
      families[0], 0, 8, std::array<SceneryPathStates, 1>{f.states}));
  f.states[1].push_back(0x1800);
  EXPECT_TRUE(scenerySurfaceRefinementContradicted(
      families[0], 0, 8, std::array<SceneryPathStates, 1>{f.states}));
}
TEST(Crash3SurfaceRefinement,
     SharedSupportsMustExistInEveryRelevantNativeState) {
  SupportFixture f;
  f.states.push_back({0, 1, 2, 3, 4, 5, 6});
  EXPECT_TRUE(f.run().empty());
  f = SupportFixture{};
  f.states[0] = {0x1800, 4, 5, 6};
  EXPECT_TRUE(f.run().empty());
  f = SupportFixture{};
  f.materials[7].semiTransparent = true;
  EXPECT_TRUE(f.run().empty());
}
} // namespace
