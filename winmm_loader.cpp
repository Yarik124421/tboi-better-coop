#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include "game_build.h"

using OnlineEvent = void (*)(uint64_t, const char*, const char*, unsigned, int, int);
bool EnableRoomRejoin(OnlineEvent logger, bool replaceExisting);
void EnableOnlineCheckpoint(OnlineEvent logger, bool resume);

namespace {

INIT_ONCE winmmOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE featuresOnce = INIT_ONCE_STATIC_INIT;
HMODULE systemWinmm;

BOOL CALLBACK LoadSystemWinmm(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    const UINT size = GetSystemDirectoryW(path, _countof(path));
    if (size == 0 || size >= _countof(path) ||
        wcscpy_s(path + size, _countof(path) - size, L"\\winmm.dll") != 0) {
        return FALSE;
    }
    systemWinmm = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return systemWinmm != nullptr;
}

template <typename Function>
Function WinmmExport(const char* name) {
    if (!InitOnceExecuteOnce(&winmmOnce, LoadSystemWinmm, nullptr, nullptr)) {
        return nullptr;
    }
    return reinterpret_cast<Function>(GetProcAddress(systemWinmm, name));
}

struct LaunchOptions {
    bool roomRejoin;
    bool saveOnline;
    bool resumeOnline;
};

LaunchOptions ParseLaunchOptions(int count, wchar_t* const* arguments) {
    LaunchOptions options{};
    for (int i = 1; i < count; ++i) {
        const wchar_t* arg = arguments[i];
        options.roomRejoin |= _wcsicmp(arg, L"-RoomRejoin") == 0;
        options.saveOnline |= _wcsicmp(arg, L"-SaveOnline") == 0;
        options.resumeOnline |= _wcsicmp(arg, L"-ResumeOnline") == 0;
    }
    return options;
}

using GameLog = void (__cdecl *)(int, const char*, ...);

// Mirror mod events into the game's own log.txt (and its desync dumps).
void GameLogEvent(uint64_t peer, const char* event, const char* reason, unsigned frame, int from, int to) {
    const auto log = reinterpret_cast<GameLog>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + IsaacRelease::GameLogRva);
    log(1, "[BetterCoop] [Frame %u] %s: %s (from=%d to=%d peer=%llu)\n", frame, event, reason, from, to, peer);
}

BOOL CALLBACK InitializeFeatures(PINIT_ONCE, PVOID, PVOID*) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return TRUE;
    }
    const auto options = ParseLaunchOptions(count, arguments);
    LocalFree(arguments);
    if (!options.roomRejoin && !options.saveOnline && !options.resumeOnline) {
        return TRUE;
    }

    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (length == 0 || length >= _countof(path)) {
        return TRUE;
    }
    const auto* filename = std::wcsrchr(path, L'\\');
    if (!filename || _wcsicmp(filename + 1, L"isaac-ng.exe") != 0) {
        return TRUE;
    }
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!IsaacRelease::MatchesLoadedImage(image) || !IsaacRelease::MatchesGameFile(path)) {
        MessageBoxW(nullptr, L"This Isaac build is not supported by Room Rejoin. The game will start without the mod.",
                    L"Isaac Room Rejoin", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    if (options.roomRejoin) {
        EnableRoomRejoin(GameLogEvent, false);
    }
    if (options.saveOnline || options.resumeOnline) {
        EnableOnlineCheckpoint(GameLogEvent, options.resumeOnline);
    }
    return TRUE;
}

}

// The game calls this timer query during its normal startup, outside DllMain.
extern "C" MMRESULT WINAPI Mod_timeGetDevCaps(LPTIMECAPS caps, UINT size) {
    static const auto original = WinmmExport<decltype(&timeGetDevCaps)>("timeGetDevCaps");
    const MMRESULT result = original ? original(caps, size) : TIMERR_NOCANDO;
    InitOnceExecuteOnce(&featuresOnce, InitializeFeatures, nullptr, nullptr);
    return result;
}

#define WINMM_FORWARD(result, name, parameters, arguments, failure) \
    extern "C" result WINAPI Mod_##name parameters { \
        static const auto original = WinmmExport<decltype(&name)>(#name); \
        return original ? original arguments : (failure); \
    }

WINMM_FORWARD(MMRESULT, timeBeginPeriod, (UINT period), (period), TIMERR_NOCANDO)
WINMM_FORWARD(MMRESULT, timeEndPeriod, (UINT period), (period), TIMERR_NOCANDO)
WINMM_FORWARD(DWORD, timeGetTime, (), (), GetTickCount())
WINMM_FORWARD(MMRESULT, timeKillEvent, (UINT id), (id), TIMERR_NOCANDO)
WINMM_FORWARD(MMRESULT, timeSetEvent, (UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user, UINT flags),
              (delay, resolution, callback, user, flags), 0)

// Steam loads its 32-bit client inside Isaac and uses these WinMM APIs.
WINMM_FORWARD(MMRESULT, mixerClose, (HMIXER handle), (handle), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerGetControlDetailsA, (HMIXEROBJ handle, LPMIXERCONTROLDETAILS details, DWORD flags),
              (handle, details, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerGetDevCapsA, (UINT_PTR device, LPMIXERCAPSA caps, UINT size),
              (device, caps, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerGetID, (HMIXEROBJ handle, UINT* id, DWORD flags),
              (handle, id, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerGetLineControlsA, (HMIXEROBJ handle, LPMIXERLINECONTROLSA controls, DWORD flags),
              (handle, controls, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerGetLineInfoA, (HMIXEROBJ handle, LPMIXERLINEA line, DWORD flags),
              (handle, line, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerOpen, (LPHMIXER handle, UINT device, DWORD_PTR callback, DWORD_PTR instance, DWORD flags),
              (handle, device, callback, instance, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, mixerSetControlDetails, (HMIXEROBJ handle, LPMIXERCONTROLDETAILS details, DWORD flags),
              (handle, details, flags), MMSYSERR_NODRIVER)

WINMM_FORWARD(MMRESULT, waveInAddBuffer, (HWAVEIN handle, LPWAVEHDR header, UINT size),
              (handle, header, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInClose, (HWAVEIN handle), (handle), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInGetDevCapsW, (UINT_PTR device, LPWAVEINCAPSW caps, UINT size),
              (device, caps, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(UINT, waveInGetNumDevs, (), (), 0)
WINMM_FORWARD(MMRESULT, waveInMessage, (HWAVEIN handle, UINT message, DWORD_PTR param1, DWORD_PTR param2),
              (handle, message, param1, param2), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInOpen, (LPHWAVEIN handle, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback,
              DWORD_PTR instance, DWORD flags), (handle, device, format, callback, instance, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInPrepareHeader, (HWAVEIN handle, LPWAVEHDR header, UINT size),
              (handle, header, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInReset, (HWAVEIN handle), (handle), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInStart, (HWAVEIN handle), (handle), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInStop, (HWAVEIN handle), (handle), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveInUnprepareHeader, (HWAVEIN handle, LPWAVEHDR header, UINT size),
              (handle, header, size), MMSYSERR_NODRIVER)

WINMM_FORWARD(MMRESULT, waveOutClose, (HWAVEOUT handle), (handle), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveOutGetDevCapsW, (UINT_PTR device, LPWAVEOUTCAPSW caps, UINT size),
              (device, caps, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(UINT, waveOutGetNumDevs, (), (), 0)
WINMM_FORWARD(MMRESULT, waveOutMessage, (HWAVEOUT handle, UINT message, DWORD_PTR param1, DWORD_PTR param2),
              (handle, message, param1, param2), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveOutOpen, (LPHWAVEOUT handle, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback,
              DWORD_PTR instance, DWORD flags), (handle, device, format, callback, instance, flags), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveOutPrepareHeader, (HWAVEOUT handle, LPWAVEHDR header, UINT size),
              (handle, header, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveOutUnprepareHeader, (HWAVEOUT handle, LPWAVEHDR header, UINT size),
              (handle, header, size), MMSYSERR_NODRIVER)
WINMM_FORWARD(MMRESULT, waveOutWrite, (HWAVEOUT handle, LPWAVEHDR header, UINT size),
              (handle, header, size), MMSYSERR_NODRIVER)
