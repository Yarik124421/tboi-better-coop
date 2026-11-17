// Usage: inject.exe <path to bettercoop_hotfix.dll>
// Loads the DLL into the single running isaac-ng.exe. Build as x86.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include "../version.h"

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: inject.exe <path to bettercoop_hotfix.dll>\n");
        return 1;
    }
    wchar_t dll[MAX_PATH];
    const DWORD length = GetFullPathNameW(argv[1], MAX_PATH, dll, nullptr);
    const DWORD attributes = length && length < MAX_PATH ? GetFileAttributesW(dll) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        std::fwprintf(stderr, L"DLL not found: %ls\n", argv[1]);
        return 1;
    }

    DWORD pid = 0;
    int matches = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"Process enumeration failed: %lu\n", GetLastError());
        return 1;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"isaac-ng.exe") == 0) {
            pid = entry.th32ProcessID;
            ++matches;
        }
    }
    CloseHandle(snapshot);
    if (matches != 1) {
        std::fwprintf(stderr, L"Expected exactly one isaac-ng.exe, found %d\n", matches);
        return 1;
    }

    const HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                       PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    const SIZE_T size = (std::wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll, size, nullptr)) {
        std::fwprintf(stderr, L"Writing the DLL path failed: %lu\n", GetLastError());
        if (remote) {
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        }
        CloseHandle(process);
        return 1;
    }
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    const HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        std::fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    const DWORD wait = WaitForSingleObject(thread, 10000);
    if (wait != WAIT_OBJECT_0) {
        std::fwprintf(stderr, L"DLL loading has not completed (wait result %lu). Check log.txt before retrying.\n", wait);
        // The loader may still be reading the path in the target process.
        CloseHandle(thread);
        CloseHandle(process);
        return 1;
    }
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    if (!module) {
        std::fwprintf(stderr, L"LoadLibraryW failed inside the game\n");
        return 1;
    }
    std::wprintf(L"Injected into isaac-ng.exe (pid %lu). Look for %ls in log.txt.\n", pid,
                 BetterCoop::HotfixMarkerWide);
    return 0;
}
