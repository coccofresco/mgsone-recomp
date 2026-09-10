#include "StudioState.hpp"

#include <ps1recomp/config_generator.h>
#include <ps1recomp/disc_reader.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/psyq_signatures.h>

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

// Constructor / Destructor

StudioState::StudioState() {
    redirector_ = std::make_unique<StreamRedirector>(logs, stateMutex, logVersion);
    oldCout_ = std::cout.rdbuf(redirector_.get());
    oldCerr_ = std::cerr.rdbuf(redirector_.get());

    try {
        if (!std::filesystem::exists("output"))
            std::filesystem::create_directory("output");
        data.outputPath = std::filesystem::absolute("output").string();
    } catch (...) {}

    LoadSettings();
    ScanFonts();
    LoadConfigToml();
}

StudioState::~StudioState() {
    std::cout.rdbuf(oldCout_);
    std::cerr.rdbuf(oldCerr_);
    SaveSettings();
}

// LoadELF

void StudioState::LoadELF(const std::string& path) {
    data.elfPath = path;
    data.isAnalysisComplete = false;
    data.functions.clear();
    data.funcOverrides.clear();

    try {
        std::ifstream file(path, std::ios::binary);
        if (file)
            data.rawElfData.assign(std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>());
        else
            data.rawElfData.clear();
    } catch (...) {
        data.rawElfData.clear();
    }

    SetStatus("Loaded: " + std::filesystem::path(path).filename().string());
    Log("File loaded: " + path);
}

// StartAnalysis

void StudioState::StartAnalysis() {
    if (isBusy || data.elfPath.empty()) return;

    isBusy = true;
    SetStatus("Analyzing...");
    std::string currentPath = data.elfPath;

    workerThread_ = std::async(std::launch::async, [this, currentPath]() {
        try {
            // 1. Handle BIN disc image -- extract boot EXE
            std::string parsePath = currentPath;
            std::vector<uint8_t> extractedExe;

            auto toLower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                return s;
            };

            if (toLower(std::filesystem::path(currentPath).extension().string()) == ".bin") {
                ps1recomp::DiscReader disc;
                if (!disc.open(currentPath) || !disc.parseFilesystem()) {
                    Log("Disc error: " + disc.getError());
                    SetStatus("Analysis Failed");
                    isBusy = false;
                    return;
                }
                const auto* bootFile = disc.findFile(disc.getBootFilename());
                if (!bootFile) {
                    Log("Boot file not found on disc");
                    SetStatus("Analysis Failed");
                    isBusy = false;
                    return;
                }
                extractedExe = disc.readFile(*bootFile);
                parsePath = currentPath + ".boot.exe";
                std::ofstream tmp(parsePath, std::ios::binary);
                tmp.write(reinterpret_cast<const char*>(extractedExe.data()),
                          extractedExe.size());
            }

            // 2. Parse ELF / PS-X EXE
            ps1recomp::ElfParser parser;
            if (!parser.load(parsePath)) {
                Log("Parse error: " + parser.getError());
                SetStatus("Analysis Failed");
                isBusy = false;
                return;
            }

            // 3. Find functions
            ps1recomp::FunctionFinder finder;
            finder.findFunctions(parser);

            // 4. Match PsyQ signatures
            ps1recomp::PsyQMatcher matcher;
            matcher.matchFunctions(parser, finder);

            // 5. Build function list for UI
            std::vector<FunctionEntry> entries;
            entries.reserve(finder.getFunctionCount());
            for (const auto& fn : finder.getFunctions()) {
                FunctionEntry e;
                e.name    = fn.name;
                e.address = fn.address;
                e.size    = fn.size;
                e.isPsyQ  = matcher.isKnown(fn.name);
                entries.push_back(std::move(e));
            }

            {
                std::lock_guard<std::mutex> lock(stateMutex);
                data.functions          = std::move(entries);
                data.isAnalysisComplete = true;
            }

            Log(fmt::format("Analysis complete: {} functions ({} PsyQ)",
                            finder.getFunctionCount(), matcher.getMatchCount()));
            SetStatus("Analysis Complete");

        } catch (const std::exception& e) {
            Log(std::string("Analysis error: ") + e.what());
            SetStatus("Analysis Failed");
        }
        isBusy = false;
    });
}

// Config TOML

void StudioState::LoadConfigToml() {
    std::vector<std::string> candidates;
    if (!configTomlPath.empty())        candidates.push_back(configTomlPath);
    if (!data.customOutputPath.empty()) candidates.push_back(data.customOutputPath + "/config.toml");
    candidates.push_back(data.outputPath + "/config.toml");
    candidates.push_back("config.toml");

    for (const auto& p : candidates) {
        try {
            std::ifstream f(p);
            if (!f) continue;
            std::stringstream buf;
            buf << f.rdbuf();
            std::string content = buf.str();
            if (content.empty()) continue;
            data.configTomlContent = content;
            configTomlPath = std::filesystem::absolute(p).string();
            return;
        } catch (...) {}
    }

    // No config found -- create default
    configTomlPath = std::filesystem::absolute("config.toml").string();
    data.configTomlContent =
        "# PS1Recomp Configuration\n"
        "# Generated by ps1Interface\n\n"
        "[game]\n"
        "disc = \"\"\n\n"
        "[output]\n"
        "path = \"output\"\n";
    std::ofstream f(configTomlPath);
    if (f) f << data.configTomlContent;
}

void StudioState::SaveConfigTOML() {
    if (!data.isAnalysisComplete) return;

    try {
        ps1recomp::ConfigGenerator generator;
        ps1recomp::ElfParser       parser;
        ps1recomp::FunctionFinder  finder;
        ps1recomp::PsyQMatcher     matcher;

        // Re-use already analysed data via a minimal regeneration
        // (full regen would require re-parsing -- here we just save what we have)
        std::string savePath = configTomlPath.empty() ? "config.toml" : configTomlPath;

        // Apply overrides to config content and write it
        // For now just persist the current editor content
        std::ofstream f(savePath);
        if (f) {
            f << data.configTomlContent;
            Log("Config saved to " + savePath);
        }
    } catch (const std::exception& e) {
        Log(std::string("Error saving config: ") + e.what());
    }
}

void StudioState::SaveConfigTomlFromEditor(const std::string& content) {
    std::string savePath = configTomlPath.empty() ? "config.toml" : configTomlPath;
    try {
        std::ofstream f(savePath);
        if (f) {
            f << content;
            data.configTomlContent = content;
            Log("config.toml saved to " + savePath);
        }
    } catch (const std::exception& e) {
        Log(std::string("Error saving config.toml: ") + e.what());
    }
}

// Ghidra CSV

void StudioState::ImportGhidraCSV(const std::string& csvPath) {
    data.ghidraCSVPath = csvPath;
    try {
        std::ifstream f(csvPath);
        if (f) {
            std::stringstream buf;
            buf << f.rdbuf();
            data.ghidraCSVContent = buf.str();
        }
    } catch (...) {}

    // Apply symbol names from CSV to matched functions
    // CSV format (Ghidra): Name,Location,Type,Namespace,Source,Reference Count
    std::istringstream ss(data.ghidraCSVContent);
    std::string line;
    std::getline(ss, line); // skip header
    int count = 0;
    while (std::getline(ss, line)) {
        std::istringstream row(line);
        std::string name, addrStr;
        if (std::getline(row, name, ',') && std::getline(row, addrStr, ',')) {
            try {
                uint32_t addr = std::stoul(addrStr, nullptr, 16);
                for (auto& fn : data.functions) {
                    if (fn.address == addr && fn.name.find("func_") == 0) {
                        fn.name = name;
                        ++count;
                        break;
                    }
                }
            } catch (...) {}
        }
    }
    Log(fmt::format("Ghidra CSV imported: {} symbols applied", count));
}

// Output dir

void StudioState::SetOutputDir(const std::string& path) {
    if (path.empty()) return;
    try {
        data.customOutputPath = std::filesystem::absolute(path).string();
        if (!std::filesystem::exists(data.customOutputPath))
            std::filesystem::create_directories(data.customOutputPath);
        Log("Output directory set to: " + data.customOutputPath);
    } catch (const std::exception& e) {
        Log(std::string("Error setting output dir: ") + e.what());
    }
}

// Fonts

void StudioState::ScanFonts() {
    availableFonts.clear();
    try {
        std::filesystem::path fontDir = "external/Font";
        if (std::filesystem::exists(fontDir)) {
            for (const auto& entry : std::filesystem::directory_iterator(fontDir)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    if (ext == ".ttf" || ext == ".TTF")
                        availableFonts.push_back(entry.path().filename().string());
                }
            }
        }
    } catch (...) {}
    if (availableFonts.empty())
        availableFonts.push_back("Default");
}

// Settings

void StudioState::LoadSettings() {
    try {
        std::ifstream f("studio_settings.ini");
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            if (key == "theme")        settings.theme     = static_cast<ThemeMode>(std::stoi(val));
            else if (key == "fontSize")  settings.fontSize  = std::stof(val);
            else if (key == "uiScale")   settings.uiScale   = std::stof(val);
            else if (key == "font")      settings.selectedFont = val;
            else if (key == "width")     settings.windowWidth  = std::stoi(val);
            else if (key == "height")    settings.windowHeight = std::stoi(val);
            else if (key == "maximized") settings.maximized    = (val == "1");
            else if (key == "outputDir") data.customOutputPath = val;
        }
    } catch (...) {}
}

void StudioState::SaveSettings() {
    try {
        std::ofstream f("studio_settings.ini");
        f << "theme="     << static_cast<int>(settings.theme) << "\n";
        f << "fontSize="  << settings.fontSize  << "\n";
        f << "uiScale="   << settings.uiScale   << "\n";
        f << "font="      << settings.selectedFont << "\n";
        f << "width="     << settings.windowWidth  << "\n";
        f << "height="    << settings.windowHeight << "\n";
        f << "maximized=" << (settings.maximized ? "1" : "0") << "\n";
        if (!data.customOutputPath.empty())
            f << "outputDir=" << data.customOutputPath << "\n";
    } catch (...) {}
}
