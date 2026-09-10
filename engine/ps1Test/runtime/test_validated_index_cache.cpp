#include "runtime/validated_index_cache.h"

#include <gtest/gtest.h>
#include <vector>

TEST(ValidatedIndexCache,
     RevalidatesIdentityAfterReorderReallocationAndRestore) {
  ps1::ValidatedIndexCache cache;
  std::vector<int> values{1, 2, 3};
  const auto lookup = [&](int key) {
    return cache.find(std::span{values},
                      [=](int value) { return value == key; });
  };
  EXPECT_EQ(lookup(2), &values[1]);
  std::swap(values[0], values[1]);
  EXPECT_EQ(lookup(2), &values[0]);
  values.reserve(1000);
  EXPECT_EQ(lookup(2), &values[0]);
  EXPECT_EQ(lookup(3), &values[2]);
  values.clear();
  EXPECT_EQ(lookup(3), nullptr);
  values = {3, 4};
  EXPECT_EQ(lookup(3), &values[0]);
  EXPECT_EQ(lookup(8), nullptr);
}

TEST(ValidatedIndexCache, ColdAndWarmPathsKeepTheSameOwnershipScope) {
  struct Scope {
    int key;
    std::vector<int> sources;
  };
  std::vector<Scope> scopes{{1, {100, 200}}, {2, {200, 100}}};
  ps1::ValidatedIndexCache owners, sources;
  const auto lookup = [&](int owner, int source) -> int * {
    auto *scope = owners.find(std::span{scopes},
                              [=](const Scope &s) { return s.key == owner; });
    return scope == nullptr
               ? nullptr
               : sources.find(std::span{scope->sources},
                              [=](int value) { return value == source; });
  };
  EXPECT_EQ(lookup(1, 100), &scopes[0].sources[0]);
  EXPECT_EQ(lookup(2, 100), &scopes[1].sources[1]);
  EXPECT_EQ(lookup(1, 200), &scopes[0].sources[1]);
  EXPECT_EQ(lookup(3, 100), nullptr);
  scopes.erase(scopes.begin());
  EXPECT_EQ(lookup(2, 100), &scopes[0].sources[1]);
  EXPECT_EQ(lookup(1, 100), nullptr);
}
