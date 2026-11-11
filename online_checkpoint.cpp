#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

const char* IsaacPrivatePairBlockReason(uintptr_t imageBase, uint64_t local, uint64_t peer);

namespace {

using SteamId = uint64_t;
using Log = void (*)(SteamId, const char*, const char*, unsigned, int, int);
using BuildStream = void* (__thiscall *)(void*, int);
using DestroyStream = void (__thiscall *)(void*);
using WriteStream = unsigned (__thiscall *)(void*, const void*, unsigned, unsigned);
using BroadcastStart = int (__thiscall *)(void*);

constexpr uintptr_t AppRva = 0x87169C;
constexpr uintptr_t GameRva = 0x871678;
constexpr uintptr_t FinalizeRva = 0x50CD10;
constexpr uintptr_t BuildMessageRva = 0x516F70;
constexpr uintptr_t BroadcastRva = 0x4EE080;
constexpr uintptr_t BuildStreamRva = 0x658040;
constexpr uintptr_t DestroyStreamRva = 0x658200;
constexpr std::array<uintptr_t, 2> ExitCalls{0x559D7A, 0x5AB77B};
constexpr std::array<uintptr_t, 2> StartCalls{0x4EAB87, 0x4EAC96};
constexpr uintptr_t BuildMessageCall = 0x4EE274;
constexpr unsigned MaximumPacketBytes = 1024 * 1024;
constexpr unsigned MaximumGameStateBytes = 8 * 1024 * 1024;
constexpr char Magic[] = "INDRUN01";

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
uintptr_t gameBuildMessage;
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
    if (size - pos < statsCount * 96 + TailBeforeRoster + 8) {
        return false;
    }
    pos += statsCount * 96 + TailBeforeRoster;
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
    LARGE_INTEGER length;
    bool valid = GetFileSizeEx(file, &length) && ReadExact(file, &resumeHeader, sizeof(resumeHeader)) &&
        std::memcmp(resumeHeader.magic, Magic, sizeof(resumeHeader.magic)) == 0 &&
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

// The original routines use ECX/EDX plus caller-cleaned stack arguments.
__declspec(naked) void __cdecl CaptureMessage(void*, int, int, void*) {
    __asm {
        push ebp
        mov ebp, esp
        push 0                  // forceNewGameMessage
        push dword ptr [ebp+20]  // shared progress state
        push 0                  // seed pointer, used only by the new-game branch
        push dword ptr [ebp+16]  // lobby/game parameter
        mov edx, [ebp+12]
        mov ecx, [ebp+8]
        call dword ptr [gameBuildMessage]
        add esp, 16
        pop ebp
        ret
    }
}

bool ActivePair(const uint8_t* app, const uint8_t* game, SteamId& owner, SteamId& partner) {
    const auto roster = Read<uintptr_t>(app, 307832);
    const auto devices = Read<uintptr_t>(app, 308184);
    const auto players = Read<uintptr_t>(game, 113320);
    const auto playersEnd = Read<uintptr_t>(game, 113324);
    if (!roster || Read<uintptr_t>(app, 307836) != roster + 40 ||
        !devices || Read<uintptr_t>(app, 308188) != devices + 8 ||
        !players || playersEnd < players + 8 || playersEnd > players + 128 || (playersEnd - players) % 4) {
        return false;
    }
    std::array<SteamId, 2> deviceUsers{};
    for (unsigned i = 0; i < 2; ++i) {
        const auto device = Read<const uint8_t*>(reinterpret_cast<const void*>(devices), i * 4);
        const auto profile = device ? Read<const void*>(device, 884) : nullptr;
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
        !Read<uint8_t>(app, 307828)) {
        return;
    }
    const auto frame = Read<unsigned>(app, 308196);
    const auto index = Read<int>(game, 99076);
    const auto room = Read<const void*>(game, 99072);
    const auto descriptor = room ? Read<const void*>(room, 4) : nullptr;
    SteamId owner = 0;
    SteamId partner = 0;
    const char* reason = nullptr;
    if (!descriptor || !(Read<uint8_t>(descriptor, 68) & 1) || Read<unsigned>(game) == 0 ||
        Read<unsigned>(game, 112700) || Read<unsigned>(game, 113272)) {
        reason = "room_not_stable_and_clear";
    } else if (Read<unsigned>(game, 157204) >= 2 || Read<unsigned>(game, 157232)) {
        reason = "unsupported_run_mode";
    } else if (Read<unsigned>(app, 308184 + 4552) || Read<unsigned>(app, 308184 + 6248)) {
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

    alignas(8) uint8_t stream[44]{};
    At<BuildStream>(BuildStreamRva)(stream, 0);
    CaptureMessage(stream, Read<int>(app, 307420), Read<int>(app, 307424), app + 312304);
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
    write(writer, resumePayload.data(), 1, resumeHeader.size);
    injected = Read<unsigned>(stream, 4) == resumeHeader.size;
}

__declspec(naked) void OnBuildMessage() {
    __asm {
        cmp byte ptr [injecting], 0
        je original_builder
        push ecx
        call InjectPacket
        add esp, 4
        ret
    original_builder:
        jmp dword ptr [gameBuildMessage]
    }
}

int __fastcall OnBroadcast(void* menu, void*) {
    if (!resumeArmed) {
        return At<BroadcastStart>(BroadcastRva)(menu);
    }
    const char* reason = resumePayloadValid ?
        IsaacPrivatePairBlockReason(image, resumeHeader.owner, resumeHeader.partner) : "checkpoint_invalid";
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

bool MatchesCall(uintptr_t rva, uintptr_t target) {
    const auto call = At<const uint8_t*>(rva);
    return call[0] == 0xe8 && Read<uint32_t>(call, 1) == target - rva - 5;
}

bool PatchCall(uintptr_t rva, uintptr_t target, uintptr_t replacement) {
    if (!MatchesCall(rva, target)) {
        return false;
    }
    auto* call = At<uint8_t*>(rva);
    DWORD oldProtection;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    const uint32_t relative = replacement - reinterpret_cast<uintptr_t>(call + 5);
    std::memcpy(call + 1, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    DWORD previous;
    VirtualProtect(call, 5, oldProtection, &previous);
    return true;
}

}

void EnableOnlineCheckpoint(Log log, bool resume) {
    if (!PrepareSaveDirectory()) {
        log(0, "online_checkpoint_unavailable", "save_directory", 0, -1, -1);
        return;
    }
    logger = log;
    image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos = At<const IMAGE_DOS_HEADER*>(0);
    const auto nt = At<const IMAGE_NT_HEADERS*>(dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage < 0x879A7C) {
        logger(0, "online_checkpoint_unavailable", "image_size", 0, -1, -1);
        return;
    }
    gameFinalize = image + FinalizeRva;
    gameBuildMessage = image + BuildMessageRva;
    saveEnabled = MatchesCall(ExitCalls[0], FinalizeRva) && MatchesCall(ExitCalls[1], FinalizeRva) &&
        PatchCall(ExitCalls[0], FinalizeRva, reinterpret_cast<uintptr_t>(OnFinalize)) &&
        PatchCall(ExitCalls[1], FinalizeRva, reinterpret_cast<uintptr_t>(OnFinalize));
    logger(0, saveEnabled ? "online_checkpoint_ready" : "online_checkpoint_unavailable",
        saveEnabled ? "before_exit" : "exit_call_sites", 0, -1, -1);
    if (resume) {
        resumePayloadValid = LoadFile();
        resumeArmed = MatchesCall(BuildMessageCall, BuildMessageRva) &&
            MatchesCall(StartCalls[0], BroadcastRva) && MatchesCall(StartCalls[1], BroadcastRva) &&
            PatchCall(BuildMessageCall, BuildMessageRva, reinterpret_cast<uintptr_t>(OnBuildMessage)) &&
            PatchCall(StartCalls[0], BroadcastRva, reinterpret_cast<uintptr_t>(OnBroadcast)) &&
            PatchCall(StartCalls[1], BroadcastRva, reinterpret_cast<uintptr_t>(OnBroadcast));
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
