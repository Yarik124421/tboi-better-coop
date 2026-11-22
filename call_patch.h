#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>

struct CallPatch {
    uintptr_t rva;
    uintptr_t replacement;
    size_t length;
    std::array<uint8_t, 7> original;
};

inline CallPatch RedirectCall(uintptr_t rva, uintptr_t target, uintptr_t replacement) {
    const uint32_t displacement = static_cast<uint32_t>(target - rva - 5);
    CallPatch patch{rva, replacement, 5, {0xE8}};
    std::memcpy(patch.original.data() + 1, &displacement, sizeof(displacement));
    return patch;
}

template <size_t N>
bool InstallCallPatches(uintptr_t image, const CallPatch (&patches)[N],
                        bool (*matchesPrevious)(const uint8_t*, size_t) = nullptr) {
    static_assert(sizeof(uintptr_t) == 4, "These patches require an x86 build");
    for (const auto& patch : patches) {
        const auto call = reinterpret_cast<const uint8_t*>(image + patch.rva);
        if (std::memcmp(call, patch.original.data(), patch.length) != 0 &&
            (!matchesPrevious || !matchesPrevious(call, patch.length))) {
            return false;
        }
    }

    // Acquire all writable pages before changing any instruction. Restore
    // protections in reverse because more than one patch can share a page.
    DWORD protections[N];
    size_t writable = 0;
    for (; writable < N; ++writable) {
        const auto& patch = patches[writable];
        if (!VirtualProtect(reinterpret_cast<void*>(image + patch.rva), patch.length,
                            PAGE_EXECUTE_READWRITE, &protections[writable])) {
            break;
        }
    }
    const bool installed = writable == N;
    if (installed) {
        for (const auto& patch : patches) {
            auto* call = reinterpret_cast<uint8_t*>(image + patch.rva);
            const uint32_t displacement = patch.replacement - reinterpret_cast<uintptr_t>(call + 5);
            call[0] = 0xE8;
            std::memcpy(call + 1, &displacement, sizeof(displacement));
            std::memset(call + 5, 0x90, patch.length - 5);
            FlushInstructionCache(GetCurrentProcess(), call, patch.length);
        }
    }
    while (writable > 0) {
        const auto& patch = patches[--writable];
        DWORD previous;
        VirtualProtect(reinterpret_cast<void*>(image + patch.rva), patch.length, protections[writable], &previous);
    }
    return installed;
}
