#pragma once
#include "runtime/input/input.h"
#include <array>
#include <cstdint>

namespace ps1::input {
// Physical host state is intentionally outside guest snapshots. Combining
// sources before publishing prevents a stick release from releasing a held
// D-pad/key, and makes reconnect/port changes release every stale button.
class HostInput {
public:
  int port = 0;
  uint16_t keyboard = 0, gamepad = 0;
  int deadzone = 12000;
  std::array<int16_t, 6> axes{}; // SDL axis order: LX LY RX RY LT RT
  static void button(uint16_t &mask, uint16_t bit, bool down) {
    if (down) mask |= bit; else mask &= ~bit;
  }
  void clear() { keyboard = gamepad = 0; axes.fill(0); }
  void disconnect() { gamepad = 0; axes.fill(0); }
  uint16_t buttons() const {
    uint16_t mask = keyboard | gamepad;
    const int threshold = deadzone;
    if (axes[0] < -threshold) mask |= BTN_LEFT;
    if (axes[0] > threshold) mask |= BTN_RIGHT;
    if (axes[1] < -threshold) mask |= BTN_UP;
    if (axes[1] > threshold) mask |= BTN_DOWN;
    if (axes[4] >= 16000) mask |= BTN_L2;
    if (axes[5] >= 16000) mask |= BTN_R2;
    return mask;
  }
  void apply(InputController &input, bool suppress = false) const {
    for (int p = 0; p < 2; ++p) {
      const auto type = p == port ? PadType::Digital : PadType::None;
      if (input.getPadType(p) != type) input.setPadType(p, type);
      input.release(0xffffu, p);
      if (p == port && !suppress) input.press(buttons(), p);
      input.setAnalog(p, 128, 128, 128, 128);
    }
  }
};
}
