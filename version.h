#pragma once

// Internal build and room-rejoin compatibility revision. Change it here only.
// GitHub release tags (for example v0.2.1) are managed separately.
#define BETTERCOOP_VERSION 19

#define BETTERCOOP_STRINGIFY_IMPL(value) #value
#define BETTERCOOP_STRINGIFY(value) BETTERCOOP_STRINGIFY_IMPL(value)
#define BETTERCOOP_WIDEN_IMPL(value) L##value
#define BETTERCOOP_WIDEN(value) BETTERCOOP_WIDEN_IMPL(value)

static_assert(BETTERCOOP_VERSION > 0, "BETTERCOOP_VERSION must be positive");

namespace BetterCoop {
inline constexpr char Version[] = BETTERCOOP_STRINGIFY(BETTERCOOP_VERSION);
inline constexpr char RoomSnapshotMarker[] = "room_snapshot_v" BETTERCOOP_STRINGIFY(BETTERCOOP_VERSION);
inline constexpr char HotfixMarker[] = "room_snapshot_v" BETTERCOOP_STRINGIFY(BETTERCOOP_VERSION) "_hotfix";
inline constexpr wchar_t HotfixMarkerWide[] =
    L"room_snapshot_v" BETTERCOOP_WIDEN(BETTERCOOP_STRINGIFY(BETTERCOOP_VERSION)) L"_hotfix";
}
