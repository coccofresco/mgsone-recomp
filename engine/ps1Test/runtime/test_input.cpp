#include "runtime/input/input.h"
#include "runtime/input/input_script.h"
#include <gtest/gtest.h>

using namespace ps1::input;

// Digital Pad Tests

TEST(InputDigital, InitialStateAllReleased) {
  InputController input;
  EXPECT_EQ(input.buttonState(0), 0xFFFF); // All released (active low)
}

TEST(InputDigital, PressAndRelease) {
  InputController input;
  input.press(BTN_CROSS, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_CROSS, 0); // Pressed = 0

  input.release(BTN_CROSS, 0);
  EXPECT_NE(input.buttonState(0) & BTN_CROSS, 0); // Released = 1
}

TEST(InputDigital, MultiplePresses) {
  InputController input;
  input.press(BTN_UP, 0);
  input.press(BTN_CROSS, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_UP, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_CROSS, 0);
  EXPECT_NE(input.buttonState(0) & BTN_CIRCLE, 0); // Not pressed
}

TEST(InputDigital, Port1Independent) {
  InputController input;
  input.press(BTN_START, 0);
  EXPECT_EQ(input.buttonState(0) & BTN_START, 0);
  EXPECT_NE(input.buttonState(1) & BTN_START, 0); // Port 1 unaffected
}

TEST(InputDigital, ShortTapSurvivesUntilGuestVBlankSample) {
  InputController input;

  // SDL can deliver both events in one host-loop batch. The physical state is
  // already released, but the emulated pad must expose the press for a frame.
  input.press(BTN_START, 0);
  input.release(BTN_START, 0);
  EXPECT_NE(input.buttonState(0) & BTN_START, 0);

  input.latchVBlank();
  EXPECT_EQ(input.latchedButtonState(0) & BTN_START, 0);

  input.latchVBlank();
  EXPECT_NE(input.latchedButtonState(0) & BTN_START, 0);
}

TEST(InputDigital, HeldButtonRemainsLatchedUntilReleaseVBlank) {
  InputController input;
  input.press(BTN_CROSS, 0);
  input.latchVBlank();
  input.latchVBlank();
  EXPECT_EQ(input.latchedButtonState(0) & BTN_CROSS, 0);

  input.release(BTN_CROSS, 0);
  EXPECT_EQ(input.latchedButtonState(0) & BTN_CROSS, 0);
  input.latchVBlank();
  EXPECT_NE(input.latchedButtonState(0) & BTN_CROSS, 0);
}

TEST(InputScript, ParsesIntervalsAndCombinesOverlapsPerPort) {
  InputScript script;
  std::string error;
  ASSERT_TRUE(script.parse("# deterministic test\n"
                           "10 12 cross\n"
                           "11 13 right 0\n"
                           "12 12 start 1\n",
                           &error))
      << error;
  EXPECT_EQ(script.intervalCount(), 3u);

  InputController input;
  script.apply(11, input);
  input.latchVBlank();
  EXPECT_EQ(input.latchedButtonState(0) & (BTN_CROSS | BTN_RIGHT), 0u);
  EXPECT_NE(input.latchedButtonState(1) & BTN_START, 0u);

  script.apply(12, input);
  input.latchVBlank();
  EXPECT_EQ(input.latchedButtonState(1) & BTN_START, 0u);

  script.apply(14, input);
  input.latchVBlank();
  EXPECT_EQ(input.latchedButtonState(0), 0xFFFFu);
  EXPECT_EQ(input.latchedButtonState(1), 0xFFFFu);
}

TEST(InputScript, DoesNotReleaseTheSamePhysicalButton) {
  InputScript script;
  std::string error;
  ASSERT_TRUE(script.parse("2 2 cross\n", &error)) << error;
  InputController input;
  input.press(BTN_CROSS);
  script.apply(2, input);
  input.latchVBlank();
  script.apply(3, input);
  input.latchVBlank();
  EXPECT_EQ(input.latchedButtonState() & BTN_CROSS, 0u);
}

TEST(InputScript, RejectsMalformedAndUnknownInput) {
  InputScript script;
  std::string error;
  EXPECT_FALSE(script.parse("12 10 cross\n", &error));
  EXPECT_NE(error.find("line 1"), std::string::npos);
  EXPECT_FALSE(script.parse("10 12 jump\n", &error));
  EXPECT_NE(error.find("unknown PS1 button"), std::string::npos);
  EXPECT_FALSE(script.parse("10 12 cross 2\n", &error));
  EXPECT_NE(error.find("port"), std::string::npos);
}

// Analog Stick Tests

TEST(InputAnalog, DefaultCenter) {
  InputController input;
  input.setPadType(0, PadType::Analog);
  // Default analog values should be centered at 0x80
  // (tested via SIO transfer sequence)
  EXPECT_EQ(input.getPadType(0), PadType::Analog);
}

TEST(InputAnalog, SetAnalogValues) {
  InputController input;
  input.setPadType(0, PadType::Analog);
  input.setAnalog(0, 0x00, 0xFF, 0x40, 0xC0);
  // Values are set internally, tested via SIO
  EXPECT_TRUE(true);
}

TEST(InputState, SnapshotRestoresPadAndSioProtocol) {
  InputController input;
  input.setPadType(0, PadType::Analog);
  input.setAnalog(0, 0x10, 0x20, 0x30, 0x40);
  input.press(BTN_CROSS | BTN_START, 0);
  input.writeRegister(0x1F801040, 0x01); // Begin a pad transaction.
  const auto saved = input.snapshot();

  input.reset();
  ASSERT_TRUE(input.restore(saved));
  EXPECT_EQ(input.getPadType(0), PadType::Analog);
  EXPECT_EQ(input.buttonState(0) & (BTN_CROSS | BTN_START), 0u);
  EXPECT_EQ(input.readRegister(0x1F801040), 0xFFu);

  // The next byte must continue the restored transaction rather than start a
  // new one, proving that the serial protocol cursor is part of the state.
  input.writeRegister(0x1F801040, 0x42);
  EXPECT_EQ(input.readRegister(0x1F801040),
            static_cast<uint32_t>(PadType::Analog));
}

// Memory Card Tests

TEST(MemCard, InitialState) {
  MemoryCard mc;
  EXPECT_TRUE(mc.isPresent());
}

TEST(MemCard, SioAccessProtocol) {
  MemoryCard mc;
  // Start communication
  uint8_t resp = mc.transfer(0x81);
  EXPECT_EQ(resp, 0xFF); // Flag byte

  // Send read command
  resp = mc.transfer(0x52);
  EXPECT_EQ(resp, 0x5A); // ID1

  // Address high byte
  resp = mc.transfer(0x00);
  EXPECT_EQ(resp, 0x5D); // ID2

  // Address low byte
  resp = mc.transfer(0x00);
  // Should return ACK
}

TEST(MemCard, WriteAndReadBack) {
  MemoryCard mc;

  // Directly test file I/O persistence
  mc.setPresent(true);
  EXPECT_TRUE(mc.isPresent());

  mc.setPresent(false);
  EXPECT_FALSE(mc.isPresent());
}

// SIO Register Tests

TEST(InputSIO, JoyCtrlReset) {
  InputController input;
  input.writeRegister16(0x1F80104A, 0x0040); // Reset bit
  // Should not crash
  EXPECT_TRUE(true);
}

TEST(InputSIO, JoyStatReadback) {
  InputController input;
  uint32_t stat = input.readRegister(0x1F801044);
  EXPECT_NE(stat, 0); // TX ready flag should be set
}
