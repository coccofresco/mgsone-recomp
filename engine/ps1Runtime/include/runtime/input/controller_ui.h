#pragma once
#include "runtime/input/controller_hub.h"
#include <imgui.h>

namespace ps1::input {
// Presentation-only diagnostics; never injects inputs into the guest.
inline void controllerPanel(ControllerHub &hub) {
  if (!ImGui::CollapsingHeader("Controller / input test")) return;
  ImGui::TextWrapped("Active: %s",hub.name().c_str());
  if (ImGui::BeginCombo("Device",hub.prefersAutomatic()?"Automatic (prefer Steam Input)":hub.name().c_str())) {
    if (ImGui::Selectable("Automatic (prefer Steam Input)",hub.prefersAutomatic())) hub.automatic();
    for (const auto &d:hub.devices()) {
      ImGui::PushID(d.id);
      if (ImGui::Selectable(d.name.c_str(),d.id==hub.activeId())) hub.select(d.id);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  int glyphs=hub.glyphs();
  if (ImGui::Combo("Button labels",&glyphs,"Automatic\0Xbox / Steam\0PlayStation\0Nintendo\0")) hub.setGlyphs(glyphs);
  int deadzone=hub.deadzone()*100/32767;
  if (ImGui::SliderInt("Stick deadzone",&deadzone,13,73,"%d%%")) hub.setDeadzone(deadzone*32767/100);
  ImGui::TextWrapped("Confirm: %s. Back: %s. R3: game menu.",hub.confirmLabel(),hub.cancelLabel());
  const auto raw=hub.sample();
  ImGui::Text("Stick: %.2f, %.2f    L2: %.2f    R2: %.2f",raw.axes[0]/32767.f,raw.axes[1]/32767.f,raw.axes[4]/32767.f,raw.axes[5]/32767.f);
  ImGui::Text("Menu: %s",raw.menu?"pressed":"released");
  static constexpr struct { uint16_t bit; const char *name; } buttons[]{
    {0x4000,"Cross"},{0x2000,"Circle"},{0x8000,"Square"},{0x1000,"Triangle"},
    {0x0010,"Up"},{0x0040,"Down"},{0x0080,"Left"},{0x0020,"Right"},
    {0x0400,"L1"},{0x0800,"R1"},{0x0008,"Start"},{0x0001,"Select"}};
  for (size_t i=0;i<std::size(buttons);++i) {
    if (i%4) ImGui::SameLine();
    ImGui::TextColored(raw.buttons&buttons[i].bit?ImVec4(.3f,1,.5f,1):ImVec4(.6f,.6f,.6f,1),"%s",buttons[i].name);
  }
  ImGui::TextWrapped("Steam Input: select the Gamepad template in Steam. Use Steam to remap controls; Guide/Home remains available to Steam.");
  if (!hub.error().empty()) ImGui::TextWrapped("%s",hub.error().c_str());
}
}
