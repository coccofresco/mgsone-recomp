#include "runtime/input/controller_hub.h"
#include "runtime/input/host_input.h"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <chrono>
#include <fstream>
#include <sstream>

using namespace ps1::input;
namespace fs=std::filesystem;
class ControllerHubTest : public ::testing::Test {
protected:
  ControllerHub hub;
  std::vector<SDL_Joystick*> joysticks;
  fs::path directory;
  void SetUp() override {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_setenv("MGS_CONTROLLER_CONFIG","",1);
    ControllerHub::configureBeforeSDL();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1"); // virtual test driver only
    ASSERT_EQ(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER),0)<<SDL_GetError();
    directory=fs::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directory(directory);
  }
  void TearDown() override {
    hub.close();
    for (auto *j:joysticks) SDL_JoystickClose(j);
    for (int i=SDL_NumJoysticks()-1;i>=0;--i) if (SDL_JoystickIsVirtual(i)) SDL_JoystickDetachVirtual(i);
    SDL_Quit(); SDL_ClearHints(); fs::remove_all(directory);
  }
  SDL_Joystick *attach(uint16_t vendor=0x045e,uint16_t product=0x028e,const char *name="Virtual Xbox") {
    SDL_VirtualJoystickDesc desc{};desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;desc.naxes=6;desc.nbuttons=21;desc.nhats=1;
    desc.vendor_id=vendor;desc.product_id=product;desc.name=name;
    desc.button_mask=(1u<<SDL_CONTROLLER_BUTTON_MAX)-1;desc.axis_mask=63;
    const int index=SDL_JoystickAttachVirtualEx(&desc);
    EXPECT_GE(index,0)<<SDL_GetError(); if (index<0) return nullptr;
    auto *j=SDL_JoystickOpen(index);EXPECT_NE(j,nullptr);joysticks.push_back(j);
    SDL_JoystickSetVirtualAxis(j,4,-32768);SDL_JoystickSetVirtualAxis(j,5,-32768);
    return j;
  }
  void pump() {
    SDL_GameControllerUpdate();SDL_Event e;
    while (SDL_PollEvent(&e)) hub.event(e);
    hub.refresh();
  }
  void start() { hub.initialize(directory/"controllers.toml");pump();hub.gameplay(false); }
  void button(SDL_Joystick *j,int b,bool down) { ASSERT_EQ(SDL_JoystickSetVirtualButton(j,b,down),0);pump(); }
  void detach(SDL_Joystick *j) {
    const auto id=SDL_JoystickInstanceID(j);
    for(int i=0;i<SDL_NumJoysticks();++i) if(SDL_JoystickGetDeviceInstanceID(i)==id){ ASSERT_EQ(SDL_JoystickDetachVirtual(i),0);break; }
    pump();
  }
};

TEST_F(ControllerHubTest, SteamVirtualWinsAndPhysicalInputsAreNotMerged) {
  auto *physical=attach();ASSERT_NE(physical,nullptr);start();
  auto *steam=attach(0x28de,0x11ff,"Steam Virtual Gamepad");ASSERT_NE(steam,nullptr);pump();
  EXPECT_EQ(hub.activeId(),SDL_JoystickInstanceID(steam));
  button(physical,SDL_CONTROLLER_BUTTON_A,true);
  EXPECT_EQ(hub.sample().buttons,0);
  button(steam,SDL_CONTROLLER_BUTTON_B,true);
  EXPECT_EQ(hub.sample().buttons,BTN_CIRCLE);
  EXPECT_FALSE(ControllerHub::isSteamVirtual(0x28de,0x1205)); // physical Deck
  EXPECT_FALSE(ControllerHub::isSteamVirtual(0x045e,0x028e)); // real Xbox
}

TEST_F(ControllerHubTest, ExplicitChoicePersistsAndSurvivesIndexRenumbering) {
  auto *first=attach();auto *second=attach(0x054c,0x0ce6,"Virtual DualSense");start();
  hub.select(SDL_JoystickInstanceID(second));hub.setDeadzone(9000);hub.setGlyphs(2);
  hub.close();
  EXPECT_EQ(SDL_GameControllerFromInstanceID(SDL_JoystickInstanceID(first)),nullptr);
  EXPECT_EQ(SDL_GameControllerFromInstanceID(SDL_JoystickInstanceID(second)),nullptr);
  hub.initialize(directory/"controllers.toml");pump();
  EXPECT_EQ(hub.activeId(),SDL_JoystickInstanceID(second));EXPECT_EQ(hub.deadzone(),9000);
  EXPECT_STREQ(hub.confirmLabel(),"Cross (bottom)");
  detach(first);EXPECT_EQ(hub.activeId(),SDL_JoystickInstanceID(second));
  button(second,SDL_CONTROLLER_BUTTON_A,true);EXPECT_EQ(hub.sample().buttons,BTN_CROSS);
}

TEST_F(ControllerHubTest, DisconnectAndReconnectWithHeldButtonWaitForRelease) {
  auto *first=attach();start();button(first,SDL_CONTROLLER_BUTTON_A,true);
  EXPECT_EQ(hub.gameplay(false).buttons,BTN_CROSS);
  detach(first);EXPECT_EQ(hub.activeId(),-1);EXPECT_EQ(hub.gameplay(false).buttons,0);
  auto *next=attach();button(next,SDL_CONTROLLER_BUTTON_B,true);
  EXPECT_EQ(hub.gameplay(false).buttons,0);EXPECT_TRUE(hub.awaitingNeutral());
  button(next,SDL_CONTROLLER_BUTTON_B,false);hub.gameplay(false);EXPECT_FALSE(hub.awaitingNeutral());
  button(next,SDL_CONTROLLER_BUTTON_B,true);EXPECT_EQ(hub.gameplay(false).buttons,BTN_CIRCLE);
}

TEST_F(ControllerHubTest, PauseAndFocusSuppressionDoNotLeakMenuConfirmIntoGame) {
  auto *j=attach();start();button(j,SDL_CONTROLLER_BUTTON_A,true);
  EXPECT_EQ(hub.gameplay(true).buttons,0);EXPECT_EQ(hub.gameplay(false).buttons,0);
  button(j,SDL_CONTROLLER_BUTTON_A,false);hub.gameplay(false);
  button(j,SDL_CONTROLLER_BUTTON_A,true);EXPECT_EQ(hub.gameplay(false).buttons,BTN_CROSS);
}

TEST_F(ControllerHubTest, RuntimeRemapWaitsForReleaseAndUsesTheNewBinding) {
  auto *j=attach();start();button(j,SDL_CONTROLLER_BUTTON_A,true);
  char guid[33];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j),guid,sizeof(guid));
  const std::string mapping=std::string(guid)+",Remapped,a:b1,b:b0,platform:Linux,";
  ASSERT_GE(SDL_GameControllerAddMapping(mapping.c_str()),0);pump();
  EXPECT_EQ(hub.sample().buttons,BTN_CIRCLE);EXPECT_EQ(hub.gameplay(false).buttons,0);
  button(j,SDL_CONTROLLER_BUTTON_A,false);hub.gameplay(false);
  button(j,SDL_CONTROLLER_BUTTON_A,true);EXPECT_EQ(hub.gameplay(false).buttons,BTN_CIRCLE);
}

TEST_F(ControllerHubTest, AllDigitalButtonsStickAndTriggersReachSelectedOriginalPort) {
  auto *j=attach();start();InputController input;HostInput host;host.port=1;
  const std::array<uint16_t,15> expected{BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,
      BTN_SELECT,0,BTN_START,BTN_L3,0,BTN_L1,BTN_R1,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT};
  for(int b=0;b<15;++b) {
    button(j,b,true);auto pad=hub.sample();host.gamepad=pad.buttons;host.axes=pad.axes;
    EXPECT_EQ(host.buttons(),expected[b])<<b;host.apply(input);input.latchVBlank();
    EXPECT_EQ(uint16_t(~input.buttonState(1)),expected[b]);EXPECT_EQ(input.buttonState(0),0xffff);
    EXPECT_EQ(pad.menu,b==SDL_CONTROLLER_BUTTON_RIGHTSTICK);button(j,b,false);
  }
  SDL_JoystickSetVirtualAxis(j,0,-18000);SDL_JoystickSetVirtualAxis(j,1,18000);pump();
  host.axes=hub.sample().axes;host.gamepad=0;host.keyboard=BTN_CROSS;
  EXPECT_EQ(host.buttons(),BTN_LEFT|BTN_DOWN|BTN_CROSS);
  host.deadzone=20000;EXPECT_EQ(host.buttons(),BTN_CROSS);
}

TEST_F(ControllerHubTest, ImGuiAndGuestReadExactlyTheSameSelectedController) {
  auto *first=attach();auto *second=attach(0x054c,0x0ce6,"Virtual DualSense");start();
  hub.select(SDL_JoystickInstanceID(second));
  auto *window=SDL_CreateWindow("input test",0,0,640,480,0);ASSERT_NE(window,nullptr);
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
  io.ConfigFlags|=ImGuiConfigFlags_NavEnableGamepad;
  unsigned char *pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
  ASSERT_TRUE(ImGui_ImplSDL2_InitForOther(window));
  auto frame=[&] {
    auto *pad=hub.active();ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,pad?&pad:nullptr,pad?1:0);
    ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();
  };
  button(first,SDL_CONTROLLER_BUTTON_A,true);frame();
  EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));EXPECT_EQ(hub.sample().buttons,0);ImGui::EndFrame();
  button(second,SDL_CONTROLLER_BUTTON_A,true);frame();
  EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));EXPECT_EQ(hub.sample().buttons,BTN_CROSS);ImGui::EndFrame();
  button(second,SDL_CONTROLLER_BUTTON_A,false);frame();ImGui::EndFrame();
  detach(second);frame();
  EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));EXPECT_EQ(hub.sample().buttons,BTN_CROSS);ImGui::EndFrame();
  ImGui_ImplSDL2_Shutdown();ImGui::DestroyContext();SDL_DestroyWindow(window);
}

TEST_F(ControllerHubTest, SteamEnvironmentIsNotOverwrittenAndHomeRemainsReserved) {
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES,"0x1111/0x2222");
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x045e/0x028e");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM,"0");
  ControllerHub::configureBeforeSDL();
  EXPECT_STREQ(SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES),"0x1111/0x2222");
  EXPECT_STREQ(SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT),"0x045e/0x028e");
  EXPECT_STREQ(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM),"0");
  EXPECT_EQ(ControllerHub::ps1Button(SDL_CONTROLLER_BUTTON_GUIDE),0);
  EXPECT_EQ(ControllerHub::ps1Button(SDL_CONTROLLER_BUTTON_RIGHTSTICK),0);
}

TEST_F(ControllerHubTest, InvalidPreferencesDoNotPartiallyApplyOrReplaceTheFile) {
  const auto path=directory/"controllers.toml";
  const std::string invalid="schema=1\npreferred_guid='../../bad'\ndeadzone=99999\nglyphs=2\n";
  {std::ofstream f(path);f<<invalid;}
  hub.initialize(path);EXPECT_FALSE(hub.error().empty());EXPECT_EQ(hub.deadzone(),12000);EXPECT_EQ(hub.glyphs(),0);
  std::ifstream f(path);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(f),{}),invalid);
}

struct MappingCase {
  const char *name,*guid;
  uint16_t vendor,product;
  int south,east,west,north,leftTrigger,rightTrigger; // negative = digital button (-index-1)
};
class CommonControllerMappingTest : public ControllerHubTest, public ::testing::WithParamInterface<MappingCase> {};
TEST_P(CommonControllerMappingTest, RealDatabaseMappingFeedsOriginalPS1ButtonsAndTriggers) {
  const auto c=GetParam();
  auto *j=attach(c.vendor,c.product,c.name);ASSERT_NE(j,nullptr);
  // Transport/firmware hardware is not emulated: use the real DB mapping with
  // only its GUID replaced by this SDL virtual device's runtime GUID.
  std::ifstream db(MGS_CONTROLLER_DB);std::string line,record;
  while(std::getline(db,line)) if(line.starts_with(c.guid)){record=line;break;}
  ASSERT_FALSE(record.empty());
  char guid[33];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j),guid,sizeof(guid));
  record.replace(0,32,guid);ASSERT_GE(SDL_GameControllerAddMapping(record.c_str()),0)<<SDL_GetError();
  for(int i=0;i<6;++i) SDL_JoystickSetVirtualAxis(j,i,0);
  for(int axis:{c.leftTrigger,c.rightTrigger}) if(axis>=0) SDL_JoystickSetVirtualAxis(j,axis,-32768);
  start();
  for(auto [raw,expected]:std::array<std::pair<int,uint16_t>,4>{{{c.south,BTN_CROSS},{c.east,BTN_CIRCLE},{c.west,BTN_SQUARE},{c.north,BTN_TRIANGLE}}}) {
    button(j,raw,true);EXPECT_EQ(hub.sample().buttons,expected)<<c.name;
    button(j,raw,false);EXPECT_EQ(hub.sample().buttons,0);
  }
  auto setTrigger=[&](int binding,bool pressed) {
    if(binding>=0) SDL_JoystickSetVirtualAxis(j,binding,pressed?32767:-32768);
    else SDL_JoystickSetVirtualButton(j,-binding-1,pressed);
    pump();
  };
  InputController input;HostInput host;
  for(auto [binding,bit]:std::array<std::pair<int,uint16_t>,2>{{{c.leftTrigger,BTN_L2},{c.rightTrigger,BTN_R2}}}) {
    setTrigger(binding,true);const auto pad=hub.sample();host.gamepad=pad.buttons;host.axes=pad.axes;
    EXPECT_EQ(host.buttons(),bit);host.port=1;host.apply(input);input.latchVBlank();
    EXPECT_EQ(input.buttonState(1)&bit,0);EXPECT_EQ(input.getPadType(0),PadType::None);
    setTrigger(binding,false);
  }
}
INSTANTIATE_TEST_SUITE_P(LinuxDatabase,CommonControllerMappingTest,::testing::Values(
  MappingCase{"Xbox360USB","030000005e0400008e02000000010000",0x045e,0x028e,0,1,2,3,2,5},
  MappingCase{"XboxOneUSB","030000005e040000d102000001010000",0x045e,0x02d1,0,1,2,3,2,5},
  MappingCase{"XboxSeriesUSB","030000005e040000120b00000b050000",0x045e,0x0b12,0,1,2,3,2,5},
  MappingCase{"DualShock4USB","030000004c050000c405000011010000",0x054c,0x05c4,1,2,0,3,3,4},
  MappingCase{"DualShock4BT","050000004c050000cc09000000010000",0x054c,0x09cc,1,2,0,3,3,4},
  MappingCase{"DualSenseUSB","030000004c050000e60c000011010000",0x054c,0x0ce6,1,2,0,3,3,4},
  MappingCase{"DualSenseBT","050000004c050000e60c000000010000",0x054c,0x0ce6,1,2,0,3,3,4},
  MappingCase{"SwitchProBT","050000007e0500000920000001000000",0x057e,0x2009,0,1,2,3,-7,-8},
  MappingCase{"SwitchProUSB","030000007e0500000920000000026803",0x057e,0x2009,1,0,3,2,4,5},
  MappingCase{"EightBitDoPro2BT","05000000c82d00000660000000010000",0x2dc8,0x6006,1,0,4,3,5,4}
),[](const auto &p){return p.param.name;});

// Windows HID/DirectInput GUIDs and bindings differ from Linux. Test their
// actual DB rows as well, with only transport enumeration replaced by SDL's
// virtual joystick. XInput's built-in mapping uses the standard SDL layout
// already exercised by ControllerHubTest.
INSTANTIATE_TEST_SUITE_P(WindowsDatabase,CommonControllerMappingTest,::testing::Values(
  MappingCase{"Xbox360Wireless","030000005e0400001907000000000000",0x045e,0x0719,0,1,2,3,2,5},
  MappingCase{"DualShock4","030000004c050000c405000000000000",0x054c,0x05c4,1,2,0,3,3,4},
  MappingCase{"DualShock4V2","030000004c050000cc09000000000000",0x054c,0x09cc,1,2,0,3,3,4},
  MappingCase{"DualSense","030000004c050000e60c000000000000",0x054c,0x0ce6,1,2,0,3,3,4},
  MappingCase{"SwitchPro","030000007e0500000920000000000000",0x057e,0x2009,0,1,2,3,-7,-8},
  MappingCase{"EightBitDoPro2","03000000c82d00000660000000000000",0x2dc8,0x6006,1,0,4,3,-9,-10}
),[](const auto &p){return p.param.name;});
