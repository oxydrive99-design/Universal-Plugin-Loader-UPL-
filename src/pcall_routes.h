#pragma once

#include <atomic>
#include <cstddef>

struct lua_State;

namespace shield_transport
{
using Pcall = int (*)(lua_State*, int, int, int);
constexpr std::size_t maxRoutes = 16;

// Each inline detour has its own immutable continuation. A later plugin can
// replace the EXE import without changing continuations in an existing chain.
struct Routes
{
    std::atomic<Pcall> continuations[maxRoutes]{};
    void* targets[maxRoutes]{}; // only accessed by the installer thread
    std::size_t count = 0;

    bool contains(void* target) const
    {
        for (std::size_t i = 0; i < count; ++i)
            if (targets[i] == target) return true;
        return false;
    }
};

// Nested routes forward through their own trampoline but only the outer route
// inspects/patches the Lua environment. Keep the matching continuation available
// when executing our injected chunk, and restore it on every return path.
struct CallFrame
{
    int& depth;
    Pcall& active;
    Pcall previous;
    bool outermost;

    CallFrame(int& depthRef, Pcall& activeRef, Pcall continuation)
        : depth(depthRef), active(activeRef), previous(activeRef),
          outermost(++depthRef == 1)
    {
        active = continuation;
    }
    ~CallFrame()
    {
        active = previous;
        --depth;
    }
    CallFrame(const CallFrame&) = delete;
    CallFrame& operator=(const CallFrame&) = delete;
};
}
