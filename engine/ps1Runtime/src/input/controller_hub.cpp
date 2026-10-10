#include "runtime/input/controller_hub.h"
#include "runtime/input/input.h"
#include <toml.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace ps1::input {
void ControllerHub::configureBeforeSDL() {
  // Position-based PS1 controls: South = Cross on Xbox, Sony and Nintendo.
  // Do not touch Steam's ignore/allow lists, HID ownership or SteamAppId.
  SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0", SDL_HINT_OVERRIDE);
}
int ControllerHub::loadMappings(const std::filesystem::path &path) {
  return SDL_GameControllerAddMappingsFromFile(path.string().c_str());
}
bool ControllerHub::isSteamVirtual(uint16_t vendor, uint16_t product) {
  // Valve's virtual gamepad identity. A physical Steam Deck/Controller is not
  // classified just because it has Valve's vendor ID or contains "Steam".
  return vendor == 0x28de && product == 0x11ff;
}
uint16_t ControllerHub::ps1Button(SDL_GameControllerButton button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return BTN_CROSS;
  case SDL_CONTROLLER_BUTTON_B: return BTN_CIRCLE;
  case SDL_CONTROLLER_BUTTON_X: return BTN_SQUARE;
  case SDL_CONTROLLER_BUTTON_Y: return BTN_TRIANGLE;
  case SDL_CONTROLLER_BUTTON_BACK: return BTN_SELECT;
  case SDL_CONTROLLER_BUTTON_START: return BTN_START;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return BTN_L3;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return BTN_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return BTN_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
  default: return 0; // Guide belongs to Steam; R3 belongs to the host menu.
  }
}
ControllerHub::~ControllerHub() { close(); }
void ControllerHub::initialize(const std::filesystem::path &preferences) {
  preferredGuid_.clear(); error_.clear(); deadzone_=12000; glyphs_=0; mappingsLoaded_=0;
  preferences_ = preferences;
  if (preferences_.empty()) {
    if (const char *p = std::getenv("MGS_CONTROLLER_CONFIG")) preferences_ = p;
  }
  std::filesystem::path bundled;
  if (char *base = SDL_GetBasePath()) { bundled = std::filesystem::path(base)/"gamecontrollerdb.txt"; SDL_free(base); }
  if (std::filesystem::is_regular_file(bundled)) mappingsLoaded_ = loadMappings(bundled);
  else error_ = "Bundled controller database missing; using SDL built-in mappings.";
  // SDL's environment mappings have the last word over our bundled database.
  if (const char *p = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE); p && *p) loadMappings(p);
  if (const char *p = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG); p && *p) {
    if (auto *rw = SDL_RWFromConstMem(p, static_cast<int>(std::strlen(p)))) SDL_GameControllerAddMappingsFromRW(rw, 1);
  }
  try {
    if (!preferences_.empty() && std::filesystem::exists(preferences_)) {
      const auto v = toml::parse(preferences_.string());
      const auto guid = toml::find<std::string>(v, "preferred_guid");
      const auto deadzone = toml::find<int>(v, "deadzone");
      const auto glyphs = toml::find<int>(v, "glyphs");
      if (toml::find<int>(v, "schema") != 1 || deadzone < 4000 || deadzone > 24000 || glyphs < 0 || glyphs > 3 ||
          (!guid.empty() && (guid.size()!=32 || guid.find_first_not_of("0123456789abcdef")!=std::string::npos)))
        throw std::runtime_error("Invalid controller preferences; defaults retained.");
      preferredGuid_ = guid; deadzone_ = deadzone; glyphs_ = glyphs;
    }
  } catch (const std::exception &e) { error_ = e.what(); }
  refresh();
}
void ControllerHub::event(const SDL_Event &e) {
  if (e.type==SDL_CONTROLLERDEVICEADDED || e.type==SDL_CONTROLLERDEVICEREMOVED || e.type==SDL_CONTROLLERDEVICEREMAPPED ||
      e.type==SDL_JOYDEVICEADDED || e.type==SDL_JOYDEVICEREMOVED) dirty_ = true;
  if (e.type==SDL_CONTROLLERDEVICEREMAPPED && e.cdevice.which==activeId_) requireNeutral();
}
bool ControllerHub::refresh() {
  if (!dirty_ && (!active() || SDL_GameControllerGetAttached(active()))) return false;
  dirty_ = false;
  const auto previous = activeId_;
  for (auto it=devices_.begin(); it!=devices_.end();) {
    if (!SDL_GameControllerGetAttached(it->handle)) {
      SDL_GameControllerClose(it->handle); it=devices_.erase(it);
    } else ++it;
  }
  for (int index=0; index<SDL_NumJoysticks(); ++index) {
    const auto id=SDL_JoystickGetDeviceInstanceID(index);
    if (!SDL_IsGameController(index) || std::any_of(devices_.begin(),devices_.end(),[&](auto &d){return d.id==id;})) continue;
    if (auto *pad=SDL_GameControllerOpen(index)) {
      char guid[33]{}; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)),guid,sizeof(guid));
      const char *name=SDL_GameControllerName(pad);
      // SDL may translate the vendor/product getters to the physical device
      // behind Steam Input. The raw joystick GUID retains the virtual VID/PID.
      Uint16 rawVendor=0,rawProduct=0;
      SDL_GetJoystickGUIDInfo(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)),&rawVendor,&rawProduct,nullptr,nullptr);
      devices_.push_back({id,pad,name?name:"Gamepad",guid,isSteamVirtual(rawVendor,rawProduct)});
    }
  }
  auto chosen=devices_.end();
  const auto choose=[&](auto pred) {
    // Stable instance when two identical devices match; never mix their input.
    auto current=std::find_if(devices_.begin(),devices_.end(),[&](auto &d){return d.id==previous && pred(d);});
    return current!=devices_.end()?current:std::find_if(devices_.begin(),devices_.end(),pred);
  };
  if (!preferredGuid_.empty()) chosen=choose([&](auto &d){return d.guid==preferredGuid_;});
  if (chosen==devices_.end()) chosen=choose([](auto &d){return d.steamVirtual;});
  if (chosen==devices_.end()) chosen=choose([](auto &){return true;});
  activeId_=chosen==devices_.end()?-1:chosen->id;
  if (previous!=activeId_) {
    requireNeutral();
    std::cout<<"[Controller] active="<<name()<<" instance="<<activeId_<<" mode="
             <<(chosen!=devices_.end()&&chosen->steamVirtual?"Steam virtual":"SDL gamepad")<<'\n';
  }
  return previous!=activeId_;
}
const ControllerHub::Device *ControllerHub::selected() const {
  auto it=std::find_if(devices_.begin(),devices_.end(),[&](auto &d){return d.id==activeId_;});
  return it==devices_.end()?nullptr:&*it;
}
SDL_GameController *ControllerHub::active() const { const auto *d=selected(); return d?d->handle:nullptr; }
std::string ControllerHub::name() const { const auto *d=selected(); return d?d->name:"No controller connected"; }
void ControllerHub::select(SDL_JoystickID id) {
  auto it=std::find_if(devices_.begin(),devices_.end(),[&](auto &d){return d.id==id;});
  if (it==devices_.end()) return;
  activeId_=id; preferredGuid_=it->guid; requireNeutral(); save();
}
void ControllerHub::automatic() { preferredGuid_.clear(); dirty_=true; refresh(); requireNeutral(); save(); }
void ControllerHub::setDeadzone(int value) { deadzone_=std::clamp(value,4000,24000); requireNeutral(); save(); }
void ControllerHub::setGlyphs(int value) { glyphs_=std::clamp(value,0,3); save(); }
void ControllerHub::save() {
  if (preferences_.empty()) return;
  try {
    std::filesystem::create_directories(preferences_.parent_path());
    toml::value v(toml::table{}); v["schema"]=1;v["preferred_guid"]=preferredGuid_;v["deadzone"]=deadzone_;v["glyphs"]=glyphs_;
    auto tmp=preferences_;tmp += ".tmp";
    {std::ofstream out(tmp);out<<toml::format(v);out.flush();if (!out) throw std::runtime_error("Cannot save controller preferences.");}
    std::filesystem::rename(tmp,preferences_);
  } catch (const std::exception &e) { error_=e.what(); }
}
const char *ControllerHub::confirmLabel() const {
  const auto type=active()?SDL_GameControllerGetType(active()):SDL_CONTROLLER_TYPE_UNKNOWN;
  if (glyphs_==2 || (glyphs_==0 && (type==SDL_CONTROLLER_TYPE_PS3 || type==SDL_CONTROLLER_TYPE_PS4 || type==SDL_CONTROLLER_TYPE_PS5))) return "Cross (bottom)";
  if (glyphs_==3 || (glyphs_==0 && type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO)) return "B (bottom)";
  return "A (bottom)";
}
const char *ControllerHub::cancelLabel() const {
  const auto label=std::string(confirmLabel());
  return label.starts_with("Cross")?"Circle (right)":label.starts_with("B")?"A (right)":"B (right)";
}
ControllerHub::Sample ControllerHub::sample() const {
  Sample result;
  auto *pad=active(); if (!pad || !SDL_GameControllerGetAttached(pad)) return result;
  for (int i=0;i<SDL_CONTROLLER_BUTTON_MAX;++i)
    if (SDL_GameControllerGetButton(pad,static_cast<SDL_GameControllerButton>(i))) result.buttons |= ps1Button(static_cast<SDL_GameControllerButton>(i));
  for (int i=0;i<6;++i) result.axes[i]=SDL_GameControllerGetAxis(pad,static_cast<SDL_GameControllerAxis>(i));
  result.menu=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSTICK)!=0;
  return result;
}
ControllerHub::Sample ControllerHub::gameplay(bool suppressed) {
  auto result=sample();
  if (suppressed) requireNeutral();
  if (awaitingNeutral_) {
    const bool centered=std::abs(int(result.axes[0]))<deadzone_ && std::abs(int(result.axes[1]))<deadzone_ &&
        result.axes[4]<16000 && result.axes[5]<16000;
    if (!suppressed && !result.buttons && !result.menu && centered) awaitingNeutral_=false;
    return {};
  }
  return result;
}
void ControllerHub::close() {
  if (SDL_WasInit(SDL_INIT_GAMECONTROLLER))
    for (auto &d:devices_) { SDL_GameControllerRumble(d.handle,0,0,0); SDL_GameControllerClose(d.handle); }
  devices_.clear(); activeId_=-1; dirty_=true; requireNeutral();
}
}
