#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/lua_observers.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace upl_lua;
std::atomic<int> firstCalls{0}, secondCalls{0}, originalCalls{0};
int original(lua_State*, int a, int b, int c) { ++originalCalls; return a + b + c; }
int first(Pcall call, lua_State* s, int a, int b, int c) { ++firstCalls; return call(s,a,b,c); }
int second(Pcall call, lua_State* s, int a, int b, int c) { ++secondCalls; return call(s,a,b,c); }
int twice(Pcall call, lua_State* s, int a, int b, int c) { return call(s,a,b,c) + call(s,a,b,c); }
int failing(Pcall, lua_State*, int, int, int) { throw std::runtime_error("fixture"); }
Registry* nestedRegistry = nullptr;
int nestedOriginal(lua_State* s, int a, int b, int c) { return dispatch(*nestedRegistry, original, s,a,b,c); }

// Exercise the actual Windows detour installer against executable fixtures.
void* fixtureTarget = nullptr;
#define UPL_LUA_TRANSPORT_TEST
#include "../src/lua_transport.cpp"
void logFixture(const std::string& value) { std::cout << value << '\n'; }
Pcall previousTarget = nullptr;
__declspec(noinline) int keybindFixture(lua_State* s,int a,int b,int c) { return previousTarget(s,a,b,c) + 1; }

int main() {
    for (bool reverse : {false,true}) {
        Registry registry;
        assert(registry.add(reverse ? "engine" : "shield", reverse ? second : first));
        assert(registry.add(reverse ? "shield" : "engine", reverse ? first : second));
        const int baseline = originalCalls.load();
        assert(dispatch(registry, original, nullptr,1,2,3) == 6);
        assert(originalCalls == baseline + 1);
        nestedRegistry = &registry;
        const int firstBaseline = firstCalls.load(), secondBaseline = secondCalls.load();
        assert(dispatch(registry, nestedOriginal, nullptr,1,2,3) == 6);
        assert(firstCalls == firstBaseline + 1 && secondCalls == secondBaseline + 1);
        assert(registry.add("shield", first));
        assert(!registry.add("shield", second));
        const int threadedBaseline = originalCalls.load();
        std::vector<std::thread> threads;
        for (int i=0; i<8; ++i) threads.emplace_back([&] {
            for (int j=0;j<10000;++j) assert(dispatch(registry, original, reinterpret_cast<lua_State*>(static_cast<std::uintptr_t>(j+1)),1,2,3)==6);
        });
        for (auto& thread : threads) thread.join();
        assert(originalCalls == threadedBaseline + 80000);
    }
    Registry repeat;
    assert(repeat.add("injection", twice) && repeat.add("second",second));
    const int secondBaseline = secondCalls.load();
    assert(dispatch(repeat,original,nullptr,1,2,3)==12 && secondCalls == secondBaseline+2);
    Registry failure;
    assert(failure.add("throw",failing));
    try { dispatch(failure,original,nullptr,1,2,3); assert(false); } catch (const std::runtime_error&) {}
    assert(activeFrame == nullptr);
    assert(!repeat.add(nullptr,first) && !repeat.add("bad",nullptr));

    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    assert(code);
    // mov eax, 99; ret; sufficient readable padding for MinHook relocation.
    std::fill_n(code,4096,static_cast<unsigned char>(0x90)); code[0]=0xb8; code[1]=99; code[2]=code[3]=code[4]=0; code[5]=0xc3;
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    fixtureTarget=code;
    assert(UPL_GetLuaTransportVersion()==1);
    assert(UPL_RegisterLuaPcallObserver("shield",first));
    assert(UPL_RegisterLuaPcallObserver("engine",second));
    pollLuaTransport(logFixture);
    auto call=reinterpret_cast<Pcall>(fixtureTarget);
    const int firstStart=firstCalls.load(), secondStart=secondCalls.load();
    assert(call(nullptr,0,0,0)==99);
    assert(firstCalls==firstStart+1 && secondCalls==secondStart+1);
    previousTarget=call;
    fixtureTarget=reinterpret_cast<void*>(keybindFixture);
    pollLuaTransport(logFixture);
    call=reinterpret_cast<Pcall>(fixtureTarget);
    const int f=firstCalls.load(), e=secondCalls.load();
    assert(call(nullptr,0,0,0)==100);
    assert(firstCalls==f+1 && secondCalls==e+1);
    for(int i=0;i<100;++i) pollLuaTransport(logFixture);
    assert(g_routes.count==2);
    std::cout << "PASS: both observer orders, 160000 concurrent calls, nested routes, repeated continuation, error unwind, real x64 MinHook routes and Keybind chain\n";
}
