// Keep this process alive for Steam's lifetime tracking. Python owns the
// frontend/game, and both watch their parent so Steam Stop also flushes saves.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>

static std::wstring quote(const std::wstring& value) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!length || length >= path.size()) return 1;
    const auto root = std::filesystem::path(path.data()).parent_path();
    const auto python = root / L"python/python.exe";
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    bool gui = argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--big-picture");
    if (!gui) AttachConsole(ATTACH_PARENT_PROCESS);
    std::wstring command = quote(python.wstring()) + L" -X utf8 " + quote((root / L"scripts/mgs_alpha.py").wstring());
    for (int i = 1; i < argc; ++i) command += L" " + quote(argv[i]);
    LocalFree(argv);
    SetEnvironmentVariableW(L"MGS_LAUNCHER_PARENT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE log = INVALID_HANDLE_VALUE;
    HANDLE input = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    if (gui) {
        wchar_t local[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
            std::error_code ec;
            auto logs = std::filesystem::path(local) / L"MGSRecomp";
            std::filesystem::create_directories(logs, ec);
            log = CreateFileW((logs / L"launcher.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &attributes, OPEN_EXISTING, 0, nullptr);
        if (log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = log;
            si.hStdInput = input;
        }
    } else {
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        if (si.hStdOutput && si.hStdOutput != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            SetHandleInformation(si.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }
    PROCESS_INFORMATION process{};
    auto created = CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, root.c_str(), &si, &process);
    const auto error = GetLastError();
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!created) {
        auto message = L"Cannot start the bundled launcher. Extract the entire ZIP first.\nWindows error " + std::to_wstring(error);
        MessageBoxW(nullptr, message.c_str(), L"MGS Recomp", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    if (gui && result != 0 && result != 143)
        MessageBoxW(nullptr, L"The launcher could not complete the operation.\nDetails: %LOCALAPPDATA%\\MGSRecomp\\launcher.log",
                    L"MGS Recomp", MB_OK | MB_ICONERROR);
    return static_cast<int>(result);
}
