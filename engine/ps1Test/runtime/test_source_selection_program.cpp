#include "runtime/games/crash3_scenery_detail.h"
#include "runtime/source_selection_program.h"
#include <gtest/gtest.h>

namespace {
using ps1::source_selection::compile;
using ps1::source_selection::Rule;
using namespace ps1::games::crash3;

TEST(SourceSelectionProgram, ExplicitChainNeverReintroducesItsAncestor) {
  const std::vector<Rule> rules{{{0}, {1, 2}}, {{1}, {3, 4}}, {{2}, {4, 5}}};
  const auto result = compile(7, rules);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->owner, (std::vector<uint32_t>{1, 2, 3, 0, 0, 0, 0}));
  EXPECT_EQ(result->terminals[0], (std::vector<uint32_t>{3, 4, 5}));
  EXPECT_EQ(result->selectedSources, (std::vector<uint32_t>{3, 4, 5, 6}));
  EXPECT_EQ(result->chainedRules, 1u);
  // Shared terminal 4 is an obligation once, not a duplicate draw.
  for (const auto &targets : result->terminals)
    for (uint32_t face : targets)
      EXPECT_EQ(result->owner[face], 0u);
}

TEST(SourceSelectionProgram, NoGeometryOrSourceOrderingChoosesTheAlternative) {
  const std::vector<Rule> rules{{{2, 3}, {0, 1}}, {{0}, {4}}};
  const auto result = compile(6, rules);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->terminals[0], (std::vector<uint32_t>{1, 4}));
  auto reversed = rules;
  std::reverse(reversed.begin(), reversed.end());
  const auto other = compile(6, reversed);
  ASSERT_TRUE(other);
  EXPECT_EQ(other->selectedSources, result->selectedSources);
  for (uint32_t face : {0, 2, 3})
    EXPECT_EQ(other->terminals[other->owner[face] - 1],
              result->terminals[result->owner[face] - 1]);
}

TEST(SourceSelectionProgram, RejectsCyclesConflictsAndOutOfRangeAtomically) {
  for (const std::vector<Rule> rules :
       {std::vector<Rule>{{{0}, {1}}, {{1}, {0}}},
        std::vector<Rule>{{{0}, {0}}},
        std::vector<Rule>{{{0}, {1}}, {{0}, {2}}},
        std::vector<Rule>{{{0, 0}, {1}}}, std::vector<Rule>{{{3}, {1}}},
        std::vector<Rule>{{{0}, {3}}}, std::vector<Rule>{{{}, {1}}},
        std::vector<Rule>{{{0}, {}}}})
    EXPECT_FALSE(compile(3, rules));
  EXPECT_FALSE(compile(65537, {}));
  const std::vector<Rule> expansive{{{0}, {1, 2}}, {{1}, {3, 4}}};
  EXPECT_FALSE(compile(5, expansive, 5, 4));
}

TEST(SourceSelectionProgram, LongChainsUseNoRecursiveStack) {
  std::vector<Rule> rules;
  for (uint32_t i = 0; i < 5999; ++i)
    rules.push_back({{i}, {i + 1}});
  const auto result = compile(6000, rules);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->selectedSources, (std::vector<uint32_t>{5999}));
  EXPECT_EQ(result->terminals.front(), result->selectedSources);
  EXPECT_EQ(result->chainedRules, 5998u);
}

TEST(Crash3SourceSelection, CompilesMeshAndMaterialDependenciesBeforeDraws) {
  SceneryVariantPlan plan;
  plan.resourceId = 7;
  plan.geometryFingerprint = 23;
  plan.refinements = {{{0, 1}, {2, 3}, {6}}, {{2, 3}, {4, 5}, {}}};
  plan.replacements = {{{4, 7}}};
  const auto sourceFamilies = plan.refinements;
  ASSERT_TRUE(compileSceneryDrawSelection(plan, 8));
  EXPECT_EQ(plan.refinements,
            sourceFamilies); // Not relabelled as direct proof.
  ASSERT_TRUE(plan.selection);
  EXPECT_EQ(plan.selection->program.selectedSources,
            (std::vector<uint32_t>{5, 6, 7}));
  for (uint32_t lower : {0, 1, 2, 3}) {
    const auto *draw = scenerySurfaceRefinement(plan, 7, 23, lower);
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->higherFaces, (std::vector<uint32_t>{5, 7}));
    EXPECT_EQ(draw->supportFaces,
              (lower < 2 ? std::vector<uint32_t>{6} : std::vector<uint32_t>{}));
    EXPECT_FALSE(sceneryVariantReplacement(plan, 7, 23, lower));
  }
  EXPECT_EQ(sceneryVariantReplacement(plan, 7, 23, 4), 7u);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 7, 23, 4), nullptr);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 7, 24, 0), nullptr);
  EXPECT_EQ(scenerySurfaceRefinement(plan, 7, 23, 8), nullptr);
  // Caller mutation cannot change an already retained presentation endpoint.
  const auto retained = plan.selection;
  plan.refinements.push_back({{7}, {0}, {}}); // Creates a cycle.
  EXPECT_FALSE(compileSceneryDrawSelection(plan, 8));
  EXPECT_EQ(plan.selection, retained);
}

TEST(Crash3SourceSelection, MeshOwnershipTakesPrecedenceOverSameFaceAlias) {
  SceneryVariantPlan plan;
  plan.refinements = {{{0, 1}, {2, 3}, {}}};
  plan.replacements = {{{0, 4}}, {{1, 5}}, {{6, 0}}};
  ASSERT_TRUE(compileSceneryDrawSelection(plan, 7));
  EXPECT_EQ(plan.selection->program.selectedSources,
            (std::vector<uint32_t>{2, 3, 4, 5}));
  EXPECT_FALSE(sceneryVariantReplacement(plan, 0, 0, 6));
  const auto *draw = scenerySurfaceRefinement(plan, 0, 0, 6);
  ASSERT_NE(draw, nullptr);
  EXPECT_EQ(draw->higherFaces, (std::vector<uint32_t>{2, 3}));
}
} // namespace
