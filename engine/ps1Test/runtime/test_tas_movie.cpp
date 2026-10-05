#include "runtime/input/tas_movie.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace ps1::input;

namespace {
bool waitForGate(TasMovie &movie) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!movie.parked() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  return movie.parked();
}

class TasMovieTest : public ::testing::Test {
protected:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("mgs-tas-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".mgst");
  void TearDown() override { std::filesystem::remove(path); }
};
} // namespace

TEST_F(TasMovieTest, ReplaysExactSamplesAndTerminalWitnessAtNativeBoundaries) {
  std::string error;
  TasMovie record;
  ASSERT_TRUE(record.open(path, false, error)) << error;
  record.start(100);
  InputController input;
  input.setFrameSampling(true);
  const uint8_t ram[] = {1, 2, 3, 4};
  // Real game/host ownership: only the game thread arrives at checkpoints.
  for (unsigned step = 0; step < 3; ++step) {
    record.requestTick();
    std::thread guest([&] { record.checkpoint(0x80010000 + step * 4); });
    EXPECT_TRUE(waitForGate(record));
    if (step < 2) {
      input.press(step == 0 ? BTN_CROSS : BTN_SELECT, step);
      input.setAnalog(step, 12, 34, 56, 78);
      input.latchVBlank();
      EXPECT_TRUE(record.frame(101 + step, ram, sizeof(ram), input));
      record.release();
    } else {
      EXPECT_TRUE(record.end(102, ram, sizeof(ram)));
      record.finish();
    }
    guest.join();
  }
  TasMovie replay;
  ASSERT_TRUE(replay.open(path, true, error)) << error;
  replay.start(100);
  InputController secondInput;
  secondInput.setFrameSampling(true);
  for (unsigned step = 0; step < 3; ++step) {
    std::thread guest([&] { replay.checkpoint(0x80010000 + step * 4); });
    EXPECT_TRUE(waitForGate(replay));
    if (step < 2) {
      secondInput.press(BTN_START); // must never leak into movie playback
      EXPECT_TRUE(replay.frame(101 + step, ram, sizeof(ram), secondInput));
      const auto pad = secondInput.latchedSample()[step];
      EXPECT_EQ(pad.buttons, uint16_t(~(step == 0 ? BTN_CROSS : BTN_SELECT)));
      EXPECT_EQ(pad.analogRY, 78);
      replay.release();
    } else {
      EXPECT_TRUE(replay.end(102, ram, sizeof(ram)));
      EXPECT_TRUE(replay.done());
      replay.finish();
    }
    guest.join();
  }
  EXPECT_FALSE(replay.failed()) << replay.error();
}

TEST_F(TasMovieTest, RejectsTruncationAndMalformedTimelines) {
  const std::string frame = "F 1 101 10 2147549184 65535 128 128 128 128 65 "
                            "65535 128 128 128 128 65\n";
  for (const auto &contents :
       {"MGSTAS 1\n" + frame,
        "MGSTAS 1\n" + frame + frame + "E 2 3 101 10 2147549184\n",
        "MGSTAS 1\n" + frame + "E 1 1 101 10 2147549184\n",
        "MGSTAS 1\n" + frame + "E 1 2 101 10 2147549184\ntrailing\n"}) {
    std::ofstream(path) << contents;
    TasMovie replay;
    std::string error;
    EXPECT_FALSE(replay.open(path, true, error));
    EXPECT_FALSE(error.empty());
  }
}

TEST_F(TasMovieTest, DesyncDoesNotReleaseGuestBeforeDiagnostics) {
  std::ofstream(path)
      << "MGSTAS 1\nF 1 101 10 2147549184 65535 128 128 128 128 65 65535 128 "
         "128 128 128 65\nE 1 2 101 10 2147549184\n";
  TasMovie replay;
  std::string error;
  ASSERT_TRUE(replay.open(path, true, error)) << error;
  replay.start(100);
  std::atomic<bool> resumed{false};
  std::thread guest([&] {
    replay.checkpoint(0x80010000);
    resumed = true;
  });
  EXPECT_TRUE(waitForGate(replay));
  InputController input;
  const uint8_t wrongRam[] = {3, 4};
  EXPECT_FALSE(replay.frame(101, wrongRam, sizeof(wrongRam), input));
  EXPECT_TRUE(replay.failed());
  EXPECT_EQ(replay.frameCount(), 0);
  EXPECT_FALSE(resumed.load());
  replay.finish();
  guest.join();
  EXPECT_TRUE(resumed.load());
}

TEST_F(TasMovieTest, RefusesOverwriteAndWrongInitialState) {
  std::ofstream(path)
      << "MGSTAS 1\nF 1 101 10 2147549184 65535 128 128 128 128 65 65535 128 "
         "128 128 128 65\nE 1 2 101 10 2147549184\n";
  TasMovie record, replay;
  std::string error;
  EXPECT_FALSE(record.open(path, false, error));
  ASSERT_TRUE(replay.open(path, true, error));
  replay.start(99);
  EXPECT_TRUE(replay.failed());
}

TEST_F(TasMovieTest, ClockCanAdvanceWhileAnotherContinuationCannotBeSaved) {
  TasMovie movie;
  std::string error;
  ASSERT_TRUE(movie.open(path, false, error));
  movie.start(100);
  movie.requestTick();
  std::thread guest([&] { movie.checkpoint(0x80010000, false); });
  EXPECT_TRUE(waitForGate(movie));
  EXPECT_FALSE(movie.canSave());
  InputController input;
  const uint8_t ram[] = {0};
  EXPECT_TRUE(movie.frame(101, ram, sizeof(ram), input));
  EXPECT_EQ(movie.frameCount(), 1u);
  movie.finish(); // deliberately incomplete; the clock must still release
  guest.join();
  EXPECT_TRUE(movie.failed());
}
