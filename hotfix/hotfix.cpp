// Loads the current room-rejoin hooks into an already running isaac-ng.exe.
// Every call site the old winmm.dll redirected is taken over, so the old
// room-rejoin hooks stop running. Online checkpoint hooks stay with the old DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cwchar>

using OnlineEvent = void (*)(uint64_t, const char*, const char*, unsigned, int, int);
void EnableRoomRejoin(OnlineEvent logger);
void SetRoomRejoinLogger(OnlineEvent logger);

namespace {

using GameLog = void (__cdecl *)(int, const char*, ...);
constexpr uintptr_t GameLogRva = 0x6112C0;
constexpr DWORD MaxThreads = 512;

DWORD threadIds[MaxThreads];
HANDLE threads[MaxThreads];

void SilentEvent(uint64_t, const char*, const char*, unsigned, int, int) {}

void GameLogEvent(uint64_t peer, const char* event, const char* reason, unsigned frame, int from, int to) {
    const auto log = reinterpret_cast<GameLog>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + GameLogRva);
    log(1, "[BetterCoop] [Frame %u] %s: %s (from=%d to=%d peer=%llu)\n", frame, event, reason, from, to, peer);
}

DWORD WINAPI Install(void*) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = length && length < MAX_PATH ? std::wcsrchr(path, L'\\') : nullptr;
    if (!name || _wcsicmp(name + 1, L"isaac-ng.exe") != 0) {
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

    for (DWORD i = 0; i < opened; ++i) {
        SuspendThread(threads[i]);
    }
    EnableRoomRejoin(SilentEvent);
    for (DWORD i = 0; i < opened; ++i) {
        ResumeThread(threads[i]);
        CloseHandle(threads[i]);
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
