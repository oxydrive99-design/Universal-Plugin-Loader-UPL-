#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "MinHook.h"
#include "pcall_routes.h"
#include "lua_observers.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
namespace {
using LuaPcall = upl_lua::Pcall;
shield_transport::Routes g_routes;
upl_lua::Registry g_observers;
void (*g_transportLog)(const std::string&) = nullptr;
template <std::size_t Site>
int routedLuaPcall(lua_State* state, int arguments, int results, int errorFunction) {
    auto continuation = g_routes.continuations[Site].load(std::memory_order_acquire);
    return upl_lua::dispatch(g_observers, continuation, state, arguments, results, errorFunction);
}
constexpr LuaPcall kRouteDetours[shield_transport::maxRoutes] = {
    routedLuaPcall<0>, routedLuaPcall<1>, routedLuaPcall<2>, routedLuaPcall<3>,
    routedLuaPcall<4>, routedLuaPcall<5>, routedLuaPcall<6>, routedLuaPcall<7>,
    routedLuaPcall<8>, routedLuaPcall<9>, routedLuaPcall<10>, routedLuaPcall<11>,
    routedLuaPcall<12>, routedLuaPcall<13>, routedLuaPcall<14>, routedLuaPcall<15>
};
void* gamePcallTarget()
{
#ifdef UPL_LUA_TRANSPORT_TEST
    return fixtureTarget;
#else
    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) return nullptr;

    auto* base = reinterpret_cast<std::byte*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor)
    {
        const char* library = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(library, "lua51.dll") != 0 && _stricmp(library, "luajit.dll") != 0)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        IMAGE_THUNK_DATA* names = nullptr;
        if (descriptor->OriginalFirstThunk != 0)
            names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);

        for (std::size_t i = 0; thunk[i].u1.Function != 0; ++i)
        {
            bool namedPcall = false;
            if (names != nullptr && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
            {
                auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                namedPcall = std::strcmp(reinterpret_cast<const char*>(imported->Name), "lua_pcall") == 0;
            }
            if (!namedPcall) continue;

            // Read the actual EXE route. Never write the import or adopt a random
            // secondary module's unused lua_pcall import.
            return reinterpret_cast<void*>(std::atomic_ref<ULONG_PTR>(thunk[i].u1.Function)
                .load(std::memory_order_acquire));
        }
    }
    return nullptr;
#endif
}

bool observeGameRoute()
{
    void* target = gamePcallTarget();
    if (target == nullptr) return false;
    if (g_routes.contains(target)) return true;
    if (g_routes.count == shield_transport::maxRoutes)
    {
        g_transportLog("ERROR: Lua route capacity exhausted; existing routes kept intact");
        return false;
    }

    const std::size_t site = g_routes.count;
    void* trampoline = nullptr;
    MH_STATUS result = MH_CreateHook(target, reinterpret_cast<void*>(kRouteDetours[site]), &trampoline);
    if (result != MH_OK || trampoline == nullptr)
    {
        g_transportLog("ERROR: Lua route hook creation failed: " + std::to_string(result));
        return false;
    }
    g_routes.continuations[site].store(reinterpret_cast<LuaPcall>(trampoline), std::memory_order_release);
    result = MH_EnableHook(target);
    if (result != MH_OK)
    {
        MH_RemoveHook(target);
        g_routes.continuations[site].store(nullptr, std::memory_order_release);
        g_transportLog("ERROR: Lua route hook activation failed: " + std::to_string(result));
        return false;
    }
    g_routes.targets[site] = target;
    ++g_routes.count;
    g_transportLog("OK: EXE lua_pcall route observed (#" + std::to_string(g_routes.count) +
            "); import left unchanged, private continuation retained");
    return true;
}


} // namespace
void pollLuaTransport(void (*logger)(const std::string&)) {
    g_transportLog = logger;
    const auto observers = g_observers.snapshot();
    if (!observers.count) return;
    static bool initialized = false;
    if (!initialized) {
        const auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            static bool reported = false;
            if (!reported) { logger("ERROR: shared Lua transport initialization failed"); reported = true; }
            return;
        }
        initialized = true;
        logger("LUA: shared transport initialized; plugins do not install competing hooks");
    }
    static std::size_t reportedCount = 0;
    if (reportedCount != observers.count) {
        reportedCount = observers.count;
        logger("LUA: registered observers=" + std::to_string(reportedCount));
    }
    observeGameRoute();
}
extern "C" __declspec(dllexport) unsigned UPL_GetLuaTransportVersion() { return upl_lua::apiVersion; }
extern "C" __declspec(dllexport) int UPL_RegisterLuaPcallObserver(const char* id, upl_lua::Observer observer) {
    if (!id || !*id || !observer) return 0;
    HMODULE owner = nullptr;
    // UPL never unloads registered observers: continuations may still be running.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(observer), &owner)) return 0;
    return g_observers.add(id, observer);
}
