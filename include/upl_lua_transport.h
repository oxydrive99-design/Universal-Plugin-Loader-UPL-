#pragma once

// Resolve these exports from the already-loaded vcruntime140_1.dll using
// GetProcAddress. Keep the proxy's VC runtime import library out of plugins.
#ifdef _WIN32
#define UPL_LUA_CALL __cdecl
#else
#define UPL_LUA_CALL
#endif

struct lua_State;
typedef int (UPL_LUA_CALL *UPL_LuaPcall)(struct lua_State*, int, int, int);
typedef int (UPL_LUA_CALL *UPL_LuaObserver)(UPL_LuaPcall, struct lua_State*, int, int, int);
typedef unsigned int (UPL_LUA_CALL *UPL_GetLuaTransportVersionFn)(void);
typedef int (UPL_LUA_CALL *UPL_RegisterLuaPcallObserverFn)(const char*, UPL_LuaObserver);
#define UPL_LUA_TRANSPORT_VERSION 1u

// The supplied continuation is valid synchronously inside the observer only.
// Forward the original call and return its result. Never retain the continuation
// or allow C++ exceptions to escape through the Lua ABI.
