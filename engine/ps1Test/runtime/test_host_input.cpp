#include "runtime/input/host_input.h"
#include <gtest/gtest.h>
using namespace ps1::input;

TEST(HostInputTest, StickAndDpadReleaseCannotCancelHeldKeyboard) {
  HostInput host;
  host.keyboard = BTN_LEFT;
  host.gamepad = BTN_LEFT | BTN_CROSS;
  host.axes[0] = -30000;
  EXPECT_EQ(host.buttons(), BTN_LEFT | BTN_CROSS);
  host.gamepad = 0; host.axes[0] = 0;
  EXPECT_EQ(host.buttons(), BTN_LEFT);
  host.keyboard = 0; host.axes[0] = 11999;
  EXPECT_EQ(host.buttons(), 0);
}

TEST(HostInputTest, SwitchingOriginalPortsDisconnectsOldPadButNotCards) {
  InputController input;
  HostInput host;
  host.keyboard = BTN_CROSS;
  host.apply(input); input.latchVBlank();
  EXPECT_EQ(input.getPadType(0), PadType::Digital);
  EXPECT_EQ(input.getPadType(1), PadType::None);
  EXPECT_EQ(input.buttonState(0) & BTN_CROSS, 0); // guest active-low
  host.port = 1; host.apply(input); input.latchVBlank();
  EXPECT_EQ(input.getPadType(0), PadType::None);
  EXPECT_EQ(input.getPadType(1), PadType::Digital);
  EXPECT_NE(input.buttonState(0) & BTN_CROSS, 0);
  EXPECT_EQ(input.buttonState(1) & BTN_CROSS, 0);
  // The route touches only pad type/live inputs; memory-card protocol remains
  // independent (its persistence is covered by the dedicated card tests).
}

TEST(HostInputTest, DisconnectReleasesAxesTriggersAndButtons) {
  HostInput host;
  host.gamepad = BTN_CIRCLE;
  host.axes = {32000, -32000, 0, 0, 32000, 32000};
  host.keyboard = BTN_SQUARE;
  EXPECT_EQ(host.buttons(), BTN_CIRCLE | BTN_RIGHT | BTN_UP | BTN_L2 | BTN_R2 | BTN_SQUARE);
  host.disconnect();
  EXPECT_EQ(host.buttons(), BTN_SQUARE);
  InputController input;
  host.apply(input, true);
  EXPECT_EQ(input.buttonState(), 0xffff);
  host.clear(); host.apply(input);
  EXPECT_EQ(input.buttonState(), 0xffff);
}
