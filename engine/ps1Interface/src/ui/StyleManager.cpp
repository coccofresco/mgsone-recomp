#include "ui/StyleManager.hpp"
#include "imgui.h"

namespace StyleManager {

void SetupFonts(const AppSettings& settings) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.SizePixels = settings.fontSize;

    // Try to load user-selected font; fall back to default
    bool loaded = false;
    if (settings.selectedFont != "Default") {
        std::string fontPath = "external/Font/" + settings.selectedFont;
        if (io.Fonts->AddFontFromFileTTF(fontPath.c_str(), settings.fontSize, &cfg))
            loaded = true;
    }
    if (!loaded)
        io.Fonts->AddFontDefault(&cfg);

    io.Fonts->Build();
}

void ApplyTheme(ThemeMode theme, const AppSettings& settings) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(settings.uiScale);

    switch (theme) {
    case ThemeMode::Light:
        ImGui::StyleColorsLight();
        break;

    case ThemeMode::Custom: {
        ImGui::StyleColorsDark();
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]  = ImVec4(settings.customBgBase[0],
                                             settings.customBgBase[1],
                                             settings.customBgBase[2],
                                             settings.customBgBase[3]);
        colors[ImGuiCol_Header]    = ImVec4(settings.customAccent[0],
                                             settings.customAccent[1],
                                             settings.customAccent[2],
                                             0.65f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(settings.customAccent[0],
                                                 settings.customAccent[1],
                                                 settings.customAccent[2],
                                                 0.85f);
        colors[ImGuiCol_Button]    = ImVec4(settings.customAccent[0],
                                             settings.customAccent[1],
                                             settings.customAccent[2],
                                             0.75f);
        break;
    }

    case ThemeMode::Dark:
    default:
        ImGui::StyleColorsDark();
        // Slightly tweaked dark theme
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]       = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
        colors[ImGuiCol_ChildBg]        = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        colors[ImGuiCol_Header]         = ImVec4(0.00f, 0.45f, 0.78f, 0.65f);
        colors[ImGuiCol_HeaderHovered]  = ImVec4(0.00f, 0.55f, 0.90f, 0.80f);
        colors[ImGuiCol_Button]         = ImVec4(0.00f, 0.45f, 0.78f, 0.75f);
        colors[ImGuiCol_ButtonHovered]  = ImVec4(0.00f, 0.55f, 0.90f, 0.90f);
        colors[ImGuiCol_Tab]            = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
        colors[ImGuiCol_TabSelected]    = ImVec4(0.00f, 0.45f, 0.78f, 1.00f);
        colors[ImGuiCol_TitleBgActive]  = ImVec4(0.00f, 0.30f, 0.55f, 1.00f);
        break;
    }
}

} // namespace StyleManager
