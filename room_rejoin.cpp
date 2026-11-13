#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using SteamId = uint64_t;
using Log = void (*)(SteamId, const char*, const char*, unsigned, int, int);
using UpdateRoomTransition = int (__thiscall *)(void*);
using ProcessPendingJoins = void (__thiscall *)(void*, char);
using UpdateNetManager = bool (__thiscall *)(void*);
using BroadcastFrameInput = void (__thiscall *)(void*, unsigned);
using StartPendingGame = void (__thiscall *)(void*);
using RestoreJoinedGame = int (__thiscall *)(void*, void*, int, int);
using CaptureGameState = int (__thiscall *)(void*, void*);
using UpdateGame = void (__thiscall *)(void*);
using JoinEntityPass = int (__cdecl *)();
using GetMatchmaking = void* (__cdecl *)();
using GetLobbyOwner = SteamId (__cdecl *)(void*, SteamId);
using GetLobbyMemberCount = int (__cdecl *)(void*, SteamId);
using GetLobbyMember = SteamId (__cdecl *)(void*, SteamId, int);
using GetLobbyData = const char* (__cdecl *)(void*, SteamId, const char*);
using GetLobbyMemberData = const char* (__cdecl *)(void*, SteamId, SteamId, const char*);
using SetLobbyMemberData = void (__cdecl *)(void*, SteamId, const char*, const char*);

// RVAs and object offsets for the image validated by Start-NetDiag.ps1.
constexpr uintptr_t RoomUpdateCallRva = 0x2FB51C;
constexpr uintptr_t RoomUpdateRva = 0x4318A0;
constexpr uintptr_t ProcessJoinsRva = 0x50DF40;
constexpr uintptr_t NetUpdateCallRva = 0x5550AD;
constexpr uintptr_t NetUpdateRva = 0x50B150;
constexpr uintptr_t BroadcastInputRva = 0x502950;
constexpr uintptr_t StartPendingCallRva = 0x554D5E;
constexpr uintptr_t StartPendingRva = 0x559720;
constexpr uintptr_t WriteParityRva = 0x517882;
constexpr uintptr_t ReadParityRva = 0x5185AA;
constexpr uintptr_t RestoreHostCallRva = 0x5187CB;
constexpr uintptr_t RestoreJoinedGameRva = 0x50E2A0;
constexpr uintptr_t CaptureSnapshotCallRva = 0x517509;
constexpr uintptr_t CaptureGameStateRva = 0x2F9000;
constexpr uintptr_t GameUpdateCallRva = 0x555231;
constexpr uintptr_t GameUpdateRva = 0x2FADC0;
constexpr uintptr_t HostJoinPassCallRva = 0x50CCF7;
constexpr uintptr_t JoinEntityPassRva = 0x50D3F0;
constexpr uintptr_t GuestJoinPassCallRva = 0x2F5990;
// Global MT19937 used for the frame checksum. The join message carries it.
constexpr uintptr_t GlobalRngStateRva = 0x87AC70;
constexpr uintptr_t GlobalRngIndexRva = 0x834F04;
constexpr size_t GlobalRngWords = 624;
constexpr uintptr_t GameRva = 0x871678;
constexpr uintptr_t AppRva = 0x87169C;
constexpr uintptr_t CurrentLobbyRva = 0x879A74;
constexpr uintptr_t LobbySteamVtableRva = 0x79E9A4;

constexpr size_t GameStartingRoomIndex = 99024;
constexpr size_t GameRoom = 99072;
constexpr size_t GameRoomIndex = 99076;
constexpr size_t GameDimension = 99084;
constexpr size_t GameRoomTransition = 112700;
constexpr size_t GameStageTransition = 113272;
constexpr size_t GamePlayers = 113320;
// ItemOverlay (transformation/giantbook style): state 0 idle, 1 delay, 2
// showing. While showing, Game::Update skips the world update.
constexpr size_t GameItemOverlay = 0x1C034;
constexpr size_t RoomEntityList = 4700;
constexpr size_t RoomEntityCount = 4708;
constexpr size_t EntityType = 40;
constexpr size_t EntityVariant = 44;
constexpr size_t EntitySubtype = 48;
constexpr size_t EntityPosition = 828;
constexpr uintptr_t GameLogRva = 0x6112C0;
constexpr unsigned DiagFrames = 8;
constexpr size_t PlayerControlsEnabled = 1040;
constexpr size_t EntityFlags = 0x168;
constexpr size_t EntityActive = 370;
// FLAG_INTERPOLATION_UPDATE: Player::Update skips its RNG-drawing logic.
constexpr uint32_t FlagInterpolationUpdate = 0x4000;
constexpr bool MarkHostPlayersInPass = false;
constexpr size_t PlayerKeys = 4956;
constexpr size_t PlayerGoldenKey = 4960;
constexpr size_t PlayerGoldenBomb = 4961;
constexpr size_t PlayerBombs = 4964;
constexpr size_t PlayerCoins = 4968;
constexpr size_t SavedPlayerCount = 105848;
constexpr size_t SavedPlayers = 105852;
constexpr size_t SavedPlayerSize = 1500;
constexpr size_t AppOnlineRun = 307828;
constexpr size_t AppRoster = 307832;
constexpr size_t AppNetManager = 308184;
constexpr size_t AppHasPendingSnapshot = 307505;
constexpr size_t NetFrame = 12;
constexpr size_t NetDesyncState = 4552;
constexpr size_t NetPendingHead = 6244;
constexpr size_t NetPendingCount = 6248;
constexpr size_t DeviceProfile = 884;
constexpr size_t DeviceSentInputs = 940;
constexpr unsigned ReplayFrames = 16;
// Both clients simulate the join frame and the next OnlineInputDelay frames
// with different inputs for the host: the guest pre-fills them as neutral and
// drops the host's real packets. Keep controls off on both sides until a
// shared frame well past that window, then enable them on the same frame.
constexpr unsigned ControlDelayFrames = 30;
constexpr unsigned ControlGateTimeoutFrames = 900;
constexpr char CapabilityKey[] = "isaac_netdiag_room_rejoin";
constexpr char CapabilityVersion[] = "11";

struct SharedResources {
    int coins;
    int keys;
    int bombs;
    uint8_t goldenKey;
    uint8_t goldenBomb;
};

struct Room {
    const void* game;
    int stage;
    int stageType;
    int index;
    int dimension;
    bool clear;
};

struct ControlGate {
    const void* game;
    const void* manager;
    SteamId peer;
    unsigned joinFrame;
    unsigned enableFrame;
};

struct RejoinReplay {
    const void* manager;
    SteamId local;
    SteamId peer;
    unsigned firstFrame;
    int fromRoom;
    Room destination;
    ULONGLONG deadline;
    ULONGLONG nextSend;
};

uintptr_t image;
Log logEvent;
Room sourceRoom{};
RejoinReplay replay{};
SteamId advertisedLobby;
bool writingRoomSnapshot;
SharedResources stagedResources;
const void* markedHostManager;
const void* markedGuestApp;
unsigned markedGuestFrame;
const void* guestGame;
unsigned guestStartFrame;
ULONGLONG guestDeadline;
UpdateRoomTransition originalUpdate;
ProcessPendingJoins processPendingJoins;
UpdateNetManager originalNetUpdate;
StartPendingGame originalStartPending;
RestoreJoinedGame originalRestoreJoinedGame;
CaptureGameState originalCaptureGameState;
UpdateGame originalGameUpdate;
JoinEntityPass originalJoinEntityPass;
// Global RNG exactly as received in the room snapshot message. Host and guest
// run different join setup code afterwards, which draws different amounts
// from it; both put this copy back once that setup is done.
uint32_t receivedRng[GlobalRngWords];
int receivedRngIndex;
bool receivedRngValid;
bool hostRngPending;
ControlGate controlGate{};
// Per-frame entity dump right after a room rejoin, on both clients.
unsigned diagFirstFrame;
unsigned diagLastFrame;
bool diagActive;
BroadcastFrameInput broadcastFrameInput;

template <typename T>
T Read(const void* object, size_t offset = 0) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(value));
    return value;
}

template <typename T>
void Write(void* object, size_t offset, const T& value) {
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}

template <typename T>
T At(uintptr_t rva) {
    return reinterpret_cast<T>(image + rva);
}

template <typename T>
T SteamExport(const char* name) {
    const auto steam = GetModuleHandleW(L"steam_api.dll");
    return steam ? reinterpret_cast<T>(GetProcAddress(steam, name)) : nullptr;
}

SteamId CurrentSteamLobby(uintptr_t imageBase) {
    const auto lobby = *reinterpret_cast<void**>(imageBase + CurrentLobbyRva);
    return lobby && Read<uintptr_t>(lobby) == imageBase + LobbySteamVtableRva
        ? Read<SteamId>(lobby, 8) : 0;
}

Room CurrentRoom(const void* game) {
    const auto room = Read<const void*>(game, GameRoom);
    const auto descriptor = room ? Read<const void*>(room, 4) : nullptr;
    return {game, Read<int>(game), Read<int>(game, 4), Read<int>(game, GameRoomIndex),
            Read<int>(game, GameDimension), descriptor && (Read<uint8_t>(descriptor, 68) & 1) != 0};
}

bool OrdinaryRoomChange(const Room& from, const Room& to) {
    return from.game == to.game && from.stage == to.stage && from.stageType == to.stageType &&
        from.dimension == to.dimension && from.index >= 0 && to.index >= 0 && from.index != to.index;
}

SteamId PendingPeer(const void* manager) {
    if (Read<unsigned>(manager, NetPendingCount) != 1) {
        return 0;
    }
    const auto head = Read<const void*>(manager, NetPendingHead);
    const auto node = head ? Read<const void*>(head) : nullptr;
    if (!node || node == head || Read<const void*>(head, 4) != node ||
        Read<const void*>(node) != head || Read<const void*>(node, 8) != head ||
        Read<uint8_t>(node, 13) != 0) {
        return 0;
    }
    return Read<SteamId>(node, 16);
}

SteamId SoloLocalPlayer(const void* app, const void* manager) {
    const auto devices = Read<uintptr_t>(manager);
    const auto devicesEnd = Read<uintptr_t>(manager, 4);
    const auto roster = Read<uintptr_t>(app, AppRoster);
    const auto rosterEnd = Read<uintptr_t>(app, AppRoster + 4);
    if (!devices || devicesEnd != devices + 4 || !roster || rosterEnd != roster + 20) {
        return 0;
    }
    const auto device = Read<const void*>(reinterpret_cast<const void*>(devices));
    const auto profile = Read<const void*>(reinterpret_cast<const void*>(roster), 4);
    const auto deviceProfile = device ? Read<const void*>(device, DeviceProfile) : nullptr;
    if (!deviceProfile || !profile || (Read<uint8_t>(profile) & 1) == 0 ||
        Read<SteamId>(deviceProfile, 8) != Read<SteamId>(profile, 8)) {
        return 0;
    }
    return Read<SteamId>(profile, 8);
}

}

const char* IsaacPrivatePairBlockReason(uintptr_t imageBase, SteamId local, SteamId peer) {
    const SteamId lobby = CurrentSteamLobby(imageBase);
    if (!lobby) {
        return "no_steam_lobby";
    }
    const auto steam = GetModuleHandleW(L"steam_api.dll");
    if (!steam) {
        return "steam_module";
    }
    const auto getMatchmaking = reinterpret_cast<GetMatchmaking>(GetProcAddress(steam, "SteamAPI_SteamMatchmaking_v009"));
    const auto getLobbyOwner = reinterpret_cast<GetLobbyOwner>(GetProcAddress(steam, "SteamAPI_ISteamMatchmaking_GetLobbyOwner"));
    const auto getLobbyMemberCount = reinterpret_cast<GetLobbyMemberCount>(GetProcAddress(steam, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers"));
    const auto getLobbyMember = reinterpret_cast<GetLobbyMember>(GetProcAddress(steam, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex"));
    const auto getLobbyData = reinterpret_cast<GetLobbyData>(GetProcAddress(steam, "SteamAPI_ISteamMatchmaking_GetLobbyData"));
    if (!getMatchmaking || !getLobbyOwner || !getLobbyMemberCount || !getLobbyMember || !getLobbyData) {
        return "matchmaking_exports";
    }
    void* matchmaking = getMatchmaking();
    if (!lobby || !matchmaking || getLobbyOwner(matchmaking, lobby) != local) {
        return "not_lobby_owner";
    }
    const char* visibility = getLobbyData(matchmaking, lobby, "visibility");
    if (!visibility || (std::strcmp(visibility, "0") != 0 && std::strcmp(visibility, "1") != 0)) {
        return "lobby_visibility";
    }
    if (getLobbyMemberCount(matchmaking, lobby) != 2) {
        return "lobby_size";
    }
    const auto first = getLobbyMember(matchmaking, lobby, 0);
    const auto second = getLobbyMember(matchmaking, lobby, 1);
    if (!((first == local && second == peer) || (first == peer && second == local))) {
        return "lobby_members";
    }
    return nullptr;
}

namespace {

void AdvertiseRoomRejoin() {
    const SteamId lobby = CurrentSteamLobby(image);
    if (!lobby || lobby == advertisedLobby) {
        advertisedLobby = lobby;
        return;
    }
    const auto getMatchmaking = SteamExport<GetMatchmaking>("SteamAPI_SteamMatchmaking_v009");
    const auto getData = SteamExport<GetLobbyData>("SteamAPI_ISteamMatchmaking_GetLobbyData");
    const auto setData = SteamExport<SetLobbyMemberData>("SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
    void* matchmaking = getMatchmaking ? getMatchmaking() : nullptr;
    const char* visibility = matchmaking && getData ? getData(matchmaking, lobby, "visibility") : nullptr;
    if (visibility && (std::strcmp(visibility, "0") == 0 || std::strcmp(visibility, "1") == 0) && setData) {
        setData(matchmaking, lobby, CapabilityKey, CapabilityVersion);
        advertisedLobby = lobby;
    }
}

bool PeerSupportsRoomRejoin(SteamId peer) {
    const auto getMatchmaking = SteamExport<GetMatchmaking>("SteamAPI_SteamMatchmaking_v009");
    const auto getData = SteamExport<GetLobbyMemberData>("SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
    void* matchmaking = getMatchmaking ? getMatchmaking() : nullptr;
    const char* version = matchmaking && getData
        ? getData(matchmaking, CurrentSteamLobby(image), peer, CapabilityKey) : nullptr;
    return version && std::strcmp(version, CapabilityVersion) == 0;
}

const uint8_t* SinglePlayer(const void* game) {
    const auto players = Read<const uint8_t* const*>(game, GamePlayers);
    const auto end = Read<const uint8_t* const*>(game, GamePlayers + 4);
    if (!players || end != players + 1 || !players[0] ||
        Read<int>(players[0], EntityType) != 1 || Read<int>(players[0], EntityVariant) != 0) {
        return nullptr;
    }
    return players[0];
}

bool HasProtectedRoomObjects(const void* room) {
    const auto entities = Read<const void* const*>(room, RoomEntityList);
    const unsigned count = Read<unsigned>(room, RoomEntityCount);
    if ((count != 0 && !entities) || count > 4096) {
        return true;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto entity = entities[i];
        if (entity) {
            const int type = Read<int>(entity, EntityType);
            if (type == 5 || type == 6) {
                return true;
            }
        }
    }
    return false;
}

const char* RoomContentBlockReason(const Room& room) {
    // Online floors can hold a Revive Machine in their starting room.
    // Guard it even when the live entity list does not report the machine.
    if (room.index == Read<int>(room.game, GameStartingRoomIndex)) {
        return "floor_start_room";
    }
    const auto currentRoom = Read<const void*>(room.game, GameRoom);
    return !currentRoom || HasProtectedRoomObjects(currentRoom) ? "room_has_pickups_or_machines" : nullptr;
}

void TryRoomRejoin(const Room& from, const Room& to) {
    auto* app = *At<uint8_t**>(AppRva);
    if (!app || Read<unsigned>(app, 8) != 2 || Read<uint8_t>(app, AppOnlineRun) == 0 ||
        to.stage <= 0 || Read<unsigned>(to.game, GameStageTransition) != 0) {
        return;
    }
    auto* manager = app + AppNetManager;
    if (Read<unsigned>(manager, NetPendingCount) == 0) {
        return;
    }
    const unsigned frame = Read<unsigned>(manager, NetFrame);
    const SteamId peer = PendingPeer(manager);
    const SteamId local = SoloLocalPlayer(app, manager);
    const auto player = SinglePlayer(to.game);
    const char* reason = nullptr;
    if (!from.clear || !to.clear) {
        reason = "room_not_clear";
    } else if (Read<unsigned>(manager, NetDesyncState) != 0) {
        reason = "desync_recovery_active";
    } else if (!peer || !local || peer == local || !player) {
        reason = "not_solo_with_one_pending_peer";
    } else {
        reason = RoomContentBlockReason(to);
        if (!reason) {
            reason = IsaacPrivatePairBlockReason(image, local, peer);
        }
        if (!reason && !PeerSupportsRoomRejoin(peer)) {
            reason = "peer_needs_room_rejoin";
        }
    }
    if (reason) {
        logEvent(peer, "room_rejoin_skip", reason, frame, from.index, to.index);
        return;
    }
    logEvent(peer, "room_rejoin_begin", "process_pending", frame, from.index, to.index);
    stagedResources = {Read<int>(player, PlayerCoins), Read<int>(player, PlayerKeys), Read<int>(player, PlayerBombs),
                       Read<uint8_t>(player, PlayerGoldenKey), Read<uint8_t>(player, PlayerGoldenBomb)};
    writingRoomSnapshot = true;
    processPendingJoins(manager, 0);
    writingRoomSnapshot = false;
    if (*At<uint8_t**>(AppRva) == app) {
        const auto count = Read<unsigned>(manager, NetPendingCount);
        if (count == 0) {
            const auto now = GetTickCount64();
            replay = {manager, local, peer, frame, from.index, to, now + 8000, now + 250};
        }
        logEvent(peer, "room_rejoin_end", count == 0 ? "queue_consumed" : "still_pending",
                 frame, from.index, to.index);
    }
}

SteamId InputDeviceUser(const void* device) {
    const auto profile = device ? Read<const void*>(device, DeviceProfile) : nullptr;
    return profile ? Read<SteamId>(profile, 8) : 0;
}

void ReplayRoomInputs(void* manager) {
    if (replay.manager != manager) {
        return;
    }
    const auto now = GetTickCount64();
    if (now < replay.nextSend) {
        return;
    }
    const auto app = *At<const uint8_t**>(AppRva);
    const auto game = *At<const uint8_t**>(GameRva);
    const unsigned frame = Read<unsigned>(manager, NetFrame);
    const auto& room = replay.destination;
    if (!app || app + AppNetManager != manager || Read<unsigned>(app, 8) != 2 ||
        game != room.game || Read<int>(game, GameRoomIndex) != room.index ||
        Read<int>(game, GameDimension) != room.dimension || Read<int>(game) != room.stage ||
        Read<int>(game, 4) != room.stageType ||
        Read<unsigned>(game, GameRoomTransition) != 0 ||
        Read<unsigned>(game, GameStageTransition) != 0 || Read<unsigned>(manager, NetDesyncState) != 0 ||
        frame < replay.firstFrame || frame - replay.firstFrame >= ReplayFrames || now >= replay.deadline) {
        logEvent(replay.peer, "room_rejoin_replay_end", now >= replay.deadline ? "timeout" : "window_closed",
                 frame, replay.fromRoom, room.index);
        replay = {};
        return;
    }
    const auto devices = Read<void* const*>(manager);
    const auto devicesEnd = Read<void* const*>(manager, 4);
    if (!devices || devicesEnd != devices + 2) {
        replay = {};
        return;
    }
    const bool firstIsLocal = InputDeviceUser(devices[0]) == replay.local;
    void* localDevice = devices[firstIsLocal ? 0 : 1];
    const auto remoteDevice = devices[firstIsLocal ? 1 : 0];
    if (InputDeviceUser(localDevice) != replay.local || InputDeviceUser(remoteDevice) != replay.peer) {
        replay = {};
        return;
    }

    replay.nextSend = now + 250;
    // The native broadcaster looks up the frame in the existing send history.
    // As in the game's own resend path, a missing entry is a no-op.
    const auto history = Read<const void*>(localDevice, DeviceSentInputs);
    if (!history || Read<unsigned>(localDevice, DeviceSentInputs + 4) == 0) {
        return;
    }
    const auto newest = Read<const void*>(history, 8);
    if (!newest || newest == history) {
        return;
    }
    const unsigned last = Read<unsigned>(newest, 16);
    if (last < replay.firstFrame) {
        return;
    }
    const unsigned distance = last - replay.firstFrame;
    for (unsigned offset = 0; offset < ReplayFrames && offset <= distance; ++offset) {
        broadcastFrameInput(localDevice, replay.firstFrame + offset);
    }
    logEvent(replay.peer, "room_rejoin_replay", "buffered_local_input", frame, replay.fromRoom, room.index);
}

void DumpEntities(const void* game, const char* when, bool force = false);
unsigned lastDesyncState;

bool __fastcall OnNetUpdate(void* manager, void*) {
    const bool result = originalNetUpdate(manager);
    if (logEvent) {
        // Any desync, not only right after a rejoin: dump every entity with
        // its flags (status effects etc.) once, so both sides can be compared.
        const unsigned desyncState = Read<unsigned>(manager, NetDesyncState);
        if (desyncState != 0 && lastDesyncState == 0) {
            if (const auto game = *At<const void**>(GameRva)) {
                DumpEntities(game, "on_desync", true);
            }
        }
        lastDesyncState = desyncState;
        AdvertiseRoomRejoin();
        ReplayRoomInputs(manager);
        if (guestGame) {
            const auto game = *At<const void**>(GameRva);
            const unsigned frame = Read<unsigned>(manager, NetFrame);
            const bool advanced = frame >= guestStartFrame && frame - guestStartFrame >= ReplayFrames;
            if (game != guestGame || frame < guestStartFrame) {
                guestGame = nullptr;
            } else if (advanced || GetTickCount64() >= guestDeadline) {
                logEvent(0, "room_rejoin_guest_progress", advanced ? "advanced" : "timeout",
                         frame, -1, Read<int>(game, GameRoomIndex));
                guestGame = nullptr;
            }
        }
    }
    return result;
}

void __fastcall ObserveSnapshotParity(const void* app, unsigned parity) {
    markedGuestApp = nullptr;
    markedHostManager = nullptr;
    // Vanilla reads only bit zero. The extra bit identifies the room
    // snapshot for both the guest and the host's local copy.
    if (!logEvent || (parity & 0x80) == 0) {
        return;
    }
    const auto* manager = static_cast<const uint8_t*>(app) + AppNetManager;
    const unsigned frame = Read<unsigned>(manager, NetFrame);
    // The message's RNG state was applied just before this parity byte.
    std::memcpy(receivedRng, At<const void*>(GlobalRngStateRva), sizeof(receivedRng));
    receivedRngIndex = *At<const int*>(GlobalRngIndexRva);
    receivedRngValid = true;
    if (Read<unsigned>(app, 8) == 2) {
        if (manager == replay.manager && frame == replay.firstFrame) {
            markedHostManager = manager;
        }
    } else if (advertisedLobby && CurrentSteamLobby(image) == advertisedLobby) {
        markedGuestApp = app;
        markedGuestFrame = frame;
    }
}

__declspec(naked) void OnWriteParity() {
    __asm {
        pushfd
        cmp byte ptr [writingRoomSnapshot], 0
        je storeParity
        or al, 80h
    storeParity:
        popfd
        mov byte ptr [ebp - 1FE3Dh], al
        ret
    }
}

__declspec(naked) void OnReadParity() {
    __asm {
        pushad
        mov ecx, esi
        movzx edx, byte ptr [ebp - 0Dh]
        call ObserveSnapshotParity
        popad
        movzx ecx, byte ptr [ebp - 0Dh]
        and ecx, 1
        ret
    }
}

int __fastcall OnCaptureRoomSnapshot(void* game, void*, void* snapshot) {
    const int result = originalCaptureGameState(game, snapshot);
    if (writingRoomSnapshot && Read<unsigned>(snapshot, SavedPlayerCount) == 2) {
        // Joining creates a second character with starting counters. Persist
        // the host's pre-join shared pool for both characters.
        for (size_t i = 0; i < 2; ++i) {
            const size_t offset = SavedPlayers + i * SavedPlayerSize;
            Write(snapshot, offset + 32, stagedResources.coins);
            Write(snapshot, offset + 36, stagedResources.keys);
            Write(snapshot, offset + 40, stagedResources.bombs);
            Write(snapshot, offset + 44, stagedResources.goldenKey);
            Write(snapshot, offset + 45, stagedResources.goldenBomb);
        }
    }
    return result;
}

bool RestoreReceivedRng(SteamId peer, unsigned frame, const char* side) {
    if (!receivedRngValid) {
        logEvent(peer, "room_rejoin_rng", "missing", frame, -1, -1);
        return false;
    }
    std::memcpy(At<void*>(GlobalRngStateRva), receivedRng, sizeof(receivedRng));
    *At<int*>(GlobalRngIndexRva) = receivedRngIndex;
    receivedRngValid = false;
    logEvent(peer, "room_rejoin_rng", side, frame, receivedRngIndex, -1);
    return true;
}

// The host's join path can leave an item overlay pending (it fires a few
// frames later and freezes the host's world); the guest never has one.
void ResetItemOverlay(uint8_t* game, SteamId peer, unsigned frame, const char* side) {
    const int state = Read<int>(game, GameItemOverlay);
    if (state != 0) {
        Write(game, GameItemOverlay, 0);
    }
    logEvent(peer, "room_rejoin_overlay", side, frame, state, 0);
}

bool SetRestoredRoomControls(const void* game, uint8_t enabled) {
    if (Read<unsigned>(game, GameRoomTransition) != 0 ||
        Read<unsigned>(game, GameStageTransition) != 0) {
        return false;
    }
    const auto players = Read<uint8_t* const*>(game, GamePlayers);
    const auto end = Read<uint8_t* const*>(game, GamePlayers + 4);
    if (!players || end != players + 2 || !players[0] || !players[1] || players[0] == players[1]) {
        return false;
    }
    players[0][PlayerControlsEnabled] = enabled;
    players[1][PlayerControlsEnabled] = enabled;
    return true;
}

// Called on both clients with the same join frame. The vanilla join loader
// leaves ControlsEnabled cleared; keep it that way until enableFrame.
bool ArmControlGate(const void* game, const void* manager, SteamId peer, unsigned joinFrame) {
    if (!SetRestoredRoomControls(game, 0)) {
        controlGate = {};
        return false;
    }
    controlGate = {game, manager, peer, joinFrame, joinFrame + ControlDelayFrames};
    diagFirstFrame = joinFrame;
    diagLastFrame = joinFrame + DiagFrames;
    diagActive = true;
    return true;
}

// Gives control back to every player in the game, whatever their count.
// Used when the join failed (e.g. desync recovery removed the guest) so the
// remaining player is never left frozen.
int ReleaseAllControls(const void* game) {
    if (!game || Read<unsigned>(game, GameRoomTransition) != 0 ||
        Read<unsigned>(game, GameStageTransition) != 0) {
        return -1;
    }
    const auto players = Read<uint8_t* const*>(game, GamePlayers);
    const auto end = Read<uint8_t* const*>(game, GamePlayers + 4);
    if (!players || end < players || end - players > 8) {
        return -1;
    }
    int released = 0;
    for (auto player = players; player != end; ++player) {
        if (*player && (*player)[PlayerControlsEnabled] == 0) {
            (*player)[PlayerControlsEnabled] = 1;
            ++released;
        }
    }
    return released;
}

void AbandonControlGate(const void* game, const char* reason, unsigned frame) {
    const int released = ReleaseAllControls(game);
    logEvent(controlGate.peer, "room_rejoin_controls", reason, frame, released, -1);
    controlGate = {};
}

void UpdateControlGate(const void* game) {
    if (!controlGate.game) {
        return;
    }
    if (hostRngPending) {
        hostRngPending = false;
        RestoreReceivedRng(controlGate.peer, controlGate.joinFrame, "restored_late");
    }
    const auto app = *At<const uint8_t**>(AppRva);
    if (game != controlGate.game || !app || app + AppNetManager != controlGate.manager ||
        Read<unsigned>(app, 8) != 2) {
        AbandonControlGate(game, "gate_dropped", controlGate.joinFrame);
        return;
    }
    const unsigned frame = Read<unsigned>(controlGate.manager, NetFrame);
    const auto players = Read<uint8_t* const*>(game, GamePlayers);
    const auto end = Read<uint8_t* const*>(game, GamePlayers + 4);
    if (!players || end != players + 2) {
        // Desync recovery or a disconnect removed a player: nothing to sync.
        AbandonControlGate(game, "players_changed", frame);
        return;
    }
    if (frame < controlGate.joinFrame || frame - controlGate.joinFrame >= ControlGateTimeoutFrames) {
        AbandonControlGate(game, "gate_timeout", frame);
        return;
    }
    if (frame < controlGate.enableFrame) {
        // Re-assert every simulated frame so neither side can drift.
        SetRestoredRoomControls(game, 0);
        return;
    }
    if (SetRestoredRoomControls(game, 1)) {
        logEvent(controlGate.peer, "room_rejoin_controls", "enabled", frame, -1, Read<int>(game, GameRoomIndex));
        controlGate = {};
    }
}

void LogJoinPassPlayers(SteamId peer, const char* side) {
    const auto app = *At<const uint8_t**>(AppRva);
    const auto game = *At<const uint8_t**>(GameRva);
    if (!app || !game) {
        return;
    }
    const unsigned frame = Read<unsigned>(app, AppNetManager + NetFrame);
    const auto players = Read<const uint8_t* const*>(game, GamePlayers);
    const auto end = Read<const uint8_t* const*>(game, GamePlayers + 4);
    logEvent(peer, side, "stage_transition", frame, Read<int>(game, GameStageTransition),
             Read<int>(game, GameRoomTransition));
    for (auto player = players; player && player < end && player - players < 8; ++player) {
        if (*player) {
            const auto flags = Read<uint64_t>(*player, EntityFlags);
            logEvent(peer, side, "player_flags_lo_hi", frame, static_cast<int>(flags), static_cast<int>(flags >> 32));
            logEvent(peer, side, "player_active_controls", frame, Read<uint8_t>(*player, EntityActive),
                     Read<uint8_t>(*player, PlayerControlsEnabled));
        }
    }
}

// Guest only: diagnostics for the post-join entity pass inside StartNetworked.
// Guest only: StartPendingNetworkGame arms a stage transition before
// StartNetworked runs this pass, so the guest's players took a different
// Player::Update path than the host's (no RNG draws, sprites one frame
// apart). Run the pass with the transition idle, exactly like the host.
int __cdecl OnGuestJoinPass() {
    const auto game = *At<uint8_t**>(GameRva);
    const bool roomJoin = logEvent && markedGuestApp && game;
    uint32_t transition[3]{};
    if (roomJoin) {
        LogJoinPassPlayers(0, "room_rejoin_guest_pass");
        std::memcpy(transition, game + GameStageTransition, sizeof(transition));
        Write<uint32_t>(game, GameStageTransition, 0);
    }
    const int result = originalJoinEntityPass();
    if (roomJoin) {
        std::memcpy(game + GameStageTransition, transition, sizeof(transition));
    }
    return result;
}

// Host only: the game-start handler runs a post-join entity pass after
// LoadJoinedGameState. The guest runs its own variant inside StartNetworked.
// That pass records the join frame's checksum. On the guest the players do
// not run their non-interpolated logic in it (no global RNG draws), on the
// host they do; mark them as an interpolation update so both match.
int __cdecl OnHostJoinPass() {
    uint8_t* marked[8]{};
    size_t markedCount = 0;
    const auto game = *At<uint8_t**>(GameRva);
    if (logEvent && hostRngPending && game) {
        LogJoinPassPlayers(replay.peer, "room_rejoin_host_pass");
        const auto players = Read<uint8_t* const*>(game, GamePlayers);
        const auto end = Read<uint8_t* const*>(game, GamePlayers + 4);
        // Disabled: the guest now runs the same full pass (see OnGuestJoinPass).
        for (auto player = players; MarkHostPlayersInPass && player && player < end && markedCount < 8; ++player) {
            if (*player && (Read<uint32_t>(*player, EntityFlags) & FlagInterpolationUpdate) == 0) {
                Write(*player, EntityFlags, Read<uint32_t>(*player, EntityFlags) | FlagInterpolationUpdate);
                marked[markedCount++] = *player;
            }
        }
    }
    const int result = originalJoinEntityPass();
    for (size_t i = 0; i < markedCount; ++i) {
        Write(marked[i], EntityFlags, Read<uint32_t>(marked[i], EntityFlags) & ~FlagInterpolationUpdate);
    }
    if (logEvent && hostRngPending) {
        hostRngPending = false;
        const auto app = *At<const uint8_t**>(AppRva);
        const unsigned frame = app ? Read<unsigned>(app, AppNetManager + NetFrame) : 0;
        RestoreReceivedRng(replay.peer, frame, "restored_host");
        if (const auto liveGame = *At<uint8_t**>(GameRva)) {
            ResetItemOverlay(liveGame, replay.peer, frame, "reset_host");
        }
    }
    return result;
}

using GameLog = void (__cdecl *)(int, const char*, ...);

void DumpEntities(const void* game, const char* when, bool force) {
    const auto app = *At<const uint8_t**>(AppRva);
    if (!app) {
        diagActive = false;
        return;
    }
    const unsigned frame = Read<unsigned>(app, AppNetManager + NetFrame);
    if (!force && (frame < diagFirstFrame || frame > diagLastFrame)) {
        diagActive = frame < diagFirstFrame;
        return;
    }
    const auto log = At<GameLog>(GameLogRva);
    const auto room = Read<const void*>(game, GameRoom);
    const auto entities = room ? Read<const uint8_t* const*>(room, RoomEntityList) : nullptr;
    const unsigned count = room ? Read<unsigned>(room, RoomEntityCount) : 0;
    log(1, "[BetterCoop] [Frame %u] diag %s: rng_index=%d entities=%u stage=%d room_tr=%d\n", frame, when,
        *At<const int*>(GlobalRngIndexRva), count, Read<int>(game, GameStageTransition),
        Read<int>(game, GameRoomTransition));
    // Every field Isaac_Game_Update can bail out on before the world update,
    // plus the world frame counter (+0x264F8) that only advances past them.
    log(1, "[BetterCoop] [Frame %u] diag %s: world_frame=%d freeze=%d m1D520=%d m1D654=%d m1C034=%d "
        "overlay=%d:%d m26778=%d room_f4600=%.3f\n", frame, when,
        Read<int>(game, 0x264F8), Read<int>(game, 0x264F4), Read<int>(game, 0x1D520), Read<int>(game, 0x1D654),
        Read<int>(game, 0x1C034), Read<int>(game, 429432), Read<int>(game, 429436), Read<int>(game, 157560),
        room ? static_cast<double>(Read<float>(room, 4600)) : -1.0);
    for (unsigned i = 0; entities && i < count && i < 160; ++i) {
        const auto e = entities[i];
        if (!e) {
            continue;
        }
        log(1, "[BetterCoop] [Frame %u] diag %s: #%u %d.%d.%d pos(%.2f,%.2f) flags=%08x:%08x ctl=%u\n", frame, when, i,
            Read<int>(e, EntityType), Read<int>(e, EntityVariant), Read<int>(e, EntitySubtype),
            static_cast<double>(Read<float>(e, EntityPosition)), static_cast<double>(Read<float>(e, EntityPosition + 4)),
            Read<uint32_t>(e, EntityFlags + 4), Read<uint32_t>(e, EntityFlags),
            Read<int>(e, EntityType) == 1 ? Read<uint8_t>(e, PlayerControlsEnabled) : 0u);
        if (Read<int>(e, EntityType) == 1) {
            // Cached stats (offsets from the Lua EntityPlayer bindings).
            log(1, "[BetterCoop] [Frame %u] diag %s: #%u stats dmg=%.4f firedelay=%.4f shotspeed=%.4f range=%.2f "
                "height=%.3f fall=%.3f:%.3f speed=%.4f tearflags=%08x:%08x:%08x:%08x\n", frame, when, i,
                static_cast<double>(Read<float>(e, 0x1470)), static_cast<double>(Read<float>(e, 0x1460)),
                static_cast<double>(Read<float>(e, 0x1464)), static_cast<double>(Read<float>(e, 0x1480)),
                static_cast<double>(Read<float>(e, 0x1474)), static_cast<double>(Read<float>(e, 0x1478)),
                static_cast<double>(Read<float>(e, 0x147C)), static_cast<double>(Read<float>(e, 0x1568)),
                Read<uint32_t>(e, 0x1494), Read<uint32_t>(e, 0x1490), Read<uint32_t>(e, 0x148C), Read<uint32_t>(e, 0x1488));
        }
        // Full raw player object once per window (and on desync) so both
        // sides can be diffed for state the game's checksum does not cover.
        if (Read<int>(e, EntityType) == 1 && (force || (frame == diagLastFrame && when[0] == 'a'))) {
            constexpr size_t PlayerObjectSize = 0x2F00;
            constexpr size_t PerLine = 32;
            for (size_t offset = 0; offset < PlayerObjectSize; offset += PerLine * 4) {
                uint32_t w[PerLine];
                std::memcpy(w, e + offset, sizeof(w));
                log(1, "[BetterCoop] [Frame %u] raw %s #%u +%04x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                    "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                    "%08x %08x %08x\n", frame, when, i, static_cast<unsigned>(offset),
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14],
                    w[15], w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23], w[24], w[25], w[26], w[27], w[28],
                    w[29], w[30], w[31]);
            }
        }
    }
}

void __fastcall OnGameUpdate(void* game, void*) {
    if (logEvent) {
        UpdateControlGate(game);
        if (diagActive) {
            DumpEntities(game, "before_update");
        }
    }
    originalGameUpdate(game);
    if (logEvent && diagActive) {
        DumpEntities(game, "after_update");
    }
}

int __fastcall OnRestoreHost(void* manager, void*, void* snapshot, int roster, int flags) {
    const bool roomSnapshot = logEvent && manager == markedHostManager;
    markedHostManager = nullptr;
    const int result = originalRestoreJoinedGame(manager, snapshot, roster, flags);
    if (roomSnapshot) {
        const auto game = *At<const void**>(GameRva);
        const auto& room = replay.destination;
        const bool restored = game && game == room.game && Read<int>(game, GameRoomIndex) == room.index &&
            Read<int>(game, GameDimension) == room.dimension && Read<int>(game) == room.stage &&
            Read<int>(game, 4) == room.stageType &&
            ArmControlGate(game, manager, replay.peer, Read<unsigned>(manager, NetFrame));
        hostRngPending = restored;
        logEvent(replay.peer, "room_rejoin_host_controls", restored ? "deferred" : "unexpected_room_or_players",
                 Read<unsigned>(manager, NetFrame), replay.fromRoom, room.index);
    }
    return result;
}

void __fastcall OnStartPending(void* app, void*) {
    const bool roomSnapshot = logEvent && app == markedGuestApp &&
        Read<unsigned>(app, AppNetManager + NetFrame) == markedGuestFrame &&
        Read<uint8_t>(app, AppHasPendingSnapshot) != 0;
    originalStartPending(app);
    if (roomSnapshot && Read<unsigned>(app, 8) == 2 &&
        Read<uint8_t>(app, AppHasPendingSnapshot) == 0) {
        markedGuestApp = nullptr;
        auto* game = *At<uint8_t**>(GameRva);
        if (game && Read<unsigned>(game, GameStageTransition) == 2 &&
            Read<unsigned>(game, GameStageTransition + 4) == 49 &&
            Read<unsigned>(game, GameStageTransition + 8) == 50) {
            constexpr unsigned idle = 0;
            std::memcpy(game + GameStageTransition, &idle, sizeof(idle));
            RestoreReceivedRng(0, markedGuestFrame, "restored_guest");
            ResetItemOverlay(game, 0, markedGuestFrame, "reset_guest");
            const bool armed = ArmControlGate(game, static_cast<uint8_t*>(app) + AppNetManager, 0, markedGuestFrame);
            guestGame = game;
            guestStartFrame = markedGuestFrame;
            guestDeadline = GetTickCount64() + 8000;
            logEvent(0, "room_rejoin_guest_ready", armed ? "controls_deferred" : "unexpected_room_or_players", markedGuestFrame,
                     -1, Read<int>(game, GameRoomIndex));
        }
    }
}

int __fastcall OnRoomUpdate(void* transition, void*) {
    const auto game = *At<const uint8_t**>(GameRva);
    const bool observe = logEvent && game && game + GameRoomTransition == transition &&
        Read<unsigned>(transition, 28) == 0;
    if (observe && Read<unsigned>(transition) == 1) {
        sourceRoom = CurrentRoom(game);
    }
    const int result = originalUpdate(transition);
    if (observe && *At<const uint8_t**>(GameRva) == game && Read<unsigned>(transition) == 0) {
        const Room source = sourceRoom;
        sourceRoom = {};
        const Room destination = CurrentRoom(game);
        if (source.game && OrdinaryRoomChange(source, destination)) {
            TryRoomRejoin(source, destination);
        }
    }
    return result;
}

template <size_t N>
bool PatchCall(uintptr_t rva, const uint8_t (&expected)[N], uintptr_t replacement) {
    static_assert(N >= 5);
    auto* call = At<uint8_t*>(rva);
    // A site already redirected by an older copy of this mod (a hotfix loaded
    // into a running game) is a call followed by NOP padding. Take it over.
    bool hooked = call[0] == 0xE8;
    for (size_t i = 5; hooked && i < N; ++i) {
        hooked = call[i] == 0x90;
    }
    if (std::memcmp(call, expected, N) != 0 && !hooked) {
        return false;
    }
    DWORD protection;
    if (!VirtualProtect(call, N, PAGE_EXECUTE_READWRITE, &protection)) {
        return false;
    }
    uint8_t bytes[N];
    std::memset(bytes, 0x90, N);
    bytes[0] = 0xE8;
    const auto displacement = replacement - reinterpret_cast<uintptr_t>(call + 5);
    std::memcpy(bytes + 1, &displacement, sizeof(displacement));
    std::memcpy(call, bytes, N);
    FlushInstructionCache(GetCurrentProcess(), call, N);
    DWORD oldProtection;
    VirtualProtect(call, N, protection, &oldProtection);
    return true;
}

}

void EnableRoomRejoin(Log logger) {
    image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos = At<const IMAGE_DOS_HEADER*>(0);
    const auto nt = At<const IMAGE_NT_HEADERS*>(dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage < CurrentLobbyRva + 8) {
        logger(0, "room_rejoin_unavailable", "image_size", 0, -1, -1);
        return;
    }
    originalUpdate = At<UpdateRoomTransition>(RoomUpdateRva);
    originalNetUpdate = At<UpdateNetManager>(NetUpdateRva);
    originalStartPending = At<StartPendingGame>(StartPendingRva);
    originalRestoreJoinedGame = At<RestoreJoinedGame>(RestoreJoinedGameRva);
    originalCaptureGameState = At<CaptureGameState>(CaptureGameStateRva);
    originalGameUpdate = At<UpdateGame>(GameUpdateRva);
    originalJoinEntityPass = At<JoinEntityPass>(JoinEntityPassRva);
    processPendingJoins = At<ProcessPendingJoins>(ProcessJoinsRva);
    broadcastFrameInput = At<BroadcastFrameInput>(BroadcastInputRva);
    if (!PatchCall(RoomUpdateCallRva, {0xE8, 0x7F, 0x63, 0x13, 0x00}, reinterpret_cast<uintptr_t>(OnRoomUpdate)) ||
        !PatchCall(NetUpdateCallRva, {0xE8, 0x9E, 0x60, 0xFB, 0xFF}, reinterpret_cast<uintptr_t>(OnNetUpdate)) ||
        !PatchCall(StartPendingCallRva, {0xE8, 0xBD, 0x49, 0x00, 0x00}, reinterpret_cast<uintptr_t>(OnStartPending)) ||
        !PatchCall(RestoreHostCallRva, {0xE8, 0xD0, 0x5A, 0xFF, 0xFF}, reinterpret_cast<uintptr_t>(OnRestoreHost)) ||
        !PatchCall(GuestJoinPassCallRva, {0xE8, 0x5B, 0x7A, 0x21, 0x00}, reinterpret_cast<uintptr_t>(OnGuestJoinPass)) ||
        !PatchCall(HostJoinPassCallRva, {0xE8, 0xF4, 0x06, 0x00, 0x00}, reinterpret_cast<uintptr_t>(OnHostJoinPass)) ||
        !PatchCall(GameUpdateCallRva, {0xE8, 0x8A, 0x5B, 0xDA, 0xFF}, reinterpret_cast<uintptr_t>(OnGameUpdate)) ||
        !PatchCall(CaptureSnapshotCallRva, {0xE8, 0xF2, 0x1A, 0xDE, 0xFF}, reinterpret_cast<uintptr_t>(OnCaptureRoomSnapshot)) ||
        !PatchCall(WriteParityRva, {0x88, 0x85, 0xC3, 0x01, 0xFE, 0xFF}, reinterpret_cast<uintptr_t>(OnWriteParity)) ||
        !PatchCall(ReadParityRva, {0x0F, 0xB6, 0x4D, 0xF3, 0x83, 0xE1, 0x01}, reinterpret_cast<uintptr_t>(OnReadParity))) {
        logger(0, "room_rejoin_unavailable", "patch_call", 0, -1, -1);
        return;
    }
    logEvent = logger;
    logEvent(0, "room_rejoin_ready", "room_snapshot_v11", 0, -1, -1);
}

// Used by the hotfix: patch with a silent logger while other threads are
// suspended, then switch to the real one after they resume.
void SetRoomRejoinLogger(Log logger) {
    if (logEvent) {
        logEvent = logger;
        logEvent(0, "room_rejoin_ready", "room_snapshot_v11_hotfix", 0, -1, -1);
    }
}

// Used by the hotfix: an earlier build could leave a solo host frozen after a
// failed join. Only touches a lone player, so no remote peer can diverge.
void ReleaseStuckSoloControls() {
    const auto app = *At<const uint8_t**>(AppRva);
    const auto game = *At<const void**>(GameRva);
    if (!logEvent || !app || !game || Read<unsigned>(app, 8) != 2) {
        return;
    }
    const auto players = Read<uint8_t* const*>(game, GamePlayers);
    const auto end = Read<uint8_t* const*>(game, GamePlayers + 4);
    if (!players || end != players + 1 || !players[0] || players[0][PlayerControlsEnabled] != 0) {
        return;
    }
    const int released = ReleaseAllControls(game);
    logEvent(0, "room_rejoin_controls", released > 0 ? "solo_unstuck" : "solo_unstuck_failed",
             Read<unsigned>(app, AppNetManager + NetFrame), released, Read<int>(game, GameRoomIndex));
}

void InitializeRoomRejoin(Log logger) {
    char enabled[2];
    if (GetEnvironmentVariableA("ISAAC_NETDIAG_ROOM_REJOIN", enabled, sizeof(enabled)) == 1 &&
        enabled[0] == '1') {
        EnableRoomRejoin(logger);
    }
}

void DisableRoomRejoin() {
    logEvent = nullptr;
}
