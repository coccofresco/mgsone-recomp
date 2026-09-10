#include <cstdio>
#include <cstdlib>
#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

namespace {

// Tests run from build/ps1Test/, so the repository root is two levels up.
// The pre-existing pipeline tests use "../" and consequently skip forever --
// do not copy that.
constexpr const char *kRepoRoot = "../../";

std::string atRoot(const char *relative) {
  return std::string(kRepoRoot) + relative;
}

// Counts distinct non-black colours inside the display band (y=12..227), which
// is where the game's visible framebuffer lives.  Pixel count alone cannot tell
// content from an empty screen: a plain clear already covers the whole band, so
// it scores ~221k non-zero pixels while carrying only 4 distinct colours.
// Colour diversity is what separates "something was drawn" from "the screen was
// cleared".  Returns -1 when the file is unreadable or malformed.
long countBandColours(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good())
    return -1;
  std::string magic;
  int w = 0, h = 0, maxval = 0;
  in >> magic >> w >> h >> maxval;
  if (magic != "P6" || w <= 0 || h <= 0)
    return -1;
  in.get();
  std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
  in.read(reinterpret_cast<char *>(px.data()),
          static_cast<std::streamsize>(px.size()));
  if (in.gcount() != static_cast<std::streamsize>(px.size()))
    return -1;
  std::set<std::array<unsigned char, 3>> colours;
  for (int y = 12; y < 228 && y < h; ++y) {
    const size_t base = static_cast<size_t>(y) * w * 3;
    for (int x = 0; x < w; ++x) {
      const size_t i = base + static_cast<size_t>(x) * 3;
      if (px[i] || px[i + 1] || px[i + 2])
        colours.insert({px[i], px[i + 1], px[i + 2]});
    }
  }
  return static_cast<long>(colours.size());
}

// Counts non-zero 16-bit pixels in a P6 PPM produced by dumpVramPpm.
// Returns -1 when the file is unreadable or malformed.
long countNonZeroPixels(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good())
    return -1;
  std::string magic;
  int w = 0, h = 0, maxval = 0;
  in >> magic >> w >> h >> maxval;
  if (magic != "P6" || w <= 0 || h <= 0)
    return -1;
  in.get(); // single whitespace byte before the pixel payload
  std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
  in.read(reinterpret_cast<char *>(px.data()),
          static_cast<std::streamsize>(px.size()));
  if (in.gcount() != static_cast<std::streamsize>(px.size()))
    return -1;
  long n = 0;
  for (size_t i = 0; i + 2 < px.size(); i += 3)
    if (px[i] || px[i + 1] || px[i + 2])
      ++n;
  return n;
}

bool exists(const std::string &path) {
  if (FILE *f = std::fopen(path.c_str(), "r")) {
    std::fclose(f);
    return true;
  }
  return false;
}

// The golden frame contract for Crash bring-up.  Skipped when the game config
// or the runtime binary is absent, matching the skip-if-no-rom pattern in
// ps1Test/pipeline/.
TEST(GoldenFrame, CrashRendersContentAtFrame400) {
  const std::string config = atRoot("configs/crash.toml");
  const std::string runtime = atRoot("build/ps1Runtime/ps1Runtime");
  if (!exists(config))
    GTEST_SKIP() << "configs/crash.toml not found. Skipping.";
  if (!exists(runtime))
    GTEST_SKIP() << "ps1Runtime binary not found. Skipping.";

  const std::string ppm = "/tmp/ps1_golden_frame_test.ppm";
  std::remove(ppm.c_str());

  // Needs a real display. `dummy` has no Vulkan WSI -- the runtime exits
  // before the first frame. `offscreen` has no representative vsync, so the
  // main loop free-runs at ~880fps and frame 200 lands 0.2s into the boot, before
  // anything is drawn; pushing the capture later there is unstable because
  // content presence then races the per-frame clear.  Under a vsynced display
  // frame 200 is 3.3s in and the capture is repeatable.
  if (!std::getenv("WAYLAND_DISPLAY") && !std::getenv("DISPLAY"))
    GTEST_SKIP() << "No display available; golden frame needs vsynced "
                    "rendering. Skipping.";

  // Run from the repository root: the disc paths inside crash.toml are
  // relative to it.
  // PS1_DISPATCH_PERMISSIVE: this test's contract is the render pipeline, not
  // the completeness of the dispatch table. Unmapped calls abort by default so
  // that a missing function is found at its cause rather than swallowed -- but
  // Crash still reaches several of them before frame 200 (the analyzer splits
  // functions and their intra-function branches become dispatches to addresses
  // that were never emitted). Until the boundary analysis is fixed, this test
  // deliberately runs the knowingly-incomplete build, which is exactly what the
  // escape hatch is for. Drop this variable once the game boots clean; if it
  // then still passes, the render contract never depended on the hatch.
  const std::string cmd =
      "cd " + std::string(kRepoRoot) +
      " && SDL_AUDIODRIVER=dummy PS1_DISPATCH_PERMISSIVE=1 "
      // Frame 400, not 200. The old anchor was tuned to the hand-ported GOOL
      // VM's boot timeline; since d04dcd7 the game runs its own interpreter and
      // content arrives later. Measured on the native build: 16 distinct band
      // colours at frame 200, ~988 by frame 400. The threshold below is
      // unchanged -- only the anchor moved, so it still separates a real screen
      // from a cleared one (which scores 4).
      "PS1_VRAM_DUMP_FRAME=400 PS1_VRAM_DUMP_PATH=" +
      ppm +
      " timeout --kill-after=5 25 ./build/ps1Runtime/ps1Runtime "
      "--config configs/crash.toml > /dev/null 2>&1";
  std::system(cmd.c_str());

  const long n = countNonZeroPixels(ppm);
  ASSERT_GE(n, 0) << "VRAM capture missing or malformed at " << ppm;
  EXPECT_GT(n, 0) << "VRAM is entirely black at frame 200";

  // Content check.  A cleared screen scores exactly 4 colours in this band;
  // the Universal Interactive Studios boot screen scored 300..477 across five
  // runs on 2026-07-28, four of them byte-identical at 300.  The bound sits far
  // below that and far above a clear, so it fails on a blank screen without
  // being flaky.
  //
  // That reproducibility depends on PS1_VRAM_DUMP_FRAME counting VBlanks rather
  // than host render-loop iterations.  Anchored on the render loop the same
  // five runs scored 16..119, because the loop's rate follows compositor load
  // while the game advances on a steady 60 Hz -- so "frame 200" landed on a
  // different point of the intro each time.
  const long colours = countBandColours(ppm);
  ASSERT_GE(colours, 0) << "VRAM capture missing or malformed at " << ppm;
  EXPECT_GT(colours, 50)
      << "display band has " << colours
      << " distinct colours -- a cleared screen scores 4, so nothing was drawn";

  std::remove(ppm.c_str());
}

} // namespace
