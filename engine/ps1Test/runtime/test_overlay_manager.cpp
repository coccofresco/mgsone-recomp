#include <gtest/gtest.h>
#include <runtime/overlay_manager.h>
#include <vector>

using namespace ps1;

// Basic Registration

TEST(OverlayManagerTest, RegisterOverlay) {
  OverlayManager mgr;
  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);
  mgr.registerOverlay("menu", 0x80040000, 0x4000, 1);

  EXPECT_EQ(mgr.overlayCount(), 2u);
  EXPECT_EQ(mgr.activeCount(), 0u);
}

// Activation

TEST(OverlayManagerTest, ActivateDeactivate) {
  OverlayManager mgr;
  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);

  EXPECT_FALSE(mgr.isActive("battle"));
  EXPECT_TRUE(mgr.activateOverlay("battle"));
  EXPECT_TRUE(mgr.isActive("battle"));
  EXPECT_EQ(mgr.activeCount(), 1u);

  EXPECT_TRUE(mgr.deactivateOverlay("battle"));
  EXPECT_FALSE(mgr.isActive("battle"));
  EXPECT_EQ(mgr.activeCount(), 0u);
}

TEST(OverlayManagerTest, ActivateNonexistent) {
  OverlayManager mgr;
  EXPECT_FALSE(mgr.activateOverlay("nonexistent"));
}

// Memory Write Notification

TEST(OverlayManagerTest, NotifyActivatesOverlay) {
  OverlayManager mgr;
  std::vector<std::pair<int, bool>> hookCalls;

  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);

  mgr.setDispatchHook([&](int idx, bool active) {
    hookCalls.push_back({idx, active});
  });

  // Write to the overlay's RAM region
  mgr.notifyMemWrite(0x80040000, 0x8000);

  EXPECT_TRUE(mgr.isActive("battle"));
  ASSERT_EQ(hookCalls.size(), 1u);
  EXPECT_EQ(hookCalls[0].first, 0);
  EXPECT_TRUE(hookCalls[0].second);
}

TEST(OverlayManagerTest, NotifyOutsideRegion) {
  OverlayManager mgr;
  bool called = false;

  mgr.registerOverlay("battle", 0x80040000, 0x8000, 0);
  mgr.setDispatchHook([&](int, bool) { called = true; });

  // Write outside the overlay region
  mgr.notifyMemWrite(0x80010000, 0x1000);

  EXPECT_FALSE(mgr.isActive("battle"));
  EXPECT_FALSE(called);
}

TEST(OverlayManagerTest, ConflictDeactivatesOldOverlay) {
  OverlayManager mgr;
  std::vector<std::pair<int, bool>> hookCalls;

  // Two overlays at the same address (Crash levels, etc.)
  mgr.registerOverlay("level1", 0x80040000, 0x8000, 0);
  mgr.registerOverlay("level2", 0x80040000, 0x8000, 1);

  mgr.setDispatchHook([&](int idx, bool active) {
    hookCalls.push_back({idx, active});
  });

  // Load level1
  mgr.notifyMemWrite(0x80040000, 0x8000);
  EXPECT_TRUE(mgr.isActive("level1"));
  EXPECT_FALSE(mgr.isActive("level2"));

  hookCalls.clear();

  // Load level2 into same region -- should deactivate level1
  mgr.notifyMemWrite(0x80040000, 0x8000);

  EXPECT_FALSE(mgr.isActive("level1"));
  EXPECT_TRUE(mgr.isActive("level2"));

  // Should have: deactivate(level1), activate(level2)
  ASSERT_GE(hookCalls.size(), 2u);

  // Find deactivation of level1
  bool foundDeactivate = false;
  bool foundActivate = false;
  for (const auto &[idx, active] : hookCalls) {
    if (idx == 0 && !active) foundDeactivate = true;
    if (idx == 1 && active) foundActivate = true;
  }
  EXPECT_TRUE(foundDeactivate);
  EXPECT_TRUE(foundActivate);
}

TEST(OverlayManagerTest, PartialOverlapActivates) {
  OverlayManager mgr;

  mgr.registerOverlay("code", 0x80040000, 0x4000, 0);

  // Write that partially overlaps the overlay region
  mgr.notifyMemWrite(0x80042000, 0x4000); // overlaps 0x80042000-0x80046000

  EXPECT_TRUE(mgr.isActive("code"));
}

TEST(OverlayManagerTest, RepeatedWriteDoesntDuplicateActivation) {
  OverlayManager mgr;
  int activateCount = 0;

  mgr.registerOverlay("code", 0x80040000, 0x4000, 0);
  mgr.setDispatchHook([&](int, bool active) {
    if (active) activateCount++;
  });

  // Write twice to same region
  mgr.notifyMemWrite(0x80040000, 0x4000);
  mgr.notifyMemWrite(0x80040000, 0x4000);

  // Should only activate once (already active)
  EXPECT_EQ(activateCount, 1);
}
