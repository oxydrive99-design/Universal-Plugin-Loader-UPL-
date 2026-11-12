# Shared Lua transport (UPL 0.4.2)

Two DLLs independently detouring the same `lua_pcall` target can bypass each other's observers. This previously left Creation Shield in FALLBACK after Engine Blackout R1 loaded, even though UPL had successfully loaded both DLLs.

UPL now owns the detours. Migrated plugins register observers instead of installing their own hooks. The loader watches the EXE's named Lua import without replacing the import pointer, keeps an immutable continuation for each observed target, and passes a synchronous call through all registered observers before reaching the existing continuation. Nested routes are forwarded without repeating the observer chain.

Existing discovery, minimum-loader checks and active-content APIs are preserved. The transport remains dormant until a plugin registers an observer. It does not make arbitrary legacy plugins compatible: plugins targeting the same addresses must migrate to the shared API. KeybindBridge retains its existing hooks; the shared continuation preserves the existing route where it is observed.

## Exports and registration

Use [upl_lua_transport.h](../include/upl_lua_transport.h) with `GetProcAddress` on the already loaded `vcruntime140_1.dll`:

- `UPL_GetLuaTransportVersion()` returns `1`.
- `UPL_RegisterLuaPcallObserver(id, observer)` returns `1` on success, `0` on failure.
- `UPL_GetApiVersion()` remains `1`; it describes the existing active-content API, not the new transport.

Registration uses a stable plugin identifier. Repeating the same ID and pointer is idempotent. Replacing the callback for an existing ID fails. Empty IDs, null pointers, IDs longer than 160 bytes and more than 32 observers are rejected. The owning DLL is pinned for process lifetime; unloading or unregistering observers is not supported. Registration order is the observer order. Calls take a snapshot, so a newly registered observer becomes visible on a subsequent call.

```cpp
int __cdecl observe(UPL_LuaPcall next, lua_State* state,
                    int nargs, int nresults, int errfunc)
{
    // Check UPL_IsContentActive/UPL_IsPluginActive before touching mod state.
    // Inspect the function and its environment while preserving the Lua stack.
    const int result = next(state, nargs, nresults, errfunc);
    // Patch callbacks after successful script execution, if needed.
    return result;
}

HMODULE loader = GetModuleHandleW(L"vcruntime140_1.dll");
if (!loader) return;
auto version = reinterpret_cast<UPL_GetLuaTransportVersionFn>(
    GetProcAddress(loader, "UPL_GetLuaTransportVersion"));
auto registerObserver = reinterpret_cast<UPL_RegisterLuaPcallObserverFn>(
    GetProcAddress(loader, "UPL_RegisterLuaPcallObserver"));
if (!version || version() != UPL_LUA_TRANSPORT_VERSION || !registerObserver)
    return; // Log incompatibility; do not install a competing fallback hook.
if (!registerObserver("example.mod.lua-observer", observe))
    return; // Log registration failure.
```

Observers must forward the original call with unchanged arguments and return its result. The continuation may also execute an injected chunk synchronously while the observer is active. Do not save it, call it from another thread, or use it after returning. Each continuation invocation restores its dispatch position, so an injected chunk does not consume another observer's place in the original call.

Lua states, result counts, error handlers and the stack remain the plugin's responsibility. C++ exceptions must not escape through Lua's C ABI. The transport context is thread-local and its C++ scopes restore state on return/unwind. Calls re-entering the route while dispatch is active use the existing continuation without re-inspecting the environment, matching the migrated plugins' outermost-call behavior.

Use `"minLoaderVersion": "0.4.2"` in migrated plugins' `Native/plugin.json`. Keep inactive observers transparent: a mapped DLL does not imply that its mod is enabled in the current world.

## Diagnostics and validation

The loader reports `LUA: shared transport initialized`, `LUA: registered observers=N`, and the observed route count. At most 16 route targets are retained. Failure or exhaustion is logged; existing routes are retained. A different route is picked up by the normal loader polling loop.

Run `tests\BUILD_TESTS.bat` from an x64 Developer Command Prompt. Tests cover both observer orders, 160,000 concurrent calls, nested routes, repeated continuations, context restoration after a C++ test exception, idempotent registration, real x64 MinHook detours and a simulated intermediate KeybindBridge continuation.

Integration validation outside this repository used the migrated CreationShieldBridge 0.7.7 and Engine Blackout R2 DLLs with the game's Lua 5.1: API injection, world callbacks, both engine types' blackout/restoration, both load orders, three fresh Lua states and inactive-plugin gating. In-game validation then confirmed both observers registered, the shield's call count continued increasing with `api=1 worlds=2`, both engine callbacks were patched, and the lighting module reported no faults. Renderer behavior, multiplayer and unrelated legacy DLLs still require their own validation.
