#pragma once
#include <SDL.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps1::input {
// One selected SDL controller feeds both gameplay and host UI. The hub owns
// handles; callers must clear ImGui's manual handle list before shutdown.
class ControllerHub {
public:
  struct Device {
    SDL_JoystickID id = -1;
    SDL_GameController *handle = nullptr;
    std::string name, guid;
    bool steamVirtual = false;
  };
  struct Sample {
    uint16_t buttons = 0;
    std::array<int16_t, 6> axes{};
    bool menu = false;
  };
  static void configureBeforeSDL();
  static int loadMappings(const std::filesystem::path &path);
  static uint16_t ps1Button(SDL_GameControllerButton button);
  static bool isSteamVirtual(uint16_t vendor, uint16_t product);
  ControllerHub() = default;
  ~ControllerHub();
  ControllerHub(const ControllerHub &) = delete;
  ControllerHub &operator=(const ControllerHub &) = delete;
  void initialize(const std::filesystem::path &preferences = {});
  void event(const SDL_Event &event);
  bool refresh(); // returns true when the selected instance changes
  void select(SDL_JoystickID id); // explicit selection persists by GUID
  void automatic();
  void close();
  SDL_GameController *active() const;
  SDL_JoystickID activeId() const { return activeId_; }
  const std::vector<Device> &devices() const { return devices_; }
  std::string name() const;
  const char *confirmLabel() const;
  const char *cancelLabel() const;
  Sample sample() const;
  Sample gameplay(bool suppressed);
  void requireNeutral() { awaitingNeutral_ = true; }
  bool awaitingNeutral() const { return awaitingNeutral_; }
  int deadzone() const { return deadzone_; }
  void setDeadzone(int value);
  int glyphs() const { return glyphs_; }
  void setGlyphs(int value);
  bool prefersAutomatic() const { return preferredGuid_.empty(); }
  int mappingsLoaded() const { return mappingsLoaded_; }
  const std::string &error() const { return error_; }
private:
  const Device *selected() const;
  void save();
  std::vector<Device> devices_;
  SDL_JoystickID activeId_ = -1;
  bool dirty_ = true, awaitingNeutral_ = true;
  std::string preferredGuid_, error_;
  std::filesystem::path preferences_;
  int deadzone_ = 12000, glyphs_ = 0, mappingsLoaded_ = 0;
};
}
