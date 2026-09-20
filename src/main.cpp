#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr wchar_t kLoaderVersion[] = L"0.4.0";
constexpr char kLoaderVersionText[] = "0.4.0";
constexpr wchar_t kAppId[] = L"387990";
constexpr wchar_t kManifestRelativePath[] = L"Native\\plugin.json";
constexpr DWORD kLogPollMs = 40;
constexpr std::uint64_t kFileTimeTicksPerSecond = 10'000'000ULL;
constexpr std::uint64_t kLogCreationSlackTicks = 10ULL * kFileTimeTicksPerSecond;

HMODULE g_selfModule = nullptr;
HMODULE g_realRuntime = nullptr;
FARPROC g_runtimeExports[3]{};

std::mutex g_activeStateMutex;
std::set<std::string> g_activeContent;
std::set<std::string> g_pendingContent;
std::set<std::string> g_activePlugins;
std::set<std::string> g_pendingPlugins;
bool g_activeBlockOpen = false;

const char* const kRuntimeExportNames[3] = {
    "__CxxFrameHandler4",
    "__NLG_Dispatch2",
    "__NLG_Return2"
};

std::uint64_t fileTimeValue(const FILETIME& time)
{
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::wstring modulePath(HMODULE module)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::wstring executablePath()
{
    return modulePath(nullptr);
}

std::string utf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring lowerWide(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z')
        {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    });
    return value;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void beginActiveStateBlock()
{
    std::lock_guard lock(g_activeStateMutex);
    g_pendingContent.clear();
    g_pendingPlugins.clear();
    g_activeBlockOpen = true;
}

void markActiveContent(const std::string& contentId)
{
    std::lock_guard lock(g_activeStateMutex);
    g_pendingContent.insert(lowerAscii(contentId));
}

void markActivePlugin(const std::string& pluginId)
{
    std::lock_guard lock(g_activeStateMutex);
    g_pendingPlugins.insert(lowerAscii(pluginId));
}

void endActiveStateBlock()
{
    std::lock_guard lock(g_activeStateMutex);
    g_activeContent = g_pendingContent;
    g_activePlugins = g_pendingPlugins;
    g_pendingContent.clear();
    g_pendingPlugins.clear();
    g_activeBlockOpen = false;
}

bool isContentActive(std::string contentId)
{
    contentId = lowerAscii(std::move(contentId));
    std::lock_guard lock(g_activeStateMutex);
    const auto& state = g_activeBlockOpen ? g_pendingContent : g_activeContent;
    return state.find(contentId) != state.end();
}

bool isPluginActive(std::string pluginId)
{
    pluginId = lowerAscii(std::move(pluginId));
    std::lock_guard lock(g_activeStateMutex);
    const auto& state = g_activeBlockOpen ? g_pendingPlugins : g_activePlugins;
    return state.find(pluginId) != state.end();
}

fs::path releaseDirectory()
{
    const std::wstring exe = executablePath();
    return exe.empty() ? fs::path{} : fs::path(exe).parent_path();
}

fs::path gameDirectory()
{
    const fs::path release = releaseDirectory();
    return release.empty() ? fs::path{} : release.parent_path();
}

fs::path loaderLogPath()
{
    const fs::path release = releaseDirectory();
    return release.empty() ? fs::path(L"UniversalPluginLoader.log")
                           : release / L"UniversalPluginLoader.log";
}

std::mutex& logMutex()
{
    static std::mutex mutex;
    return mutex;
}

void logLine(const std::string& message)
{
    std::lock_guard lock(logMutex());
    const fs::path path = loaderLogPath();
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (stream)
    {
        stream << message << "\r\n";
    }
    OutputDebugStringA(("[UniversalPluginLoader] " + message + "\n").c_str());
}

void resetLog()
{
    std::lock_guard lock(logMutex());
    std::ofstream stream(loaderLogPath(), std::ios::binary | std::ios::trunc);
    if (stream)
    {
        stream << "Universal Plugin Loader " << utf8(kLoaderVersion) << "\r\n";
    }
}

std::string windowsErrorText(DWORD error)
{
    if (error == ERROR_SUCCESS)
    {
        return "0";
    }

    LPSTR raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags, nullptr, error, 0,
                                        reinterpret_cast<LPSTR>(&raw), 0, nullptr);
    std::string text = length > 0 && raw != nullptr ? std::string(raw, length) : std::to_string(error);
    if (raw != nullptr)
    {
        LocalFree(raw);
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
    {
        text.pop_back();
    }
    return text;
}

bool commandLineDisablesLoader()
{
    const wchar_t* commandLine = GetCommandLineW();
    if (commandLine == nullptr)
    {
        return false;
    }
    const std::wstring lowered = lowerWide(commandLine);
    return lowered.find(L"-noinject") != std::wstring::npos ||
           lowered.find(L"-noupl") != std::wstring::npos;
}

bool isScrapMechanicProcess()
{
    const fs::path exe(executablePath());
    return lowerWide(exe.filename().wstring()) == L"scrapmechanic.exe";
}

std::string readSmallTextFile(const fs::path& path, std::size_t maxBytes = 1024 * 1024)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {};
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > maxBytes)
    {
        return {};
    }
    stream.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!data.empty())
    {
        stream.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (!stream && !stream.eof())
        {
            return {};
        }
    }
    return data;
}

std::string jsonStringValue(const std::string& document, std::string_view key)
{
    const std::string marker = "\"" + std::string(key) + "\"";
    std::size_t cursor = document.find(marker);
    if (cursor == std::string::npos)
    {
        return {};
    }
    cursor = document.find(':', cursor + marker.size());
    if (cursor == std::string::npos)
    {
        return {};
    }
    cursor = document.find('"', cursor + 1);
    if (cursor == std::string::npos)
    {
        return {};
    }

    std::string value;
    for (++cursor; cursor < document.size(); ++cursor)
    {
        const char ch = document[cursor];
        if (ch == '"')
        {
            return value;
        }
        if (ch == '\\' && cursor + 1 < document.size())
        {
            const char escaped = document[++cursor];
            switch (escaped)
            {
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case '"': value.push_back('"'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return {};
            }
            continue;
        }
        value.push_back(ch);
    }
    return {};
}

struct NumericVersion
{
    int major = 0;
    int minor = 0;
    int patch = 0;
};

std::optional<NumericVersion> parseNumericVersion(std::string_view text)
{
    NumericVersion result{};
    int* parts[3] = {&result.major, &result.minor, &result.patch};
    std::size_t cursor = 0;
    for (int index = 0; index < 3; ++index)
    {
        if (cursor >= text.size() || !std::isdigit(static_cast<unsigned char>(text[cursor])))
        {
            return std::nullopt;
        }
        int value = 0;
        while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])))
        {
            value = value * 10 + (text[cursor] - '0');
            ++cursor;
        }
        *parts[index] = value;
        if (index < 2)
        {
            if (cursor >= text.size() || text[cursor] != '.')
            {
                return std::nullopt;
            }
            ++cursor;
        }
    }
    // Suffixes such as -upl are allowed after the numeric triplet.
    return result;
}

bool versionAtLeast(std::string_view current, std::string_view required)
{
    const auto a = parseNumericVersion(current);
    const auto b = parseNumericVersion(required);
    if (!a || !b)
    {
        return false;
    }
    if (a->major != b->major) return a->major > b->major;
    if (a->minor != b->minor) return a->minor > b->minor;
    return a->patch >= b->patch;
}

bool validUuid(std::string_view value)
{
    if (value.size() != 36)
    {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if ((dash && ch != '-') || (!dash && std::isxdigit(ch) == 0))
        {
            return false;
        }
    }
    return true;
}

bool validPluginId(const std::string& id)
{
    if (id.empty() || id.size() > 128)
    {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-';
    });
}

struct ActiveUgcEntry
{
    std::string contentId;
    std::uint64_t steamFileId = 0;
    bool isLocal = false;
};

std::optional<ActiveUgcEntry> parseActiveUgcLine(const std::string& line)
{
    if (line.find(" - UGC - Type:") == std::string::npos)
    {
        return std::nullopt;
    }

    constexpr std::string_view contentMarker = "Content ID: ";
    constexpr std::string_view steamMarker = "Steam File ID: ";
    const std::size_t contentStartRaw = line.find(contentMarker);
    const std::size_t steamStartRaw = line.find(steamMarker);
    if (contentStartRaw == std::string::npos || steamStartRaw == std::string::npos)
    {
        return std::nullopt;
    }

    const std::size_t contentStart = contentStartRaw + contentMarker.size();
    const std::size_t contentEnd = line.find(" -", contentStart);
    if (contentEnd == std::string::npos)
    {
        return std::nullopt;
    }

    ActiveUgcEntry entry;
    entry.contentId = line.substr(contentStart, contentEnd - contentStart);
    if (!validUuid(entry.contentId))
    {
        return std::nullopt;
    }

    const std::size_t steamStart = steamStartRaw + steamMarker.size();
    const std::size_t steamEnd = line.find(" -", steamStart);
    const std::string steamText = line.substr(
        steamStart,
        steamEnd == std::string::npos ? std::string::npos : steamEnd - steamStart);
    try
    {
        entry.steamFileId = std::stoull(steamText);
    }
    catch (...)
    {
        return std::nullopt;
    }

    const std::string lowered = lowerAscii(line);
    entry.isLocal = lowered.find(" - local: true") != std::string::npos;
    return entry;
}

std::optional<fs::path> findSteamAppsAncestor(fs::path start)
{
    std::error_code error;
    start = fs::weakly_canonical(start, error);
    if (error)
    {
        start = start.lexically_normal();
    }

    for (fs::path current = start; !current.empty(); current = current.parent_path())
    {
        if (lowerWide(current.filename().wstring()) == L"steamapps")
        {
            return current;
        }
        if (current == current.root_path())
        {
            break;
        }
    }
    return std::nullopt;
}

std::string trimAscii(std::string value)
{
    auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string unescapeVdfString(std::string value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size())
        {
            const char next = value[++i];
            if (next == '\\' || next == '"')
            {
                out.push_back(next);
            }
            else
            {
                out.push_back('\\');
                out.push_back(next);
            }
        }
        else
        {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::wstring wideFromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::vector<fs::path> discoverWorkshopRoots()
{
    std::vector<fs::path> roots;
    std::set<std::wstring> seenLibraries;

    const auto steamApps = findSteamAppsAncestor(gameDirectory());
    if (!steamApps)
    {
        logLine("WARNING: could not locate steamapps ancestor from game path: " + utf8(gameDirectory().wstring()));
        return roots;
    }

    auto addLibrary = [&](const fs::path& libraryRoot) {
        std::error_code error;
        fs::path normalized = fs::weakly_canonical(libraryRoot, error);
        if (error) normalized = libraryRoot.lexically_normal();
        const std::wstring key = lowerWide(normalized.wstring());
        if (!seenLibraries.insert(key).second) return;
        roots.push_back(normalized / L"steamapps" / L"workshop" / L"content" / kAppId);
    };

    // Always include the Steam library that actually contains Scrap Mechanic.
    addLibrary(steamApps->parent_path());

    // Steam stores every configured library here. We only need the quoted "path" values.
    const std::string vdf = readSmallTextFile(*steamApps / L"libraryfolders.vdf", 4 * 1024 * 1024);
    if (!vdf.empty())
    {
        std::istringstream stream(vdf);
        std::string line;
        while (std::getline(stream, line))
        {
            const std::string lowered = lowerAscii(line);
            const std::size_t keyPos = lowered.find("\"path\"");
            if (keyPos == std::string::npos) continue;

            const std::size_t firstQuote = line.find('"', keyPos + 6);
            if (firstQuote == std::string::npos) continue;
            const std::size_t secondQuote = line.find('"', firstQuote + 1);
            if (secondQuote == std::string::npos) continue;

            const std::string raw = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            const std::wstring wide = wideFromUtf8(unescapeVdfString(trimAscii(raw)));
            if (!wide.empty()) addLibrary(fs::path(wide));
        }
    }

    return roots;
}

std::vector<fs::path> discoverLocalModsRoots()
{
    std::vector<fs::path> roots;

    wchar_t appData[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
    if (length == 0 || length >= std::size(appData))
    {
        logLine("WARNING: APPDATA is unavailable; local mod discovery disabled");
        return roots;
    }

    const fs::path userRoot = fs::path(appData) / L"Axolot Games" / L"Scrap Mechanic" / L"User";
    std::error_code error;
    if (!fs::is_directory(userRoot, error))
    {
        logLine("WARNING: Scrap Mechanic local User directory not found: " + utf8(userRoot.wstring()));
        return roots;
    }

    for (fs::directory_iterator it(userRoot, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        if (!it->is_directory(error)) continue;
        const std::wstring name = lowerWide(it->path().filename().wstring());
        if (name.rfind(L"user_", 0) != 0) continue;

        const fs::path mods = it->path() / L"Mods";
        error.clear();
        if (fs::is_directory(mods, error))
        {
            roots.push_back(mods);
        }
        error.clear();
    }

    return roots;
}

bool descriptionMatchesContent(const fs::path& modRoot, const ActiveUgcEntry& entry)
{
    const std::string description = readSmallTextFile(modRoot / L"description.json", 512 * 1024);
    if (description.empty())
    {
        return false;
    }
    const std::string localId = lowerAscii(jsonStringValue(description, "localId"));
    return !localId.empty() && localId == lowerAscii(entry.contentId);
}

fs::path resolveLocalModRoot(const std::vector<fs::path>& localModsRoots, const ActiveUgcEntry& entry)
{
    for (const fs::path& modsRoot : localModsRoots)
    {
        std::error_code error;
        for (fs::directory_iterator iterator(modsRoot, fs::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error))
        {
            if (!iterator->is_directory(error)) continue;
            if (descriptionMatchesContent(iterator->path(), entry))
            {
                return iterator->path();
            }
        }
    }
    return {};
}

fs::path resolveWorkshopModRoot(const std::vector<fs::path>& workshopRoots, const ActiveUgcEntry& entry)
{
    for (const fs::path& workshopRoot : workshopRoots)
    {
        std::error_code error;
        if (entry.steamFileId != 0)
        {
            const fs::path direct = workshopRoot / std::to_wstring(entry.steamFileId);
            if (fs::is_directory(direct, error) && descriptionMatchesContent(direct, entry))
            {
                return direct;
            }
        }

        // Fallback: some developer/Steam states can have a useful Content ID even
        // when the direct Steam-file folder is unavailable.
        error.clear();
        if (!fs::is_directory(workshopRoot, error)) continue;
        for (fs::directory_iterator iterator(workshopRoot, fs::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error))
        {
            if (iterator->is_directory(error) && descriptionMatchesContent(iterator->path(), entry))
            {
                return iterator->path();
            }
        }
    }
    return {};
}

bool pathInsideRoot(const fs::path& candidate, const fs::path& root)
{
    std::error_code error;
    const fs::path canonicalCandidate = fs::weakly_canonical(candidate, error);
    if (error)
    {
        return false;
    }
    error.clear();
    const fs::path canonicalRoot = fs::weakly_canonical(root, error);
    if (error)
    {
        return false;
    }

    const fs::path relative = canonicalCandidate.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute())
    {
        return false;
    }
    for (const auto& component : relative)
    {
        if (component == L"..")
        {
            return false;
        }
    }
    return true;
}

struct PluginManifest
{
    std::string id;
    std::string name;
    std::string version;
    std::string minLoaderVersion;
    fs::path manifestPath;
    fs::path dllPath;
};

std::optional<PluginManifest> readPluginManifest(const fs::path& modRoot)
{
    const fs::path manifestPath = modRoot / kManifestRelativePath;
    std::error_code fileError;
    if (!fs::is_regular_file(manifestPath, fileError))
    {
        return std::nullopt;
    }

    const std::string document = readSmallTextFile(manifestPath, 256 * 1024);
    if (document.empty())
    {
        logLine("INVALID_MANIFEST: path=" + utf8(manifestPath.wstring()) + " reason=empty_or_unreadable");
        return std::nullopt;
    }

    PluginManifest manifest;
    manifest.manifestPath = manifestPath;
    manifest.id = jsonStringValue(document, "id");
    manifest.name = jsonStringValue(document, "name");
    manifest.version = jsonStringValue(document, "version");
    manifest.minLoaderVersion = jsonStringValue(document, "minLoaderVersion");
    std::string dll = jsonStringValue(document, "dll");
    std::replace(dll.begin(), dll.end(), '/', '\\');

    if (!validPluginId(manifest.id))
    {
        logLine("INVALID_MANIFEST: path=" + utf8(manifestPath.wstring()) + " reason=invalid_plugin_id");
        return std::nullopt;
    }
    if (dll.empty())
    {
        logLine("INVALID_MANIFEST: id=" + manifest.id + " path=" + utf8(manifestPath.wstring()) + " reason=missing_dll");
        return std::nullopt;
    }
    if (!manifest.minLoaderVersion.empty())
    {
        if (!parseNumericVersion(manifest.minLoaderVersion))
        {
            logLine("INVALID_MANIFEST: id=" + manifest.id + " reason=invalid_minLoaderVersion value=" + manifest.minLoaderVersion);
            return std::nullopt;
        }
        if (!versionAtLeast(kLoaderVersionText, manifest.minLoaderVersion))
        {
            logLine("INCOMPATIBLE_LOADER: id=" + manifest.id + " requires=" + manifest.minLoaderVersion +
                    " current=" + kLoaderVersionText);
            return std::nullopt;
        }
    }

    const fs::path relativeDll = fs::path(std::wstring(dll.begin(), dll.end()));
    if (relativeDll.is_absolute())
    {
        logLine("INVALID_MANIFEST: id=" + manifest.id + " reason=absolute_dll_path");
        return std::nullopt;
    }
    for (const auto& component : relativeDll)
    {
        if (component == L"..")
        {
            logLine("INVALID_MANIFEST: id=" + manifest.id + " reason=parent_traversal");
            return std::nullopt;
        }
    }

    manifest.dllPath = manifestPath.parent_path() / relativeDll;
    if (!pathInsideRoot(manifest.dllPath, modRoot))
    {
        logLine("INVALID_MANIFEST: id=" + manifest.id + " reason=dll_escapes_mod_root");
        return std::nullopt;
    }

    if (lowerWide(manifest.dllPath.extension().wstring()) != L".dll")
    {
        logLine("INVALID_MANIFEST: id=" + manifest.id + " reason=dll_extension_not_allowed");
        return std::nullopt;
    }

    std::error_code error;
    if (!fs::is_regular_file(manifest.dllPath, error))
    {
        logLine("DLL_NOT_FOUND: id=" + manifest.id + " path=" + utf8(manifest.dllPath.wstring()));
        return std::nullopt;
    }
    return manifest;
}

class PluginLoader
{
public:
    PluginLoader(std::vector<fs::path> localModsRoots, std::vector<fs::path> workshopRoots)
        : m_localModsRoots(std::move(localModsRoots)),
          m_workshopRoots(std::move(workshopRoots))
    {
    }

    void processActiveMod(const ActiveUgcEntry& entry)
    {
        markActiveContent(entry.contentId);
        const std::string activeKey = lowerAscii(entry.contentId);
        if (!m_seenActiveContent.insert(activeKey).second)
        {
            return;
        }

        fs::path modRoot;
        std::string source;

        // Scrap Mechanic explicitly marks developer/local content in the Active UGC line.
        // Prefer that source so a local test copy is never shadowed by a subscribed Workshop copy.
        if (entry.isLocal)
        {
            modRoot = resolveLocalModRoot(m_localModsRoots, entry);
            source = "Local";
        }
        else
        {
            modRoot = resolveWorkshopModRoot(m_workshopRoots, entry);
            source = "Workshop";
        }

        // Defensive fallback for unusual logs/install states. The Content ID validation
        // still prevents loading a DLL from an unrelated mod.
        if (modRoot.empty() && entry.isLocal)
        {
            modRoot = resolveWorkshopModRoot(m_workshopRoots, entry);
            if (!modRoot.empty()) source = "Workshop fallback";
        }
        else if (modRoot.empty() && !entry.isLocal)
        {
            modRoot = resolveLocalModRoot(m_localModsRoots, entry);
            if (!modRoot.empty()) source = "Local fallback";
        }

        if (modRoot.empty())
        {
            logLine("WARNING: active UGC could not be resolved: content=" + entry.contentId +
                    " steam=" + std::to_string(entry.steamFileId) +
                    " local=" + (entry.isLocal ? "true" : "false"));
            return;
        }

        logLine("RESOLVED: " + source + " mod " + entry.contentId + " -> " + utf8(modRoot.wstring()));

        const fs::path manifestPath = modRoot / kManifestRelativePath;
        std::error_code manifestError;
        if (!fs::is_regular_file(manifestPath, manifestError))
        {
            logLine("TRACE: active mod has no Native/plugin.json: " + utf8(modRoot.wstring()));
            return;
        }

        const auto manifest = readPluginManifest(modRoot);
        if (!manifest)
        {
            // readPluginManifest emitted a specific INVALID_MANIFEST / DLL_NOT_FOUND /
            // INCOMPATIBLE_LOADER diagnostic.
            return;
        }

        markActivePlugin(manifest->id);

        const std::string pluginKey = lowerAscii(manifest->id);
        const auto loaded = m_loadedPlugins.find(pluginKey);
        if (loaded != m_loadedPlugins.end())
        {
            logLine("SKIPPED_DUPLICATE: id=" + manifest->id +
                    (manifest->version.empty() ? std::string{} : " version=" + manifest->version) +
                    " owner=" + utf8(loaded->second.ownerRoot.wstring()) +
                    " duplicate=" + utf8(modRoot.wstring()));
            return;
        }

        const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        SetLastError(ERROR_SUCCESS);
        HMODULE module = LoadLibraryExW(manifest->dllPath.c_str(), nullptr, flags);
        if (module == nullptr)
        {
            const DWORD error = GetLastError();
            logLine("LOAD_FAILED: id=" + manifest->id +
                    " path=" + utf8(manifest->dllPath.wstring()) +
                    " error=" + windowsErrorText(error));
            return;
        }

        LoadedPlugin loadedPlugin;
        loadedPlugin.module = module;
        loadedPlugin.ownerRoot = modRoot;
        loadedPlugin.dllPath = manifest->dllPath;
        loadedPlugin.version = manifest->version;
        m_loadedPlugins.emplace(pluginKey, std::move(loadedPlugin));
        m_modules.push_back(module);
        logLine("LOADED: id=" + manifest->id +
                (manifest->version.empty() ? std::string{} : " version=" + manifest->version) +
                " source=" + source +
                " content=" + entry.contentId +
                " dll=" + utf8(manifest->dllPath.wstring()));
    }

    void beginNewActiveBlock()
    {
        // During a UGC transition exported queries intentionally see the pending
        // block rather than the previous world. This prevents native plugins from
        // touching content that Scrap Mechanic is currently unloading.
        beginActiveStateBlock();
        m_seenActiveContent.clear();
    }

    void endActiveBlock()
    {
        endActiveStateBlock();
        logLine("ACTIVE UGC: state committed");
    }

private:
    struct LoadedPlugin
    {
        HMODULE module = nullptr;
        fs::path ownerRoot;
        fs::path dllPath;
        std::string version;
    };

    std::vector<fs::path> m_localModsRoots;
    std::vector<fs::path> m_workshopRoots;
    std::set<std::string> m_seenActiveContent;
    std::map<std::string, LoadedPlugin> m_loadedPlugins;
    std::vector<HMODULE> m_modules;
};

std::uint64_t processCreationTime()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    {
        return 0;
    }
    return fileTimeValue(creation);
}

struct LogCandidate
{
    fs::path path;
    std::uint64_t creation = 0;
};

std::optional<LogCandidate> findCurrentGameLog(const fs::path& logsDir,
                                               std::uint64_t processCreation)
{
    const fs::path pattern = logsDir / L"game-*.log";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }

    std::optional<LogCandidate> best;
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }
        const std::uint64_t creation = fileTimeValue(data.ftCreationTime);
        if (processCreation != 0 && creation + kLogCreationSlackTicks < processCreation)
        {
            continue; // stale log from a previous Scrap Mechanic process
        }
        if (!best || creation > best->creation)
        {
            best = LogCandidate{logsDir / data.cFileName, creation};
        }
    }
    while (FindNextFileW(find, &data));

    FindClose(find);
    return best;
}

class ActiveUgcParser
{
public:
    explicit ActiveUgcParser(PluginLoader& loader)
        : m_loader(loader)
    {
    }

    void line(std::string value)
    {
        if (!value.empty() && value.back() == '\r')
        {
            value.pop_back();
        }

        if (value.find("--------------- Active UGC Content ---------------") != std::string::npos)
        {
            m_inBlock = true;
            m_hadEntries = false;
            m_loader.beginNewActiveBlock();
            logLine("ACTIVE UGC: block detected");
            return;
        }

        if (!m_inBlock)
        {
            return;
        }

        const auto entry = parseActiveUgcLine(value);
        if (entry)
        {
            m_hadEntries = true;
            logLine("ACTIVE UGC: content=" + entry->contentId +
                    " steam=" + std::to_string(entry->steamFileId) +
                    " local=" + (entry->isLocal ? "true" : "false"));
            m_loader.processActiveMod(*entry);
            return;
        }

        if (value.find_first_not_of(" \t") == std::string::npos)
        {
            m_inBlock = false;
            m_loader.endActiveBlock();
        }
    }

private:
    PluginLoader& m_loader;
    bool m_inBlock = false;
    bool m_hadEntries = false;
};

DWORD WINAPI loaderWorker(LPVOID)
{
    if (!isScrapMechanicProcess())
    {
        return 0;
    }

    resetLog();
    logLine("START: Universal Plugin Loader 0.4.0");
    logLine("INFO: DLLs are considered only for ACTIVE Local/Workshop mods");
    logLine("INFO: plugin manifest path is Native/plugin.json");
    logLine("INFO: manifest supports optional minLoaderVersion");

    if (commandLineDisablesLoader())
    {
        logLine("DISABLED: -noinject or -noupl command-line switch");
        return 0;
    }

    const std::vector<fs::path> localModsRoots = discoverLocalModsRoots();
    const std::vector<fs::path> workshopRoots = discoverWorkshopRoots();
    const fs::path logsDir = gameDirectory() / L"Logs";

    if (localModsRoots.empty())
    {
        logLine("INFO: no local Mods roots discovered");
    }
    else
    {
        for (const fs::path& root : localModsRoots)
        {
            logLine("INFO: Local Mods root = " + utf8(root.wstring()));
        }
    }

    if (workshopRoots.empty())
    {
        logLine("WARNING: no Workshop roots discovered");
    }
    else
    {
        for (const fs::path& root : workshopRoots)
        {
            logLine("INFO: Workshop root = " + utf8(root.wstring()));
        }
    }

    logLine("INFO: Logs directory = " + utf8(logsDir.wstring()));

    PluginLoader pluginLoader(localModsRoots, workshopRoots);
    ActiveUgcParser parser(pluginLoader);
    const std::uint64_t createdAt = processCreationTime();

    std::optional<LogCandidate> logCandidate;
    while (!logCandidate)
    {
        logCandidate = findCurrentGameLog(logsDir, createdAt);
        if (!logCandidate)
        {
            Sleep(kLogPollMs);
        }
    }

    logLine("INFO: following current game log = " + utf8(logCandidate->path.wstring()));

    HANDLE file = INVALID_HANDLE_VALUE;
    while (file == INVALID_HANDLE_VALUE)
    {
        file = CreateFileW(logCandidate->path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            Sleep(kLogPollMs);
        }
    }

    std::string pending;
    std::vector<char> buffer(64 * 1024);
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF)
            {
                logLine("ERROR: failed reading game log: " + windowsErrorText(error));
                CloseHandle(file);
                return 2;
            }
        }

        if (bytesRead == 0)
        {
            Sleep(kLogPollMs);
            continue;
        }

        pending.append(buffer.data(), bytesRead);
        std::size_t lineStart = 0;
        for (;;)
        {
            const std::size_t newline = pending.find('\n', lineStart);
            if (newline == std::string::npos)
            {
                pending.erase(0, lineStart);
                break;
            }
            parser.line(pending.substr(lineStart, newline - lineStart));
            lineStart = newline + 1;
        }
    }
}

bool loadOriginalRuntime()
{
    // Keep this bootstrap WinAPI-only. It runs before our forwarded VC runtime
    // entry points are guaranteed to be usable.
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(
        g_selfModule, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path))
    {
        MessageBoxW(nullptr, L"Universal Plugin Loader could not locate itself.",
                    L"Universal Plugin Loader", MB_OK | MB_ICONERROR);
        return false;
    }

    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr)
    {
        MessageBoxW(nullptr, L"Universal Plugin Loader could not locate its Release directory.",
                    L"Universal Plugin Loader", MB_OK | MB_ICONERROR);
        return false;
    }
    *(slash + 1) = L'\0';
    if (wcscat_s(path, L"vcruntime140_1_.dll") != 0)
    {
        return false;
    }

    g_realRuntime = LoadLibraryW(path);
    if (g_realRuntime == nullptr)
    {
        MessageBoxW(nullptr,
                    L"vcruntime140_1_.dll is missing. Rename the original runtime before installing Universal Plugin Loader.",
                    L"Universal Plugin Loader", MB_OK | MB_ICONERROR);
        return false;
    }

    for (std::size_t i = 0; i < std::size(kRuntimeExportNames); ++i)
    {
        g_runtimeExports[i] = GetProcAddress(g_realRuntime, kRuntimeExportNames[i]);
        if (g_runtimeExports[i] == nullptr)
        {
            MessageBoxA(nullptr, kRuntimeExportNames[i],
                        "Universal Plugin Loader: runtime export not found",
                        MB_OK | MB_ICONERROR);
            return false;
        }
    }
    return true;
}

} // namespace

extern "C"
{
#define UPL_FORWARD(func_name, index, ...) \
    reinterpret_cast<decltype(&func_name)>(g_runtimeExports[index])(__VA_ARGS__)

__declspec(dllexport) unsigned int UPL_GetApiVersion()
{
    return 1;
}

__declspec(dllexport) int UPL_IsContentActive(const char* contentId)
{
    if (contentId == nullptr || *contentId == '\0')
    {
        return 0;
    }
    return isContentActive(contentId) ? 1 : 0;
}

__declspec(dllexport) int UPL_IsPluginActive(const char* pluginId)
{
    if (pluginId == nullptr || *pluginId == '\0')
    {
        return 0;
    }
    return isPluginActive(pluginId) ? 1 : 0;
}

__declspec(dllexport) __int64 __CxxFrameHandler4(__int64 a1, __int64 a2, int a3, __int64 a4)
{
    return UPL_FORWARD(__CxxFrameHandler4, 0, a1, a2, a3, a4);
}

__declspec(dllexport) void __NLG_Dispatch2()
{
    UPL_FORWARD(__NLG_Dispatch2, 1);
}

__declspec(dllexport) void __NLG_Return2()
{
    UPL_FORWARD(__NLG_Return2, 2);
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
    {
        return TRUE;
    }

    g_selfModule = module;
    DisableThreadLibraryCalls(module);

    // The real VC runtime must be available synchronously because the game imports
    // these forwarded functions from us. Workshop discovery is deferred to a worker
    // thread so it does not run under the Windows loader lock.
    if (!loadOriginalRuntime())
    {
        return FALSE;
    }

    HANDLE thread = CreateThread(nullptr, 0, loaderWorker, nullptr, 0, nullptr);
    if (thread != nullptr)
    {
        CloseHandle(thread);
    }
    return TRUE;
}
