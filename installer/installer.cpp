#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <regex>
#include <cstring>
#include <cstdio>

namespace fs = std::filesystem;
constexpr wchar_t title[] = L"Universal Plugin Loader Installer 0.4.2";
static bool silent = false;
static int Fail(const std::wstring& message) {
    fwprintf(stderr, L"%ls\n", message.c_str());
    if (!silent) MessageBoxW(nullptr, message.c_str(), title, MB_OK | MB_ICONERROR);
    return 1;
}
static std::vector<unsigned char> Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static bool Native64(const std::vector<unsigned char>& bytes) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        size_t(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > bytes.size()) return false;
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, bytes.data() + dos.e_lfanew, sizeof(nt));
    return nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}
static bool HasMarker(const std::vector<unsigned char>& bytes, const char* marker) {
    const size_t length = std::strlen(marker) + 1;
    for (size_t i = 0; i + length <= bytes.size(); ++i)
        if (!std::memcmp(bytes.data() + i, marker, length)) return true;
    return false;
}
static bool IsUPL(const std::vector<unsigned char>& bytes) {
    return Native64(bytes) && HasMarker(bytes, "UPL_GetApiVersion");
}
static bool IsRuntime(const std::vector<unsigned char>& bytes) {
    return Native64(bytes) && !IsUPL(bytes) && HasMarker(bytes, "__CxxFrameHandler4");
}
static bool GameRunning(const fs::path& target) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) do {
        if (!_wcsicmp(entry.szExeFile, L"ScrapMechanic.exe")) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            wchar_t image[32768]{};
            DWORD length = 32768;
            const bool queried = process && QueryFullProcessImageNameW(process, 0, image, &length);
            if (process) CloseHandle(process);
            if (!queried || !_wcsicmp(fs::path(image).lexically_normal().c_str(),
                (target / L"ScrapMechanic.exe").c_str())) { found = true; break; }
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}
static fs::path Normalize(fs::path path) {
    if (!_wcsicmp(path.filename().c_str(), L"ScrapMechanic.exe")) path = path.parent_path();
    std::error_code error;
    if (fs::is_regular_file(path / L"Release" / L"ScrapMechanic.exe", error)) path /= L"Release";
    return fs::absolute(path, error).lexically_normal();
}
static bool ValidTarget(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path / L"ScrapMechanic.exe", error);
}
static std::vector<fs::path> FindGames() {
    std::vector<fs::path> games;
    std::set<std::wstring> seen;
    auto add = [&](fs::path path) {
        path = Normalize(path);
        std::wstring key = path.wstring();
        for (auto& c : key) c = towlower(c);
        if (ValidTarget(path) && seen.insert(key).second) games.push_back(path);
    };
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    fs::path parent = fs::path(executable).parent_path();
    for (int i = 0; i < 4 && !parent.empty(); ++i, parent = parent.parent_path()) add(parent);
    std::vector<fs::path> steamRoots;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t value[32768]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(root, root == HKEY_CURRENT_USER ? L"Software\\Valve\\Steam" :
            L"Software\\WOW6432Node\\Valve\\Steam", root == HKEY_CURRENT_USER ? L"SteamPath" : L"InstallPath",
            RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) steamRoots.emplace_back(value);
    }
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) if (drives & (1u << i)) {
        const std::wstring drive = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
        for (const wchar_t* suffix : {L"Steam", L"SteamLibrary", L"Program Files\\Steam", L"Program Files (x86)\\Steam"})
            steamRoots.emplace_back(fs::path(drive) / suffix);
    }
    std::vector<fs::path> libraries = steamRoots;
    for (const auto& root : steamRoots) {
        auto bytes = Read(root / L"steamapps" / L"libraryfolders.vdf");
        std::string text(bytes.begin(), bytes.end());
        std::regex pattern("\"path\"\\s*\"([^\"]+)\"");
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            std::string path = (*it)[1];
            size_t position = 0;
            while ((position = path.find("\\\\", position)) != std::string::npos) path.replace(position, 2, "\\");
            libraries.push_back(fs::u8path(path));
        }
    }
    for (const auto& root : libraries) add(root / L"steamapps" / L"common" / L"Scrap Mechanic");
    return games;
}
static fs::path ChooseGame() {
    wchar_t file[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"Scrap Mechanic (ScrapMechanic.exe)\0ScrapMechanic.exe\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = 32768;
    dialog.lpstrTitle = L"Select Scrap Mechanic\\Release\\ScrapMechanic.exe";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return {};
    return Normalize(file);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        int count = 0;
        wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
        fs::path target;
        bool explicitTarget = false;
        for (int i = 1; i < count; ++i) {
            if (!_wcsicmp(args[i], L"/silent")) silent = true;
            else if (!_wcsicmp(args[i], L"/dir") && i + 1 < count) { target = args[++i]; explicitTarget = true; }
            else { LocalFree(args); return Fail(L"Usage: installer.exe [/dir \"Scrap Mechanic\\Release\"] [/silent]"); }
        }
        LocalFree(args);
        if (silent && !explicitTarget) return Fail(L"Silent installation requires /dir.");
        if (target.empty()) {
            const auto games = FindGames();
            target = games.size() == 1 ? games.front() : ChooseGame();
            if (target.empty()) return 2;
        }
        target = Normalize(target);
        if (!ValidTarget(target)) return Fail(L"Select the Release directory containing ScrapMechanic.exe.");
        if (GameRunning(target)) return Fail(L"Close Scrap Mechanic before installing or updating UPL.");
        if (!silent && MessageBoxW(nullptr, (L"Install or update Universal Plugin Loader 0.4.2 in:\n\n" +
            target.wstring() + L"\n\nThe original runtime will be preserved.").c_str(), title,
            MB_YESNO | MB_ICONQUESTION) != IDYES) return 2;
        HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
        if (!resource) return Fail(L"Installer payload is missing.");
        const DWORD size = SizeofResource(instance, resource);
        const auto* data = static_cast<const unsigned char*>(LockResource(LoadResource(instance, resource)));
        if (!data || !size) return Fail(L"Installer payload cannot be read.");
        const std::vector<unsigned char> payload(data, data + size);
        if (!IsUPL(payload) || !HasMarker(payload, "UPL_RegisterLuaPcallObserver"))
            return Fail(L"Installer payload is invalid.");
        const fs::path current = target / L"vcruntime140_1.dll";
        const fs::path backup = target / L"vcruntime140_1_.dll";
        std::error_code error;
        const bool hasBackup = fs::exists(backup, error);
        const bool hasCurrent = fs::exists(current, error);
        if (hasBackup && !IsRuntime(Read(backup))) return Fail(L"The original runtime backup is invalid. Verify the game files through Steam.");
        if (!hasBackup && (!hasCurrent || !IsRuntime(Read(current))))
            return Fail(L"The original runtime is missing. Verify the game files through Steam before installing UPL.");
        if (hasBackup && hasCurrent && !IsUPL(Read(current)))
            return Fail(L"An original runtime already exists beside its backup. Preserve these files before installing UPL.");
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempFileNameW(target.c_str(), L"upl", 0, temporary))
            return Fail(L"Cannot write to the game directory. Run the installer as administrator if needed.");
        HANDLE output = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool saved = output != INVALID_HANDLE_VALUE && WriteFile(output, data, size, &written, nullptr) && written == size && FlushFileBuffers(output);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (!saved || Read(temporary) != payload) { DeleteFileW(temporary); return Fail(L"Could not write the loader DLL."); }
        bool movedOriginal = false;
        if (!hasBackup) {
            if (!MoveFileW(current.c_str(), backup.c_str())) { DeleteFileW(temporary); return Fail(L"Could not preserve the original runtime."); }
            movedOriginal = true;
        }
        if (!MoveFileExW(temporary, current.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (movedOriginal) MoveFileW(backup.c_str(), current.c_str());
            DeleteFileW(temporary);
            return Fail(L"Could not install UPL. Close the game and check directory permissions.");
        }
        if (!silent) MessageBoxW(nullptr, L"Universal Plugin Loader 0.4.2 installed successfully.\n\nYou can start Scrap Mechanic normally through Steam.", title, MB_OK | MB_ICONINFORMATION);
        return 0;
    } catch (...) { return Fail(L"Installation failed. Check the selected directory and its permissions."); }
}
