#include "GUI.hpp"
#include "ui/StyleManager.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_memory_editor.h"
#include "TextEditor.h"
#include "ImGuiFileDialog.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iomanip>

// Static editor instances

static MemoryEditor s_memEdit;
static TextEditor   s_codeEditor;
static TextEditor   s_configEditor;
static TextEditor   s_ghidraEditor;
static TextEditor   s_logEditor;
static bool         s_editorsInit        = false;
static bool         s_configNeedsSync    = false;
static size_t       s_lastLogVersion     = 0;
static bool         s_wantsQuit          = false;
static bool         s_showSettings       = false;

// Hex highlight

struct HexHighlight { size_t start, end; ImU32 color; };
static std::vector<HexHighlight> s_hexHighlights;

static ImU32 HexBgCallback(const ImU8*, size_t off, void*) {
    for (const auto& h : s_hexHighlights)
        if (off >= h.start && off < h.end)
            return h.color;
    return 0;
}

// Helpers

static std::string Hex32(uint32_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return ss.str();
}

// Settings window

static void DrawSettingsWindow(StudioState& state) {
    if (!s_showSettings) return;

    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings##win", &s_showSettings)) { ImGui::End(); return; }

    if (ImGui::CollapsingHeader("Theme", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* themes[] = {"Dark", "Light", "Custom"};
        int cur = static_cast<int>(state.settings.theme);
        if (ImGui::Combo("Theme", &cur, themes, 3)) {
            state.settings.theme = static_cast<ThemeMode>(cur);
            StyleManager::ApplyTheme(state.settings.theme, state.settings);
        }
        if (state.settings.theme == ThemeMode::Custom) {
            ImGui::ColorEdit4("Background", state.settings.customBgBase);
            ImGui::ColorEdit4("Accent",     state.settings.customAccent);
            if (ImGui::Button("Apply"))
                StyleManager::ApplyTheme(state.settings.theme, state.settings);
        }
    }

    if (ImGui::CollapsingHeader("Font & Scale", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginCombo("Font", state.settings.selectedFont.c_str())) {
            for (const auto& f : state.availableFonts) {
                bool sel = (state.settings.selectedFont == f);
                if (ImGui::Selectable(f.c_str(), sel)) {
                    state.settings.selectedFont = f;
                    state.pendingFontRebuild = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::SliderFloat("Font Size", &state.settings.fontSize, 10.f, 24.f))
            state.pendingFontRebuild = true;
        if (ImGui::SliderFloat("UI Scale", &state.settings.uiScale, 0.5f, 2.0f))
            StyleManager::ApplyTheme(state.settings.theme, state.settings);
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (ImGui::Button("Reset Defaults")) {
        state.settings = AppSettings();
        state.pendingFontRebuild = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) s_showSettings = false;

    ImGui::End();
}

// Explorer panel

static void DrawExplorer(StudioState& state) {
    ImGui::Begin("Explorer");

    static char filter[128] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search functions...", filter, sizeof(filter));
    ImGui::Separator();

    if (!state.data.isAnalysisComplete) {
        float cx = ImGui::GetWindowSize().x * 0.5f;
        float cy = ImGui::GetWindowSize().y * 0.4f;
        if (state.data.elfPath.empty()) {
            const char* t = "No file loaded";
            ImGui::SetCursorPos(ImVec2(cx - ImGui::CalcTextSize(t).x * 0.5f, cy));
            ImGui::TextDisabled("%s", t);
        } else {
            const char* t = "Ready to Analyze (F5)";
            ImGui::SetCursorPos(ImVec2(cx - ImGui::CalcTextSize(t).x * 0.5f, cy));
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.f), "%s", t);
        }
        ImGui::End();
        return;
    }

    ImGui::BeginChild("FuncList");
    std::string filterStr(filter);
    const auto& funcs = state.data.functions;

    static std::vector<int> idx;
    idx.clear();
    for (int i = 0; i < static_cast<int>(funcs.size()); ++i)
        if (filterStr.empty() || funcs[i].name.find(filterStr) != std::string::npos)
            idx.push_back(i);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(idx.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            int i = idx[row];
            const auto& fn = funcs[i];

            // Color: PsyQ=yellow, overridden stub=red, skip=gray, force=green, default=white
            ImVec4 color = ImVec4(1, 1, 1, 1);
            auto it = state.data.funcOverrides.find(fn.name);
            if (it != state.data.funcOverrides.end()) {
                switch (it->second) {
                case OverrideStatus::Stub:           color = ImVec4(1.0f, 0.4f, 0.4f, 1.f); break;
                case OverrideStatus::Skip:           color = ImVec4(0.5f, 0.5f, 0.5f, 1.f); break;
                case OverrideStatus::ForceRecompile: color = ImVec4(0.4f, 1.0f, 0.4f, 1.f); break;
                default: break;
                }
            } else if (fn.isPsyQ) {
                color = ImVec4(1.0f, 0.85f, 0.2f, 1.f); // yellow = PsyQ lib function
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            bool sel = (state.selectedFuncIndex == i);
            if (ImGui::Selectable(fn.name.c_str(), sel)) {
                state.selectedFuncIndex = i;

                // Update C++ preview
                std::ostringstream ss;
                ss << "// Function: " << fn.name << "\n";
                ss << "// Address:  " << Hex32(fn.address) << "\n";
                ss << "// Size:     " << fn.size << " bytes\n";
                if (fn.isPsyQ) ss << "// Source:   PsyQ library\n";
                ss << "\nvoid " << fn.name << "() {\n    // ...\n}\n";
                s_codeEditor.SetText(ss.str());

                // Jump hex view to function
                if (!state.data.rawElfData.empty() && fn.address < state.data.rawElfData.size())
                    s_memEdit.GotoAddrAndHighlight(fn.address,
                        std::min(static_cast<size_t>(fn.address + fn.size),
                                 state.data.rawElfData.size()));
            }
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Address: %s", Hex32(fn.address).c_str());
                ImGui::Text("Size:    %u bytes", fn.size);
                if (fn.isPsyQ) ImGui::TextColored(ImVec4(1,0.85f,0.2f,1), "PsyQ library function");
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// Inspector panel

static void DrawInspector(StudioState& state) {
    ImGui::Begin("Inspector");

    if (!state.data.isAnalysisComplete ||
        state.selectedFuncIndex < 0 ||
        state.selectedFuncIndex >= static_cast<int>(state.data.functions.size())) {
        ImGui::TextDisabled("Select a function to inspect");
        ImGui::End();
        return;
    }

    const auto& fn = state.data.functions[state.selectedFuncIndex];

    ImGui::TextColored(ImVec4(0.0f, 0.6f, 1.0f, 1.0f), "%s", fn.name.c_str());
    if (fn.isPsyQ)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "PsyQ library function");
    ImGui::Separator();
    ImGui::Spacing();

    // Strategy combo
    OverrideStatus cur = OverrideStatus::Default;
    auto it = state.data.funcOverrides.find(fn.name);
    if (it != state.data.funcOverrides.end()) cur = it->second;

    const char* options[] = {"Auto (Default)", "Stub (Plug)", "Skip (Ignore)", "Force Recompile"};
    int idx = static_cast<int>(cur);
    ImGui::TextDisabled("Strategy:");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##strategy", &idx, options, 4)) {
        state.data.funcOverrides[fn.name] = static_cast<OverrideStatus>(idx);
        state.Log("Strategy changed for " + fn.name);
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    if (ImGui::BeginTable("meta", 2)) {
        auto row = [](const char* label, const std::string& val) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", val.c_str());
        };
        row("Address:", Hex32(fn.address));
        row("Size:",    std::to_string(fn.size) + " bytes");
        row("Source:",  fn.isPsyQ ? "PsyQ" : "Game code");
        ImGui::EndTable();
    }

    ImGui::End();
}

// Workspace panel (tabs)

static void DrawWorkspace(StudioState& state) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.f));
    ImGui::Begin("Workspace", nullptr, ImGuiWindowFlags_NoTitleBar);

    if (ImGui::BeginTabBar("Tabs", ImGuiTabBarFlags_Reorderable)) {

        // C++ Preview
        if (ImGui::BeginTabItem("  C++ Preview  ")) {
            if (ImGui::BeginPopupContextItem("cpp_ctx")) {
                if (ImGui::MenuItem("Copy All"))
                    ImGui::SetClipboardText(s_codeEditor.GetText().c_str());
                ImGui::EndPopup();
            }
            s_codeEditor.Render("CodeEditor");
            ImGui::EndTabItem();
        }

        // Hex View
        if (ImGui::BeginTabItem("  Hex View  ")) {
            s_hexHighlights.clear();
            if (state.data.isAnalysisComplete && state.selectedFuncIndex >= 0 &&
                state.selectedFuncIndex < static_cast<int>(state.data.functions.size())) {
                const auto& fn = state.data.functions[state.selectedFuncIndex];
                if (fn.address < state.data.rawElfData.size()) {
                    s_hexHighlights.push_back({
                        fn.address,
                        std::min(static_cast<size_t>(fn.address + fn.size),
                                 state.data.rawElfData.size()),
                        IM_COL32(0, 120, 215, 80)
                    });
                }
            }
            if (state.data.rawElfData.empty())
                ImGui::TextDisabled("No binary loaded");
            else
                s_memEdit.DrawContents(state.data.rawElfData.data(), state.data.rawElfData.size());
            ImGui::EndTabItem();
        }

        // config.toml
        if (ImGui::BeginTabItem("  config.toml  ")) {
            if (s_configNeedsSync) {
                s_configEditor.SetText(state.data.configTomlContent);
                s_configNeedsSync = false;
            }
            if (ImGui::Button("Save"))        state.SaveConfigTomlFromEditor(s_configEditor.GetText());
            ImGui::SameLine();
            if (ImGui::Button("Reload")) { state.LoadConfigToml(); s_configNeedsSync = true; }
            ImGui::SameLine();
            if (ImGui::Button("Copy All"))
                ImGui::SetClipboardText(s_configEditor.GetText().c_str());
            ImGui::Separator();
            s_configEditor.Render("ConfigEditor");
            ImGui::EndTabItem();
        }

        // Ghidra CSV
        if (ImGui::BeginTabItem("  Ghidra CSV  ")) {
            if (ImGui::Button("Import CSV...")) {
                IGFD::FileDialogConfig cfg; cfg.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("GhidraCSVKey", "Choose CSV", ".csv", cfg);
            }
            if (state.data.importedCSVFiles.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("No CSV files imported yet");
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu files)", state.data.importedCSVFiles.size());
            }
            ImGui::Separator();

            if (!state.data.importedCSVFiles.empty()) {
                ImGui::BeginChild("CSVList", ImVec2(0, 100), true);
                for (int i = 0; i < static_cast<int>(state.data.importedCSVFiles.size()); ++i) {
                    const auto& p = state.data.importedCSVFiles[i];
                    std::string fname = std::filesystem::path(p).filename().string();
                    ImGui::PushID(i);
                    if (ImGui::Selectable(fname.c_str(), state.data.selectedCSVIndex == i)) {
                        state.data.selectedCSVIndex = i;
                        std::ifstream f(p);
                        if (f) {
                            std::stringstream buf; buf << f.rdbuf();
                            s_ghidraEditor.SetText(buf.str());
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        state.data.importedCSVFiles.erase(state.data.importedCSVFiles.begin() + i);
                        ImGui::PopID(); break;
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::Separator();
                if (state.data.selectedCSVIndex >= 0)
                    s_ghidraEditor.Render("GhidraEditor");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// Logs panel

static void DrawLogs(StudioState& state) {
    ImGui::Begin("Logs");

    // Status / progress bar
    if (state.isBusy.load()) {
        float t = std::fmod(static_cast<float>(ImGui::GetTime()), 1.0f);
        ImGui::ProgressBar(t, ImVec2(-1, 6), "");
        ImGui::Text("%s", state.GetStatus().c_str());
    } else {
        ImGui::TextDisabled("Status: %s", state.GetStatus().c_str());
    }

    // Toolbar (right-aligned)
    float btnW = ImGui::CalcTextSize("Copy All").x + ImGui::CalcTextSize("Clear").x + 40.f;
    ImGui::SameLine(std::max(0.f, ImGui::GetWindowWidth() - btnW));
    if (ImGui::SmallButton("Copy All")) {
        std::lock_guard<std::mutex> lk(state.stateMutex);
        std::ostringstream ss;
        for (const auto& l : state.logs) ss << l << "\n";
        ImGui::SetClipboardText(ss.str().c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> lk(state.stateMutex);
        state.logs.clear();
        state.logVersion.fetch_add(1);
    }

    ImGui::Separator();

    // Sync log editor when new lines arrive
    size_t ver = state.logVersion.load();
    if (ver != s_lastLogVersion) {
        std::lock_guard<std::mutex> lk(state.stateMutex);
        std::ostringstream ss;
        for (const auto& l : state.logs) ss << l << "\n";
        s_logEditor.SetText(ss.str());

        TextEditor::ErrorMarkers markers;
        for (size_t i = 0; i < state.logs.size(); ++i) {
            const auto& l = state.logs[i];
            if (l.find("Error") != std::string::npos ||
                l.find("error") != std::string::npos ||
                l.find("Failed") != std::string::npos)
                markers.insert({static_cast<int>(i + 1), l});
        }
        s_logEditor.SetErrorMarkers(markers);
        s_lastLogVersion = ver;
    }

    s_logEditor.Render("LogEditor");
    ImGui::End();
}

// Public API

void GUI::ApplySettings(StudioState& state) {
    StyleManager::SetupFonts(state.settings);
    StyleManager::ApplyTheme(state.settings.theme, state.settings);
}

void GUI::RebuildFontsIfNeeded(StudioState& state) {
    if (state.pendingFontRebuild.exchange(false))
        StyleManager::SetupFonts(state.settings);
}

bool GUI::WantsQuit() { return s_wantsQuit; }

void GUI::DrawStudio(StudioState& state) {
    // One-time editor init
    if (!s_editorsInit) {
        s_codeEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
        s_codeEditor.SetPalette(TextEditor::GetDarkPalette());
        s_codeEditor.SetReadOnly(true);

        s_configEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
        s_configEditor.SetPalette(TextEditor::GetDarkPalette());
        s_configEditor.SetReadOnly(false);
        s_configEditor.SetText(state.data.configTomlContent);

        s_ghidraEditor.SetPalette(TextEditor::GetDarkPalette());
        s_ghidraEditor.SetReadOnly(true);

        s_logEditor.SetPalette(TextEditor::GetDarkPalette());
        s_logEditor.SetReadOnly(true);
        s_logEditor.SetShowWhitespaces(false);

        s_memEdit.ReadOnly       = true;
        s_memEdit.OptShowAscii   = true;
        s_memEdit.OptGreyOutZeroes = true;
        s_memEdit.BgColorFn      = HexBgCallback;

        s_editorsInit = true;
    }

    // Dockspace
    ImGuiID dockId = ImGui::GetID("MainDock");
    ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport());

    static bool firstRun = true;
    if (firstRun) {
        firstRun = false;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);

        ImGuiID left  = ImGui::DockBuilderSplitNode(dockId,  ImGuiDir_Left,  0.22f, nullptr, &dockId);
        ImGuiID right = ImGui::DockBuilderSplitNode(dockId,  ImGuiDir_Right, 0.28f, nullptr, &dockId);
        ImGuiID down  = ImGui::DockBuilderSplitNode(dockId,  ImGuiDir_Down,  0.25f, nullptr, &dockId);

        ImGui::DockBuilderDockWindow("Explorer",   left);
        ImGui::DockBuilderDockWindow("Inspector",  right);
        ImGui::DockBuilderDockWindow("Logs",       down);
        ImGui::DockBuilderDockWindow("Workspace",  dockId);
        ImGui::DockBuilderFinish(dockId);
    }

    // File dialogs
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (ImGuiFileDialog::Instance()->Display("OpenFileKey")) {
        if (ImGuiFileDialog::Instance()->IsOk())
            state.LoadELF(ImGuiFileDialog::Instance()->GetFilePathName());
        ImGuiFileDialog::Instance()->Close();
    }
    if (ImGuiFileDialog::Instance()->Display("GhidraCSVKey")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            state.ImportGhidraCSV(p);
            bool dup = false;
            for (const auto& e : state.data.importedCSVFiles)
                if (e == p) { dup = true; break; }
            if (!dup) state.data.importedCSVFiles.push_back(p);
            state.data.selectedCSVIndex = static_cast<int>(state.data.importedCSVFiles.size()) - 1;
            s_ghidraEditor.SetText(state.data.ghidraCSVContent);
        }
        ImGuiFileDialog::Instance()->Close();
    }
    if (ImGuiFileDialog::Instance()->Display("OutputDirKey")) {
        if (ImGuiFileDialog::Instance()->IsOk())
            state.SetOutputDir(ImGuiFileDialog::Instance()->GetFilePathName());
        ImGuiFileDialog::Instance()->Close();
    }

    // Menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                IGFD::FileDialogConfig cfg; cfg.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("OpenFileKey", "Open PS1 Binary",
                    ".*,.elf,.exe,.bin", cfg);
            }
            if (ImGui::MenuItem("Import Ghidra CSV...")) {
                IGFD::FileDialogConfig cfg; cfg.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("GhidraCSVKey", "Choose CSV", ".csv", cfg);
            }
            if (ImGui::MenuItem("Set Output Directory...")) {
                IGFD::FileDialogConfig cfg; cfg.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("OutputDirKey", "Choose Output Dir",
                    nullptr, cfg);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Config", "Ctrl+S", false, state.data.isAnalysisComplete)) {
                state.SaveConfigTOML();
                s_configNeedsSync = true;
            }
            if (ImGui::MenuItem("Reload Config")) {
                state.LoadConfigToml();
                s_configNeedsSync = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) s_wantsQuit = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools")) {
            bool busy = state.isBusy.load();
            if (ImGui::MenuItem("Analyze", "F5", false, !busy && !state.data.elfPath.empty()))
                state.StartAnalysis();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Preferences...")) s_showSettings = true;
            ImGui::EndMenu();
        }

        // Output path display (right side of menu bar)
        std::string outDisplay = "Output: " + state.GetEffectiveOutputPath();
        float textW = ImGui::CalcTextSize(outDisplay.c_str()).x;
        if (ImGui::GetWindowWidth() > textW + 300.f) {
            ImGui::SameLine(ImGui::GetWindowWidth() - textW - 16.f);
            ImGui::TextDisabled("%s", outDisplay.c_str());
        }

        ImGui::EndMainMenuBar();
    }

    // Keyboard shortcuts
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
                IGFD::FileDialogConfig cfg; cfg.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("OpenFileKey", "Open PS1 Binary",
                    ".*,.elf,.exe,.bin", cfg);
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && state.data.isAnalysisComplete) {
                state.SaveConfigTOML();
                s_configNeedsSync = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F5) && !state.isBusy.load() &&
                !state.data.elfPath.empty())
                state.StartAnalysis();
        }
    }

    // Panels
    DrawSettingsWindow(state);
    DrawExplorer(state);
    DrawInspector(state);
    DrawWorkspace(state);
    DrawLogs(state);
}
