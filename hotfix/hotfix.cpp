// Loads the current room-rejoin hooks into an already running isaac-ng.exe.
// Every call site the old winmm.dll redirected is taken over, so the old
// room-rejoin hooks stop running. Online checkpoint hooks stay with the old DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cwchar>
#include "../game_build.h"

using OnlineEvent = void (*)(uint64_t, const char*, const char*, unsigned, int, int);
bool EnableRoomRejoin(OnlineEvent logger, bool replaceExisting);
void SetRoomRejoinLogger(OnlineEvent logger);

namespace {

using GameLog = void (__cdecl *)(int, const char*, ...);
constexpr DWORD MaxThreads = 512;

DWORD threadIds[MaxThreads];
HANDLE threads[MaxThreads];

void SilentEvent(uint64_t, const char*, const char*, unsigned, int, int) {}

void GameLogEvent(uint64_t peer, const char* event, const char* reason, unsigned frame, int from, int to) {
    const auto log = reinterpret_cast<GameLog>(
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + IsaacRelease::GameLogRva);
    log(1, "[BetterCoop] [Frame %u] %s: %s (from=%d to=%d peer=%llu)\n", frame, event, reason, from, to, peer);
}

DWORD WINAPI Install(void*) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = length && length < MAX_PATH ? std::wcsrchr(path, L'\\') : nullptr;
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!name || _wcsicmp(name + 1, L"isaac-ng.exe") != 0 ||
        !IsaacRelease::MatchesLoadedImage(image) || !IsaacRelease::MatchesGameFile(path)) {
        return 1;
    }

    // Collect and open the threads first: nothing below may allocate while
    // the game threads are frozen (one of them could hold the heap lock).
    DWORD count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 2;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok && count < MaxThreads; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) {
            threadIds[count++] = entry.th32ThreadID;
        }
    }
    CloseHandle(snapshot);
    DWORD opened = 0;
    for (DWORD i = 0; i < count; ++i) {
        if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadIds[i])) {
            threads[opened++] = thread;
        }
    }

    DWORD suspended = 0;
    for (; suspended < opened; ++suspended) {
        if (SuspendThread(threads[suspended]) == static_cast<DWORD>(-1)) {
            break;
        }
    }
    const bool installed = opened == count && suspended == opened && EnableRoomRejoin(SilentEvent, true);
    for (DWORD i = 0; i < suspended; ++i) {
        ResumeThread(threads[i]);
    }
    for (DWORD i = 0; i < opened; ++i) {
        CloseHandle(threads[i]);
    }

    if (!installed) {
        GameLogEvent(0, "room_rejoin_unavailable",
                     suspended == count ? "patch_call" : "suspend_threads", 0, -1, -1);
        return 3;
    }
    SetRoomRejoinLogger(GameLogEvent);
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Pin the module: the game's call sites will point into it.
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        if (const HANDLE thread = CreateThread(nullptr, 0, Install, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
