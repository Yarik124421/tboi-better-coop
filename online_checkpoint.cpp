#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include "call_patch.h"
#include "game_build.h"

const char* IsaacPrivatePairBlockReason(uintptr_t imageBase, uint64_t local, uint64_t peer);

namespace {

using namespace IsaacRelease;

using SteamId = uint64_t;
using Log = void (*)(SteamId, const char*, const char*, unsigned, int, int);
using BuildStream = void* (__thiscall *)(void*, int);
using DestroyStream = void (__thiscall *)(void*);
using WriteStream = unsigned (__thiscall *)(void*, const void*, unsigned, unsigned);
using BroadcastStart = int (__thiscall *)(void*);
using EncodeProgress = void* (__thiscall *)(const void*, unsigned*);

constexpr uintptr_t FinalizeRva = 0x51D610;
constexpr uintptr_t WriteSnapshotRva = 0x52B860;
constexpr uintptr_t EncodeProgressRva = 0x52F940;
constexpr uintptr_t FreeProgressRva = 0x684120;
constexpr uintptr_t BroadcastRva = 0x4F8850;
constexpr uintptr_t BuildStreamRva = 0x672E30;
constexpr uintptr_t DestroyStreamRva = 0x672F80;
constexpr uintptr_t ExitMenuCall = 0x56F92A;
constexpr uintptr_t ShutdownCall = 0x5C285B;
constexpr uintptr_t GuestReadyStartCall = 0x4F4F97;
constexpr uintptr_t LobbyStartCall = 0x4F50B4;
constexpr uintptr_t NewGameWriterCall = 0x52B496;
constexpr uintptr_t NewGameWriterRva = 0x52B540;
constexpr size_t AppSharedProgress = 313152;
constexpr size_t GameSnapshotMode = 0x26E68;
constexpr size_t GameSnapshotParameter = 0x26594;
constexpr size_t GameRunMode = 157224;
constexpr size_t GameSpecialRunState = 157252;
constexpr unsigned MaximumPacketBytes = 1024 * 1024;
constexpr unsigned MaximumGameStateBytes = 8 * 1024 * 1024;
constexpr char Magic[] = "INDRUN02";
constexpr char LegacyMagic[] = "INDRUN01";

struct Header {
    char magic[8];
    uint32_t size;
    uint32_t crc;
    SteamId owner;
    SteamId partner;
};
static_assert(sizeof(Header) == 32);

uintptr_t image;
uintptr_t gameFinalize;
uintptr_t gameWriteSnapshot;
uintptr_t gameFreeProgress;
uintptr_t gameNewGameWriter;
bool saveEnabled;
bool resumeArmed;
bool resumePayloadValid;
bool injecting;
bool injected;
Log logger;
Header resumeHeader{};
std::vector<uint8_t> resumePayload;
const char* lastBlock;
wchar_t saveDirectory[MAX_PATH];
wchar_t savePath[MAX_PATH];

template <typename T>
T Read(const void* address, size_t offset = 0) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(address) + offset, sizeof(value));
    return value;
}

template <typename T>
T At(uintptr_t rva) {
    return reinterpret_cast<T>(image + rva);
}

uint32_t Checksum(const uint8_t* data, unsigned size) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
    }
    return ~crc;
}

bool ValidatePacket(const uint8_t* bytes, unsigned size, SteamId owner, SteamId partner) {
    // Type, frame, player count, two 17-byte roster entries, then two length fields.
    if (!bytes || size < 49 || size > MaximumPacketBytes || !owner || !partner || owner == partner ||
        bytes[0] != 5 || bytes[5] != 2) {
        return false;
    }
    const SteamId first = Read<SteamId>(bytes, 6);
    const SteamId second = Read<SteamId>(bytes, 23);
    if (!((first == owner && second == partner) || (first == partner && second == owner))) {
        return false;
    }
    const unsigned unpacked = Read<unsigned>(bytes, 40);
    const unsigned packed = Read<unsigned>(bytes, 44);
    if (!unpacked || unpacked > MaximumGameStateBytes || !packed || packed > size - 49) {
        return false;
    }
    unsigned pos = 48 + packed;
    const unsigned statsCount = bytes[pos++];
    constexpr unsigned TailBeforeRoster = 4 + 624 * 4 + 1 + 4 + 1 + 4 + 8;
    constexpr unsigned StatsEntryBytes = 8 + 22 * 4;
    if (size - pos < statsCount * StatsEntryBytes + TailBeforeRoster + 8) {
        return false;
    }
    pos += statsCount * StatsEntryBytes + TailBeforeRoster;
    const uint64_t otherIds = Read<uint64_t>(bytes, pos);
    pos += 8;
    if (otherIds > (size - pos) / 8) {
        return false;
    }
    pos += static_cast<unsigned>(otherIds) * 8;
    return size - pos >= 4 && Read<unsigned>(bytes, pos) == size - pos - 4;
}

bool ReadExact(HANDLE file, void* data, DWORD size) {
    DWORD done;
    return ReadFile(file, data, size, &done, nullptr) && done == size;
}

bool WriteExact(HANDLE file, const void* data, DWORD size) {
    DWORD done;
    return WriteFile(file, data, size, &done, nullptr) && done == size;
}

bool PrepareSaveDirectory() {
    wchar_t root[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, root, _countof(root));
    if (length == 0 || length >= _countof(root)) {
        return false;
    }
    auto* separator = std::wcsrchr(root, L'\\');
    if (!separator) {
        return false;
    }
    *separator = L'\0';
    wchar_t tools[MAX_PATH];
    wchar_t mod[MAX_PATH];
    if (swprintf_s(tools, L"%s\\tools", root) < 0 ||
        swprintf_s(mod, L"%s\\IsaacNetDiag", tools) < 0 ||
        swprintf_s(saveDirectory, L"%s\\saves", mod) < 0 ||
        swprintf_s(savePath, L"%s\\online-run.ind", saveDirectory) < 0) {
        return false;
    }
    return (CreateDirectoryW(tools, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
        (CreateDirectoryW(mod, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
        (CreateDirectoryW(saveDirectory, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

bool SaveFile(const Header& header, const uint8_t* data) {
    wchar_t temporary[MAX_PATH];
    if (swprintf_s(temporary, L"%s\\online-run-%lu.tmp", saveDirectory, GetCurrentProcessId()) < 0) {
        return false;
    }
    const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool written = WriteExact(file, &header, sizeof(header)) &&
        WriteExact(file, data, header.size) && FlushFileBuffers(file);
    CloseHandle(file);
    if (written && MoveFileExW(temporary, savePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(temporary);
    return false;
}

bool LoadFile() {
    const HANDLE file = CreateFileW(savePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    // Release reads the beta GameState version (169) and uses the same
    // type-5 body. The builder appends fresh release trailers on resume.
    LARGE_INTEGER length;
    bool valid = GetFileSizeEx(file, &length) && ReadExact(file, &resumeHeader, sizeof(resumeHeader)) &&
        (std::memcmp(resumeHeader.magic, Magic, sizeof(resumeHeader.magic)) == 0 ||
         std::memcmp(resumeHeader.magic, LegacyMagic, sizeof(resumeHeader.magic)) == 0) &&
        resumeHeader.size <= MaximumPacketBytes && resumeHeader.size >= 49 &&
        length.QuadPart == sizeof(resumeHeader) + resumeHeader.size;
    if (valid) {
        resumePayload.resize(resumeHeader.size);
        valid = ReadExact(file, resumePayload.data(), resumeHeader.size);
    }
    CloseHandle(file);
    return valid && Checksum(resumePayload.data(), resumeHeader.size) == resumeHeader.crc &&
        ValidatePacket(resumePayload.data(), resumeHeader.size, resumeHeader.owner, resumeHeader.partner);
}

// Serialize the type-5 body directly. The outer builder advances the network
// generation and writes lobby progress, which should only happen on a start.
__declspec(naked) void __cdecl CaptureMessage(void*, int, int, const void*, unsigned) {
    __asm {
        push ebp
        mov ebp, esp
        push dword ptr [ebp+24]  // compressed progress size
        push dword ptr [ebp+20]  // compressed progress data
        push dword ptr [ebp+16]  // lobby/game parameter
        mov edx, [ebp+12]
        mov ecx, [ebp+8]
        call dword ptr [gameWriteSnapshot]
        add esp, 12
        pop ebp
        ret
    }
}

__declspec(naked) void __cdecl FreeEncodedProgress(void*) {
    __asm {
        push dword ptr [esp+4]
        mov edx, 1
        push 0
        push 0
        mov cl, dl
        call dword ptr [gameFreeProgress]
        add esp, 12
        ret
    }
}

bool ActivePair(const uint8_t* app, const uint8_t* game, SteamId& owner, SteamId& partner) {
    const auto roster = Read<uintptr_t>(app, AppRoster);
    const auto devices = Read<uintptr_t>(app, AppNetManager);
    const auto players = Read<uintptr_t>(game, GamePlayers);
    const auto playersEnd = Read<uintptr_t>(game, GamePlayers + 4);
    if (!roster || Read<uintptr_t>(app, AppRoster + 4) != roster + 40 ||
        !devices || Read<uintptr_t>(app, AppNetManager + 4) != devices + 8 ||
        !players || playersEnd < players + 8 || playersEnd > players + 128 || (playersEnd - players) % 4) {
        return false;
    }
    std::array<SteamId, 2> deviceUsers{};
    for (unsigned i = 0; i < 2; ++i) {
        const auto device = Read<const uint8_t*>(reinterpret_cast<const void*>(devices), i * 4);
        const auto profile = device ? Read<const void*>(device, DeviceProfile) : nullptr;
        if (!profile) {
            return false;
        }
        deviceUsers[i] = Read<SteamId>(profile, 8);
        const auto id = Read<unsigned>(device, 12);
        bool ownsCharacter = false;
        for (auto p = players; p < playersEnd; p += 4) {
            const auto player = Read<const uint8_t*>(reinterpret_cast<const void*>(p));
            if (player && Read<unsigned>(player, 44) == 0 && Read<unsigned>(player, 5656) == id) {
                ownsCharacter = true;
                break;
            }
        }
        if (!ownsCharacter) {
            return false;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        const auto profile = Read<const void*>(reinterpret_cast<const void*>(roster), i * 20 + 4);
        if (!profile) {
            return false;
        }
        const auto user = Read<SteamId>(profile, 8);
        if (user != deviceUsers[0] && user != deviceUsers[1]) {
            return false;
        }
        auto& slot = (Read<uint8_t>(profile) & 1) ? owner : partner;
        if (slot || !user) {
            return false;
        }
        slot = user;
    }
    return owner && partner && owner != partner;
}

void __cdecl SaveBeforeTeardown() {
    const auto app = *At<uint8_t**>(AppRva);
    const auto game = *At<uint8_t**>(GameRva);
    if (!saveEnabled || resumeArmed || !app || !game || Read<unsigned>(app, 8) != 2 ||
        !Read<uint8_t>(app, AppOnlineRun)) {
        return;
    }
    const auto frame = Read<unsigned>(app, AppNetManager + NetFrame);
    const auto index = Read<int>(game, GameRoomIndex);
    const auto room = Read<const void*>(game, GameRoom);
    const auto descriptor = room ? Read<const void*>(room, 4) : nullptr;
    SteamId owner = 0;
    SteamId partner = 0;
    const char* reason = nullptr;
    if (!descriptor || !(Read<uint8_t>(descriptor, 68) & 1) || Read<unsigned>(game) == 0 ||
        Read<unsigned>(game, GameRoomTransition) || Read<unsigned>(game, GameStageTransition)) {
        reason = "room_not_stable_and_clear";
    } else if (Read<unsigned>(game, GameRunMode) >= 2 || Read<unsigned>(game, GameSpecialRunState)) {
        reason = "unsupported_run_mode";
    } else if (Read<unsigned>(app, AppNetManager + NetDesyncState) ||
               Read<unsigned>(app, AppNetManager + NetPendingCount)) {
        reason = "network_transition_pending";
    } else if (!ActivePair(app, game, owner, partner)) {
        reason = "two_active_characters_required";
    } else {
        reason = IsaacPrivatePairBlockReason(image, owner, partner);
    }
    if (reason) {
        logger(partner, "online_checkpoint_skipped", reason, frame, index, index);
        return;
    }

    unsigned progressSize = 0;
    const auto progress = At<EncodeProgress>(EncodeProgressRva)(app + AppSharedProgress, &progressSize);
    if (!progress || progressSize > MaximumPacketBytes) {
        if (progress) {
            FreeEncodedProgress(progress);
        }
        logger(partner, "online_checkpoint_failed", "encode_progress", frame, index, index);
        return;
    }
    alignas(8) uint8_t stream[44]{};
    At<BuildStream>(BuildStreamRva)(stream, 0);
    CaptureMessage(stream, Read<int>(game, GameSnapshotMode), Read<int>(game, GameSnapshotParameter),
                   progress, progressSize);
    FreeEncodedProgress(progress);
    const auto bytes = Read<const uint8_t*>(stream, 24);
    const auto size = Read<unsigned>(stream, 4);
    const auto data = bytes ? bytes : Read<const uint8_t*>(stream, 12);
    bool saved = false;
    if (ValidatePacket(data, size, owner, partner)) {
        Header header{};
        std::memcpy(header.magic, Magic, sizeof(header.magic));
        header.size = size;
        header.crc = Checksum(data, size);
        header.owner = owner;
        header.partner = partner;
        saved = SaveFile(header, data);
        reason = saved ? "two_player_snapshot" : "write_error";
    } else {
        reason = "invalid_game_snapshot";
    }
    At<DestroyStream>(DestroyStreamRva)(stream);
    logger(partner, saved ? "online_checkpoint_saved" : "online_checkpoint_failed", reason, frame, index, index);
}

__declspec(naked) void OnFinalize() {
    __asm {
        pushfd
        pushad
        call SaveBeforeTeardown
        popad
        popfd
        jmp dword ptr [gameFinalize]
    }
}

void __cdecl InjectPacket(void* stream) {
    // Same stream writer as Isaac_Online_WriteExistingGameSnapshot.
    auto* writer = static_cast<uint8_t*>(stream) + 40;
    const auto vtable = Read<const void*>(writer);
    const auto write = Read<WriteStream>(vtable, 28);
    write(writer, resumePayload.data(), resumeHeader.size, 1);
    injected = Read<unsigned>(stream, 4) == resumeHeader.size;
}

// Replace only the new-run body during a resume. The native builder still
// appends RUN1 (input generation) and RCPS (committed player progress).
__declspec(naked) void OnWriteNewGame() {
    __asm {
        cmp byte ptr [injecting], 0
        je original_builder
        push ecx
        call InjectPacket
        add esp, 4
        ret
    original_builder:
        jmp dword ptr [gameNewGameWriter]
    }
}

int __fastcall OnBroadcast(void* menu, void*) {
    if (!resumeArmed) {
        return At<BroadcastStart>(BroadcastRva)(menu);
    }
    const auto game = *At<const uint8_t**>(GameRva);
    const char* reason = !resumePayloadValid ? "checkpoint_invalid" :
        game && Read<unsigned>(game) != 0 ? "game_in_progress" :
        IsaacPrivatePairBlockReason(image, resumeHeader.owner, resumeHeader.partner);
    if (reason) {
        if (!lastBlock || std::strcmp(lastBlock, reason) != 0) {
            lastBlock = reason;
            logger(resumeHeader.partner, "online_resume_blocked", reason, 0, -1, -1);
            MessageBoxW(nullptr, L"Resume requires a valid checkpoint and the same two players in a private Steam lobby.",
                L"Isaac NetDiag", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    lastBlock = nullptr;
    const auto frame = Read<unsigned>(resumePayload.data(), 1);
    injecting = true;
    injected = false;
    const int result = At<BroadcastStart>(BroadcastRva)(menu);
    injecting = false;
    if (injected) {
        resumeArmed = false;
    }
    logger(resumeHeader.partner, injected ? "online_resume_dispatched" : "online_resume_failed",
        injected ? "native_snapshot_broadcast" : "stream_write", frame, -1, -1);
    return result;
}

}

void EnableOnlineCheckpoint(Log log, bool resume) {
    if (!PrepareSaveDirectory()) {
        log(0, "online_checkpoint_unavailable", "save_directory", 0, -1, -1);
        return;
    }
    logger = log;
    image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!MatchesLoadedImage(image)) {
        logger(0, "online_checkpoint_unavailable", "game_image", 0, -1, -1);
        return;
    }
    gameFinalize = image + FinalizeRva;
    gameWriteSnapshot = image + WriteSnapshotRva;
    gameFreeProgress = image + FreeProgressRva;
    gameNewGameWriter = image + NewGameWriterRva;
    const CallPatch exitPatches[]{
        RedirectCall(ExitMenuCall, FinalizeRva, reinterpret_cast<uintptr_t>(OnFinalize)),
        RedirectCall(ShutdownCall, FinalizeRva, reinterpret_cast<uintptr_t>(OnFinalize)),
    };
    saveEnabled = InstallCallPatches(image, exitPatches);
    logger(0, saveEnabled ? "online_checkpoint_ready" : "online_checkpoint_unavailable",
        saveEnabled ? "before_exit" : "exit_call_sites", 0, -1, -1);
    if (resume) {
        resumePayloadValid = LoadFile();
        const CallPatch resumePatches[]{
            RedirectCall(NewGameWriterCall, NewGameWriterRva, reinterpret_cast<uintptr_t>(OnWriteNewGame)),
            RedirectCall(GuestReadyStartCall, BroadcastRva, reinterpret_cast<uintptr_t>(OnBroadcast)),
            RedirectCall(LobbyStartCall, BroadcastRva, reinterpret_cast<uintptr_t>(OnBroadcast)),
        };
        resumeArmed = InstallCallPatches(image, resumePatches);
        logger(resumeHeader.partner, resumeArmed && resumePayloadValid ? "online_resume_ready" : "online_resume_unavailable",
            !resumeArmed ? "start_call_sites" : resumePayloadValid ? "checkpoint_loaded" : "checkpoint_invalid", 0, -1, -1);
        if (!resumeArmed) {
            MessageBoxW(nullptr, L"Resume could not start for this game build.",
                L"Isaac NetDiag", MB_OK | MB_ICONWARNING);
        }
    }
}

void InitializeOnlineCheckpoint(Log log) {
    char mode[8]{};
    if (GetEnvironmentVariableA("ISAAC_NETDIAG_ONLINE_CHECKPOINT", mode, sizeof(mode)) < sizeof(mode)) {
        if (std::strcmp(mode, "resume") == 0) {
            EnableOnlineCheckpoint(log, true);
        } else if (std::strcmp(mode, "save") == 0) {
            EnableOnlineCheckpoint(log, false);
        }
    }
}

void DisableOnlineCheckpoint() {
    saveEnabled = false;
    resumeArmed = false;
}
