#include "runtime/config.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace ps1;

class ConfigTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    tmpPath = std::filesystem::temp_directory_path() / "ps1_test_config.toml";
  }
  void TearDown() override { std::remove(tmpPath.c_str()); }
};

TEST_F(ConfigTest, DefaultValues) {
  ConfigReader reader;
  EXPECT_TRUE(reader.config().game.profile.empty());
  EXPECT_FALSE(reader.config().game.widescreen16x9);
  EXPECT_FALSE(reader.config().game.presentationDepthCorrections);
  EXPECT_EQ(reader.config().game.sceneryCullGuardPixels, 0u);
  EXPECT_EQ(reader.config().video.backend, "vulkan");
  EXPECT_EQ(reader.config().video.windowWidth, 640);
  EXPECT_EQ(reader.config().video.windowHeight, 480);
  EXPECT_FALSE(reader.config().video.fullscreen);
  EXPECT_TRUE(reader.config().video.vsync);
  EXPECT_EQ(reader.config().video.internalResScale, 1);
  EXPECT_FALSE(reader.config().video.pgxp);
  EXPECT_FALSE(reader.config().video.pgxpNearClip);
  EXPECT_FALSE(reader.config().video.trueDepth);
  EXPECT_EQ(reader.config().video.trueDepthMode, "hybrid");
  EXPECT_TRUE(reader.config().video.wipDepthManifest.empty());
  EXPECT_FALSE(reader.config().video.trueColor);
  EXPECT_EQ(reader.config().video.displayArea, "programmed");
  EXPECT_EQ(reader.config().video.displaySignalProfile, "swanstation_no_crop");
  EXPECT_TRUE(reader.config().video.preserveAspect);
  EXPECT_EQ(reader.config().video.maxVblanksPerPresent, 1u);
  EXPECT_EQ(reader.config().audio.sampleRate, 44100);
  EXPECT_EQ(reader.config().audio.volume, 100);
  EXPECT_FALSE(reader.config().audio.mute);
}

TEST_F(ConfigTest, CreateDefault) {
  ASSERT_TRUE(ConfigReader::createDefault(tmpPath));
  EXPECT_TRUE(std::filesystem::exists(tmpPath));

  // Should be loadable
  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));
  EXPECT_EQ(reader.config().input.keyMap["z"], "cross");
  EXPECT_EQ(reader.config().input.keyMap["x"], "circle");
  EXPECT_EQ(reader.config().input.keyMap["return"], "start");
  EXPECT_EQ(reader.config().input.keyMap["backspace"], "select");
  EXPECT_EQ(reader.config().input.keyMap["right_shift"], "select");
}

TEST_F(ConfigTest, LoadAndParse) {
  // Write a test config
  {
    std::ofstream f(tmpPath);
    f << "# Test config\n";
    f << "[game]\n";
    f << "profile = \"crash3_sces_01420\"\n";
    f << "widescreen_16_9 = true\n";
    f << "presentation_depth_corrections = true\n\n";
    f << "scenery_cull_guard_pixels = 192\n\n";
    f << "[video]\n";
    f << "backend = \"vulkan\"\n";
    f << "width = 1280\n";
    f << "height = 720\n";
    f << "fullscreen = true\n";
    f << "vsync = false\n";
    f << "res_scale = 4\n";
    f << "pgxp = true\n";
    f << "pgxp_near_clip = true\n";
    f << "true_depth = true\n\n";
    f << "true_depth_mode = \"strict\"\n";
    f << "wip_depth_manifest = \"evidence/wip.manifest\"\n";
    f << "true_color = true\n";
    f << "display_area = \"nominal\"\n";
    f << "display_signal_profile = \"beetle_uncropped\"\n";
    f << "preserve_aspect = false\n\n";
    f << "max_vblanks_per_present = 3\n\n";
    f << "[audio]\n";
    f << "sample_rate = 48000\n";
    f << "volume = 75\n";
    f << "mute = false\n\n";
    f << "[input]\n";
    f << "z = \"cross\"\n";
    f << "analog = true\n\n";
    f << "[paths]\n";
    f << "disc = \"/games/crash.bin\"\n";
    f << "memcard_dir = \"./saves\"\n";
  }

  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));

  EXPECT_EQ(reader.config().game.profile, "crash3_sces_01420");
  EXPECT_TRUE(reader.config().game.widescreen16x9);
  EXPECT_TRUE(reader.config().game.presentationDepthCorrections);
  EXPECT_EQ(reader.config().game.sceneryCullGuardPixels, 192u);
  EXPECT_EQ(reader.config().video.windowWidth, 1280);
  EXPECT_EQ(reader.config().video.backend, "vulkan");
  EXPECT_EQ(reader.config().video.windowHeight, 720);
  EXPECT_TRUE(reader.config().video.fullscreen);
  EXPECT_FALSE(reader.config().video.vsync);
  EXPECT_EQ(reader.config().video.internalResScale, 4);
  EXPECT_TRUE(reader.config().video.pgxp);
  EXPECT_TRUE(reader.config().video.pgxpNearClip);
  EXPECT_TRUE(reader.config().video.trueDepth);
  EXPECT_EQ(reader.config().video.trueDepthMode, "strict");
  EXPECT_EQ(reader.config().video.wipDepthManifest,
            "evidence/wip.manifest");
  EXPECT_TRUE(reader.config().video.trueColor);
  EXPECT_EQ(reader.config().video.displayArea, "nominal");
  EXPECT_EQ(reader.config().video.displaySignalProfile, "beetle_uncropped");
  EXPECT_FALSE(reader.config().video.preserveAspect);
  EXPECT_EQ(reader.config().video.maxVblanksPerPresent, 3u);
  EXPECT_EQ(reader.config().audio.sampleRate, 48000);
  EXPECT_EQ(reader.config().audio.volume, 75);
  EXPECT_TRUE(reader.config().input.enableAnalog);
  EXPECT_EQ(reader.config().input.keyMap["z"], "cross");
  EXPECT_EQ(reader.config().paths.discImage, "/games/crash.bin");
  EXPECT_EQ(reader.config().paths.memcardDir, "./saves");
}

TEST_F(ConfigTest, SaveAndReload) {
  ConfigReader writer;
  writer.config().game.profile = "crash3_sces_01420";
  writer.config().game.widescreen16x9 = true;
  writer.config().game.presentationDepthCorrections = true;
  writer.config().game.sceneryCullGuardPixels = 256;
  writer.config().video.windowWidth = 1920;
  writer.config().video.backend = "vulkan";
  writer.config().video.internalResScale = 4;
  writer.config().video.windowHeight = 1080;
  writer.config().video.pgxp = true;
  writer.config().video.pgxpNearClip = true;
  writer.config().video.trueDepth = true;
  writer.config().video.trueDepthMode = "strict";
  writer.config().video.wipDepthManifest = "evidence/wip.manifest";
  writer.config().video.trueColor = true;
  writer.config().video.displayArea = "nominal";
  writer.config().video.displaySignalProfile = "beetle_uncropped";
  writer.config().video.preserveAspect = false;
  writer.config().video.maxVblanksPerPresent = 4;
  writer.config().audio.volume = 50;
  writer.config().paths.discImage = "/my/game.iso";

  ASSERT_TRUE(writer.save(tmpPath));

  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));
  EXPECT_EQ(reader.config().game.profile, "crash3_sces_01420");
  EXPECT_TRUE(reader.config().game.widescreen16x9);
  EXPECT_TRUE(reader.config().game.presentationDepthCorrections);
  EXPECT_EQ(reader.config().game.sceneryCullGuardPixels, 256u);
  EXPECT_EQ(reader.config().video.windowWidth, 1920);
  EXPECT_EQ(reader.config().video.backend, "vulkan");
  EXPECT_EQ(reader.config().video.internalResScale, 4);
  EXPECT_EQ(reader.config().video.windowHeight, 1080);
  EXPECT_TRUE(reader.config().video.pgxp);
  EXPECT_TRUE(reader.config().video.pgxpNearClip);
  EXPECT_TRUE(reader.config().video.trueDepth);
  EXPECT_EQ(reader.config().video.trueDepthMode, "strict");
  EXPECT_EQ(reader.config().video.wipDepthManifest,
            "evidence/wip.manifest");
  EXPECT_TRUE(reader.config().video.trueColor);
  EXPECT_EQ(reader.config().video.displayArea, "nominal");
  EXPECT_EQ(reader.config().video.displaySignalProfile, "beetle_uncropped");
  EXPECT_FALSE(reader.config().video.preserveAspect);
  EXPECT_EQ(reader.config().video.maxVblanksPerPresent, 4u);
  EXPECT_EQ(reader.config().audio.volume, 50);
  EXPECT_EQ(reader.config().paths.discImage, "/my/game.iso");
}

TEST_F(ConfigTest, MissingFileReturnsError) {
  ConfigReader reader;
  EXPECT_FALSE(reader.load("/nonexistent/config.toml"));
  EXPECT_FALSE(reader.error().empty());
}

TEST_F(ConfigTest, CommentsAndBlankLinesIgnored) {
  {
    std::ofstream f(tmpPath);
    f << "# Full line comment\n";
    f << "\n";
    f << "  \n";
    f << "[video]\n";
    f << "# Width setting\n";
    f << "width = 800\n";
  }

  ConfigReader reader;
  ASSERT_TRUE(reader.load(tmpPath));
  EXPECT_EQ(reader.config().video.windowWidth, 800);
}
