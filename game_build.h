#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>

namespace IsaacRelease {

inline constexpr uintptr_t AppRva = 0x89A094;
inline constexpr uintptr_t GameRva = 0x8994F8;
inline constexpr uintptr_t GameLogRva = 0x62A590;
inline constexpr size_t AppOnlineRun = 308548;
inline constexpr size_t AppRoster = 308552;
inline constexpr size_t AppNetManager = 308904;
inline constexpr size_t NetFrame = 12;
inline constexpr size_t NetDesyncState = 4728;
inline constexpr size_t NetPendingCount = 6456;
inline constexpr size_t DeviceProfile = 884;
inline constexpr size_t GameRoom = 99072;
inline constexpr size_t GameRoomIndex = 99076;
inline constexpr size_t GameRoomTransition = 112700;
inline constexpr size_t GameStageTransition = 113272;
inline constexpr size_t GamePlayers = 113320;

inline bool MatchesLoadedImage(uintptr_t image) {
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew != 0x140) {
        return false;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
        nt->FileHeader.TimeDateStamp == 0x6B01D5E1 && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        nt->OptionalHeader.SizeOfImage == 0x953000;
}

inline bool MatchesGameFile(const wchar_t* path) {
    constexpr BYTE expected[] = {0x02, 0x4e, 0xd7, 0x30, 0x0f, 0xbe, 0xd8, 0xb6,
        0x5a, 0xfa, 0x1f, 0xdd, 0x1e, 0x36, 0xf2, 0x0b, 0x44, 0x6e, 0xde, 0x65,
        0xdc, 0xdd, 0xa0, 0xc8, 0x37, 0x5d, 0x04, 0x23, 0xaa, 0x9f, 0x07, 0x8d};
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    HANDLE mapping = nullptr;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 128 * 1024 * 1024) {
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    }
    CloseHandle(file);
    if (!mapping) {
        return false;
    }
    const auto bytes = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!bytes) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BYTE digest[sizeof(expected)]{};
    const bool hashed = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptHash(algorithm, nullptr, 0, const_cast<BYTE*>(bytes), static_cast<ULONG>(size.QuadPart),
                   digest, sizeof(digest)) == 0;
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    UnmapViewOfFile(bytes);
    return hashed && std::memcmp(digest, expected, sizeof(expected)) == 0;
}

}
