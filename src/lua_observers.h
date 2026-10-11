#pragma once
#include "../include/upl_lua_transport.h"
#include <array>
#include <mutex>
#include <string>

struct lua_State;
namespace upl_lua {
using Pcall = UPL_LuaPcall;
using Observer = UPL_LuaObserver;
constexpr unsigned apiVersion = UPL_LUA_TRANSPORT_VERSION;
constexpr std::size_t maxObservers = 32;
struct Entry { std::string id; Observer callback = nullptr; };
struct Snapshot { std::array<Observer, maxObservers> callbacks{}; std::size_t count = 0; };
class Registry {
    std::mutex mutex;
    std::array<Entry, maxObservers> entries{};
    std::size_t count = 0;
public:
    int add(const char* id, Observer callback) {
        if (!id || !*id || !callback) return 0;
        const std::string key(id);
        if (key.size() > 160) return 0;
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < count; ++i)
            if (entries[i].id == key) return entries[i].callback == callback ? 1 : 0;
        if (count == maxObservers) return 0;
        entries[count++] = {key, callback};
        return 1;
    }
    Snapshot snapshot() {
        std::lock_guard lock(mutex);
        Snapshot result;
        result.count = count;
        for (std::size_t i = 0; i < count; ++i) result.callbacks[i] = entries[i].callback;
        return result;
    }
};
struct Frame { Snapshot observers; Pcall terminal; std::size_t cursor = 0; };
inline thread_local Frame* activeFrame = nullptr;
inline int next(lua_State* state, int arguments, int results, int errorFunction) {
    Frame* frame = activeFrame;
    if (!frame || !frame->terminal) return -1;
    if (frame->cursor == frame->observers.count)
        return frame->terminal(state, arguments, results, errorFunction);
    const auto index = frame->cursor++;
    struct Restore { Frame& frame; std::size_t index; ~Restore() { frame.cursor = index; } } restore{*frame, index};
    return frame->observers.callbacks[index](next, state, arguments, results, errorFunction);
}
inline int dispatch(Registry& registry, Pcall terminal, lua_State* state, int arguments, int results, int errorFunction) {
    if (!terminal) return -1;
    // A route may pass through an older route's trampoline. Inspect only once.
    // Real recursive Lua calls retain the same behavior as plugin CallFrame.
    if (activeFrame) return terminal(state, arguments, results, errorFunction);
    Frame frame{registry.snapshot(), terminal};
    struct Restore { Frame* previous; ~Restore() { activeFrame = previous; } } restore{activeFrame};
    activeFrame = &frame;
    return next(state, arguments, results, errorFunction);
}
}
