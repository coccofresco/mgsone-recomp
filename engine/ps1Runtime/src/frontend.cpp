// Controller-first desktop/Big Picture launcher. Uses the same Dear ImGui
// navigation as the Vulkan host menu; it never opens game discs itself.
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <toml.hpp>
#include "runtime/input/controller_hub.h"
#include "runtime/input/controller_ui.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif
namespace fs = std::filesystem;

int main(int argc, char **argv) {
  if (argc != 4) { std::cerr << "Usage: mgs-frontend STATE ACTION BIG_PICTURE\n"; return 2; }
  const fs::path statePath = argv[1], actionPath = argv[2];
  const fs::path releasedPath=statePath.parent_path()/"controllers-released";
  ps1::input::ControllerHub::configureBeforeSDL();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { std::cerr << SDL_GetError(); return 1; }
  const bool couch = std::string(argv[3]) == "1";
  auto *window = SDL_CreateWindow("MGS Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (couch ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  auto *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
  if (!renderer && window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) { std::cerr << SDL_GetError(); SDL_Quit(); return 1; }
  ImGui::CreateContext();
  auto &io = ImGui::GetIO(); io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImFontConfig font; font.SizePixels = 24.f; io.Fonts->AddFontDefault(&font);
  ImGui::StyleColorsDark(); ImGui::GetStyle().ScaleAllSizes(1.4f);
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer); ImGui_ImplSDLRenderer2_Init(renderer);
  ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,nullptr,0);
  ps1::input::ControllerHub controllers;
  controllers.initialize();
  toml::value state;
  std::string status, disc1, disc2, error;
  bool running=true, busy=false, hidden=false, waiting=false, focus=true;
  bool fullscreen=false, vsync=false, mute=false, fast=true;
  bool ambientOcclusion=true;
  int resolution=3, volume=100, choosing=0;
  constexpr int heights[]{720,1080,1440,2160};
  const char *labels[]{"720p (Deck)","1080p","1440p","2160p / 4K"};
  fs::path folder = std::getenv("HOME") ? std::getenv("HOME") : "/";
#if defined(_WIN32)
  if (const char *home = std::getenv("USERPROFILE")) folder = home;
  bool choosingDrive = false;
#endif
  auto stateStamp = fs::file_time_type::min();
  const int testFrames=std::getenv("MGS_FRONTEND_TEST_FRAMES") ?
      std::clamp(std::atoi(std::getenv("MGS_FRONTEND_TEST_FRAMES")),1,300) : 0;
  int frameNumber=0;
  const auto send = [&](toml::value value) {
    try {
      const fs::path temporary=actionPath.string()+".tmp";
      { std::ofstream out(temporary); out << toml::format(value); if (!out) throw std::runtime_error("Cannot write launcher action"); }
      fs::rename(temporary,actionPath); waiting=true;
    } catch (const std::exception &e) { error=e.what(); }
  };
  const auto action = [](const char *name) { toml::value v(toml::table{}); v["action"]=std::string(name); return v; };
  const auto command = [&](const char *name) { send(action(name)); };
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      controllers.event(event);
      if (!hidden || event.type==SDL_CONTROLLERDEVICEADDED || event.type==SDL_CONTROLLERDEVICEREMOVED)
        ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type==SDL_QUIT && !busy && !waiting) running=false;
      if (event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_F11) {
        const bool full=(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN_DESKTOP)!=0;
        SDL_SetWindowFullscreen(window,full?0:SDL_WINDOW_FULLSCREEN_DESKTOP);
      }
    }
    if (!hidden) controllers.refresh();
    try {
      const auto stamp=fs::last_write_time(statePath);
      if (stamp!=stateStamp) {
        state=toml::parse(statePath.string()); stateStamp=stamp;
        status=toml::find<std::string>(state,"status"); busy=toml::find<bool>(state,"busy");
        if (busy || !fs::exists(actionPath)) waiting=false;
        const bool hide=toml::find<bool>(state,"hide");
        if (hide!=hidden) {
          io.ClearInputKeys(); io.ClearEventsQueue();
          hidden=hide;
          if (hide) {
            ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,nullptr,0);
            controllers.close(); SDL_HideWindow(window);
            std::ofstream released(releasedPath);released<<"released\n";released.flush();
            if (!released) throw std::runtime_error("Cannot acknowledge controller handoff.");
          }
          else {
            fs::remove(releasedPath);
            ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,nullptr,0);
            controllers.close(); controllers.initialize();
            SDL_ShowWindow(window); SDL_RaiseWindow(window); focus=true;
          }
        }
        if (!busy) {
          fullscreen=toml::find<bool>(state,"fullscreen"); vsync=toml::find<bool>(state,"vsync");
          mute=toml::find<bool>(state,"mute"); fast=toml::find<bool>(state,"fast_loading");
          ambientOcclusion=toml::find_or<bool>(state,"ambient_occlusion",true);
          volume=toml::find<int>(state,"volume");
          const int height=toml::find<int>(state,"presentation_height");
          for (int i=0;i<4;++i) if (height==heights[i]) resolution=i;
        }
      }
    } catch (const std::exception &e) { error=e.what(); }
    auto *selectedPad=controllers.active();
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual,
                                selectedPad?&selectedPad:nullptr,selectedPad?1:0);
    if (hidden) { SDL_Delay(30); continue; }
    ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("MGS Recomp launcher",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("MGS RECOMP - ALPHA");
    ImGui::TextWrapped("USA v1.0 / fixed 16:9 / original game timing. Continuous completion is not yet certified.");
    ImGui::Separator();
    if (choosing) {
      ImGui::Text("Select disc %d (BIN or CUE)",choosing);
      ImGui::TextWrapped("%s",folder.string().c_str());
      if (ImGui::Button("Parent directory")) folder=folder.parent_path();
      ImGui::SameLine();
#if defined(_WIN32)
      if (ImGui::Button("Drives / removable storage")) choosingDrive = !choosingDrive;
      if (choosingDrive) {
        const DWORD drives = GetLogicalDrives();
        for (unsigned i = 0; i < 26; ++i) if (drives & (1u << i)) {
          const std::string drive = std::string(1, char('A' + i)) + ":/";
          ImGui::SameLine();
          if (ImGui::Button(drive.c_str())) { folder = drive; choosingDrive = false; }
        }
      }
#else
      if (ImGui::Button("Storage / SD cards")) folder="/run/media";
#endif
      ImGui::SameLine(); if (ImGui::Button("Cancel")) choosing=0;
      ImGui::BeginChild("Files",ImVec2(0,-70),true);
      try {
        std::vector<fs::directory_entry> entries;
        for (auto &e:fs::directory_iterator(folder,fs::directory_options::skip_permission_denied)) {
          std::string ext=e.path().extension().string();
          std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return std::tolower(c);});
          if (e.is_directory() || ext==".bin" || ext==".cue") entries.push_back(e);
        }
        std::sort(entries.begin(),entries.end(),[](auto &a,auto &b){
          if (a.is_directory()!=b.is_directory()) return a.is_directory(); return a.path()<b.path(); });
        for (auto &e:entries) {
          const std::string label=(e.is_directory()?"[DIR] ":"")+e.path().filename().string();
          ImGui::PushID(e.path().string().c_str());
          if (ImGui::Selectable(label.c_str())) {
            if (e.is_directory()) folder=e.path();
            else { (choosing==1?disc1:disc2)=e.path().string(); choosing=0; }
            ImGui::PopID();break;
          }
          ImGui::PopID();
        }
      } catch (const std::exception &e) { ImGui::TextWrapped("%s",e.what()); }
      ImGui::EndChild();
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) choosing=0;
    } else {
      ImGui::BeginDisabled(busy || waiting);
      if (focus) { ImGui::SetKeyboardFocusHere(); focus=false; }
      const bool installed=state.contains("installed") && toml::find<bool>(state,"installed");
      ImGui::BeginDisabled(!installed);
      if (ImGui::Button("PLAY",ImVec2(-1,46))) command("run");
      ImGui::EndDisabled();
      if (ImGui::CollapsingHeader("Import original discs",installed?0:ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Choose disc 1")) choosing=1;
        ImGui::SameLine(); ImGui::TextWrapped("%s",fs::path(disc1).filename().string().c_str());
        if (ImGui::Button("Choose disc 2")) choosing=2;
        ImGui::SameLine(); ImGui::TextWrapped("%s",fs::path(disc2).filename().string().c_str());
        ImGui::BeginDisabled(disc1.empty()||disc2.empty());
        if (ImGui::Button("Verify and import both discs",ImVec2(-1,42))) {
          auto v=action("import"); v["disc1"]=disc1; v["disc2"]=disc2; send(v);
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("Select the extracted USA v1.0 BIN/CUE pairs from your storage.");
      }
      if (ImGui::CollapsingHeader("Settings",ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Combo("Vulkan resolution",&resolution,labels,4);
        ImGui::Checkbox("Fullscreen",&fullscreen); ImGui::SameLine(); ImGui::Checkbox("VSync",&vsync);
        ImGui::Checkbox("Fast loading",&fast); ImGui::SameLine(); ImGui::Checkbox("Mute",&mute);
        ImGui::Checkbox("Ambient occlusion",&ambientOcclusion);
        ImGui::SliderInt("Volume",&volume,0,100);
        if (ImGui::Button("Save settings",ImVec2(-1,42))) {
          auto v=action("settings"); v["presentation_height"]=heights[resolution];
          v["fullscreen"]=fullscreen; v["vsync"]=vsync; v["fast_loading"]=fast;
          v["ambient_occlusion"]=ambientOcclusion;
          v["mute"]=mute; v["volume"]=volume; send(v);
        }
      }
      if (ImGui::CollapsingHeader("Saves and support")) {
        if (ImGui::Button("Back up saves")) command("backup");
        ImGui::SameLine(); if (ImGui::Button("Export diagnostics")) command("diagnostics");
        if (ImGui::Button("Verify installed files")) command("verify");
      }
      ps1::input::controllerPanel(controllers);
      if (ImGui::Button("Exit launcher")) running=false;
      ImGui::EndDisabled();
    }
    ImGui::Separator(); ImGui::TextWrapped("%s",status.c_str());
    if (!error.empty()) ImGui::TextWrapped("%s",error.c_str());
    ImGui::TextWrapped("D-pad / left stick: navigate. %s: select. %s: back. R3: game menu.",controllers.confirmLabel(),controllers.cancelLabel());
    ImGui::End(); ImGui::Render();
    SDL_SetRenderDrawColor(renderer,10,14,18,255); SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),renderer);
    if (testFrames && frameNumber+1==testFrames) {
      if (const char *path=std::getenv("MGS_FRONTEND_CAPTURE")) {
        int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
        auto *surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_ARGB8888);
        if (!surface || SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_ARGB8888,surface->pixels,surface->pitch)!=0 ||
            SDL_SaveBMP(surface,path)!=0) { std::cerr<<"Frontend capture failed: "<<SDL_GetError(); return 3; }
        SDL_FreeSurface(surface);
      }
      running=false;
    }
    SDL_RenderPresent(renderer);++frameNumber;
    SDL_Delay(8);
  }
  ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
  controllers.close();
  SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
  return 0;
}
