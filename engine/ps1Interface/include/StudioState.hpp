#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <ps1recomp/config_generator.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/psyq_signatures.h>

// Enums

enum class OverrideStatus {
    Default,
    Stub,
    Skip,
    ForceRecompile
};

enum class ThemeMode {
    Dark,
    Light,
    Custom
};

// Settings (persisted to studio_settings.ini)

struct AppSettings {
    ThemeMode theme      = ThemeMode::Dark;
    float     fontSize   = 15.0f;
    float     uiScale    = 1.0f;
    std::string selectedFont = "Default";
    int  windowWidth     = 1280;
    int  windowHeight    = 720;
    bool maximized       = true;

    float customBgBase[4] = {0.08f, 0.08f, 0.08f, 1.00f};
    float customAccent[4] = {0.00f, 0.48f, 0.80f, 1.00f};
};

// Stream redirector (cout/cerr -> Logs panel)

class StreamRedirector : public std::stringbuf {
public:
    StreamRedirector(std::vector<std::string>& target,
                     std::mutex& mtx,
                     std::atomic<size_t>& ver)
        : logs_(target), mutex_(mtx), version_(ver) {}

    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string line = this->str();
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty()) {
            logs_.push_back(line);
            version_.fetch_add(1);
        }
        this->str("");
        return 0;
    }

private:
    std::vector<std::string>& logs_;
    std::mutex&               mutex_;
    std::atomic<size_t>&      version_;
};

// Analysis result (PS1-specific)

struct FunctionEntry {
    std::string name;
    uint32_t    address  = 0;
    uint32_t    size     = 0;
    bool        isPsyQ   = false;   // matched against PsyQ signatures
};

// UI data

struct UIState {
    std::string elfPath;
    std::string outputPath      = "output";
    std::string customOutputPath;
    std::string ghidraCSVPath;
    std::string ghidraCSVContent;
    std::string configTomlContent;

    std::vector<uint8_t>    rawElfData;
    std::vector<FunctionEntry> functions;
    std::map<std::string, OverrideStatus> funcOverrides;

    std::vector<std::string> importedCSVFiles;
    int selectedCSVIndex = -1;

    bool isAnalysisComplete = false;
};

// StudioState

class StudioState {
public:
    UIState     data;
    AppSettings settings;
    int         selectedFuncIndex = -1;

    std::atomic<bool>   isBusy{false};
    std::atomic<bool>   pendingFontRebuild{false};
    std::atomic<size_t> logVersion{0};

    std::vector<std::string> logs;
    std::vector<std::string> availableFonts;
    std::mutex               stateMutex;
    std::string              configTomlPath;

    StudioState();
    ~StudioState();

    // Actions
    void LoadELF(const std::string& path);
    void StartAnalysis();
    void LoadConfigToml();
    void SaveConfigTOML();
    void SaveConfigTomlFromEditor(const std::string& content);
    void ImportGhidraCSV(const std::string& csvPath);
    void SetOutputDir(const std::string& path);
    void ScanFonts();
    void LoadSettings();
    void SaveSettings();

    // Logging
    void Log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(stateMutex);
        logs.push_back("[Studio] " + msg);
        logVersion.fetch_add(1);
    }

    // Status
    void        SetStatus(const std::string& msg) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusMessage_ = msg;
    }
    std::string GetStatus() const {
        std::lock_guard<std::mutex> lock(statusMutex_);
        return statusMessage_;
    }

    std::string GetEffectiveOutputPath() const {
        return data.customOutputPath.empty() ? data.outputPath : data.customOutputPath;
    }

private:
    mutable std::mutex         statusMutex_;
    std::string                statusMessage_ = "Ready";

    std::future<void>          workerThread_;
    std::streambuf*            oldCout_ = nullptr;
    std::streambuf*            oldCerr_ = nullptr;
    std::unique_ptr<StreamRedirector> redirector_;
};
