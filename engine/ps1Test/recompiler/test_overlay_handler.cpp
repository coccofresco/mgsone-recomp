// Tests for ps1Recomp -- Overlay Handler
// Validates overlay section management, address lookup, and dispatch table

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ps1recomp/overlay_handler.h>

using namespace ps1recomp;

// Helper: create a temporary TOML config

static std::string createTempConfig(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "test_overlay.toml";
  std::ofstream ofs(path);
  ofs << content;
  ofs.close();
  return path.string();
}

// Basic Overlay Management

TEST(OverlayHandler, AddOverlay) {
  OverlayHandler handler;
  EXPECT_EQ(handler.overlayCount(), 0u);

  handler.addOverlay({"battle", 0x10000, 0x80040000, 0x8000, {}});
  EXPECT_EQ(handler.overlayCount(), 1u);

  handler.addOverlay({"menu", 0x20000, 0x80050000, 0x4000, {}});
  EXPECT_EQ(handler.overlayCount(), 2u);
}

TEST(OverlayHandler, OverlayData) {
  OverlayHandler handler;
  handler.addOverlay(
      {"battle", 0x10000, 0x80040000, 0x8000, {0x80040100, 0x80040200}});

  const auto &overlays = handler.overlays();
  ASSERT_EQ(overlays.size(), 1u);
  EXPECT_EQ(overlays[0].name, "battle");
  EXPECT_EQ(overlays[0].romOffset, 0x10000u);
  EXPECT_EQ(overlays[0].ramBase, 0x80040000u);
  EXPECT_EQ(overlays[0].size, 0x8000u);
  EXPECT_EQ(overlays[0].functions.size(), 2u);
}

// Address Lookup

TEST(OverlayHandler, IsOverlayAddress) {
  OverlayHandler handler;
  handler.addOverlay({"battle", 0x10000, 0x80040000, 0x8000, {}});

  // Inside overlay range
  EXPECT_TRUE(handler.isOverlayAddress(0x80040000));
  EXPECT_TRUE(handler.isOverlayAddress(0x80040100));
  EXPECT_TRUE(handler.isOverlayAddress(0x80047FFF));

  // Outside overlay range
  EXPECT_FALSE(handler.isOverlayAddress(0x8003FFFF));
  EXPECT_FALSE(handler.isOverlayAddress(0x80048000));
  EXPECT_FALSE(handler.isOverlayAddress(0x80010000));
}

TEST(OverlayHandler, FindOverlay) {
  OverlayHandler handler;
  handler.addOverlay({"battle", 0x10000, 0x80040000, 0x8000, {}});
  handler.addOverlay({"menu", 0x20000, 0x80050000, 0x4000, {}});

  auto *ov = handler.findOverlay(0x80040100);
  ASSERT_NE(ov, nullptr);
  EXPECT_EQ(ov->name, "battle");

  ov = handler.findOverlay(0x80050100);
  ASSERT_NE(ov, nullptr);
  EXPECT_EQ(ov->name, "menu");

  EXPECT_EQ(handler.findOverlay(0x80010000), nullptr);
}

TEST(OverlayHandler, AddressBoundaries) {
  OverlayHandler handler;
  handler.addOverlay({"test", 0, 0x80040000, 0x1000, {}});

  // Exact start -- inside
  EXPECT_TRUE(handler.isOverlayAddress(0x80040000));
  // Last byte -- inside
  EXPECT_TRUE(handler.isOverlayAddress(0x80040FFF));
  // One past end -- outside
  EXPECT_FALSE(handler.isOverlayAddress(0x80041000));
  // One before start -- outside
  EXPECT_FALSE(handler.isOverlayAddress(0x8003FFFF));
}

// Conflict Detection

TEST(OverlayHandler, FindConflicts) {
  OverlayHandler handler;
  // Two overlays at the same RAM address!
  handler.addOverlay({"battle", 0x10000, 0x80040000, 0x8000, {}});
  handler.addOverlay({"menu", 0x20000, 0x80040000, 0x4000, {}});

  auto conflicts = handler.findConflicts(0x80040100);
  EXPECT_EQ(conflicts.size(), 2u);
  EXPECT_EQ(conflicts[0]->name, "battle");
  EXPECT_EQ(conflicts[1]->name, "menu");

  // Address only in battle (past menu's size)
  conflicts = handler.findConflicts(0x80044100);
  EXPECT_EQ(conflicts.size(), 1u);
  EXPECT_EQ(conflicts[0]->name, "battle");

  // No conflicts outside
  conflicts = handler.findConflicts(0x80010000);
  EXPECT_TRUE(conflicts.empty());
}

// Qualified Naming

TEST(OverlayHandler, QualifiedName) {
  OverlayHandler handler;
  handler.addOverlay({"battle", 0x10000, 0x80040000, 0x8000, {}});

  // Address in overlay -> prefixed name
  auto name = handler.qualifiedName(0x80040100);
  EXPECT_NE(name.find("overlay_battle"), std::string::npos);
  EXPECT_NE(name.find("80040100"), std::string::npos);
}

TEST(OverlayHandler, QualifiedNameNoOverlay) {
  OverlayHandler handler;

  // Address not in any overlay -> fallback
  auto name = handler.qualifiedName(0x80010000);
  EXPECT_EQ(name, "func_80010000");
}

// Dispatch Table Emission

TEST(OverlayHandler, EmitDispatchTable) {
  OverlayHandler handler;
  handler.addOverlay(
      {"battle", 0x10000, 0x80040000, 0x8000, {0x80040100, 0x80040200}});
  handler.addOverlay({"menu", 0x20000, 0x80050000, 0x4000, {0x80050100}});

  auto table = handler.emitDispatchTable();
  EXPECT_NE(table.find("lookup_overlay_func"), std::string::npos);
  EXPECT_NE(table.find("case 0:"), std::string::npos); // battle
  EXPECT_NE(table.find("case 1:"), std::string::npos); // menu
  EXPECT_NE(table.find("overlay_battle__80040100"), std::string::npos);
  EXPECT_NE(table.find("overlay_battle__80040200"), std::string::npos);
  EXPECT_NE(table.find("overlay_menu__80050100"), std::string::npos);
}

TEST(OverlayHandler, EmitDispatchTableEmpty) {
  OverlayHandler handler;
  auto table = handler.emitDispatchTable();
  EXPECT_NE(table.find("No overlays"), std::string::npos);
}

// Config Loading

TEST(OverlayHandler, LoadFromConfig) {
  // 0x10000=65536, 0x80040000=2147745792, 0x8000=32768
  // 0x80040100=2147746048, 0x80040200=2147746304
  // 0x20000=131072, 0x80050000=2147811328, 0x4000=16384
  auto path = createTempConfig(R"(
[[overlays]]
name = "battle"
rom_offset = 65536
ram_base = 2147745792
size = 32768
functions = [2147746048, 2147746304]

[[overlays]]
name = "menu"
rom_offset = 131072
ram_base = 2147811328
size = 16384
)");

  OverlayHandler handler;
  ASSERT_TRUE(handler.loadFromConfig(path));
  EXPECT_EQ(handler.overlayCount(), 2u);

  const auto &overlays = handler.overlays();
  EXPECT_EQ(overlays[0].name, "battle");
  EXPECT_EQ(overlays[0].romOffset, 0x10000u);
  EXPECT_EQ(overlays[0].ramBase, 0x80040000u);
  EXPECT_EQ(overlays[0].size, 0x8000u);
  EXPECT_EQ(overlays[0].functions.size(), 2u);

  EXPECT_EQ(overlays[1].name, "menu");
  EXPECT_EQ(overlays[1].functions.size(), 0u);

  std::filesystem::remove(path);
}

TEST(OverlayHandler, LoadConfigNoOverlays) {
  auto path = createTempConfig(R"(
[game]
name = "test_game"
)");

  OverlayHandler handler;
  ASSERT_TRUE(handler.loadFromConfig(path));
  EXPECT_EQ(handler.overlayCount(), 0u);

  std::filesystem::remove(path);
}

TEST(OverlayHandler, LoadConfigInvalidPath) {
  OverlayHandler handler;
  EXPECT_FALSE(handler.loadFromConfig("/nonexistent/path/config.toml"));
  EXPECT_FALSE(handler.getError().empty());
}
