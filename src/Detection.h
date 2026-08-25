#pragma once

#include "ParkourTypes.h"

namespace F4Parkour
{
	namespace Detection
	{
		// Player capsule constants (game units). The mantle headroom test
		// accepts either the standing or the crouched height, so mods that
		// adjust the crouch capsule (UneducatedShooter) stay compatible —
		// we enter sneak through the normal input path and let them own
		// the collision shape.
		constexpr float kPlayerHeight = 120.0f;
		constexpr float kCrouchHeight = 72.0f;
		constexpr float kCapsuleRadius = 16.0f;

		// Player state helpers (main thread).
		bool IsOnGround(RE::PlayerCharacter* a_player);
		bool IsInAir(RE::PlayerCharacter* a_player);
		bool IsSighted(RE::PlayerCharacter* a_player);   // ADS / scoped
		bool IsInPowerArmor(RE::PlayerCharacter* a_player);
		bool IsSprinting(RE::PlayerCharacter* a_player);
		bool IsForwardHeld();

		// Physical ground test: solid ground within a_maxDrop units below
		// the feet. The controller's m_currentState flickers kInAir while
		// standing on slopes and jagged bases (boulders), so the raw state
		// must never classify a move as an air start by itself (SkyParkour
		// distrusts it the same way: support flags + fallTime).
		bool GroundWithin(RE::PlayerCharacter* a_player, float a_maxDrop);
		RE::NiPoint3 DirFlat(RE::PlayerCharacter* a_player);
		float HorizontalSpeed(RE::PlayerCharacter* a_player);

		// True when the 16u-wide capsule can actually STAND at a_center
		// without overlapping geometry: a ring of down-rays across the
		// capsule footprint, cast from free space ABOVE (never probe
		// outward from the center — rays from inside convex shapes lie).
		// Only a RISE inside the ring fails (a rock bulge / wall face the
		// thin center ray threaded past — the "end the move clipped into
		// the rock" report); holes and drops stay tolerated so scan
		// permissiveness is unchanged.
		bool FootprintClear(const RE::NiPoint3& a_center);

		// One full detection pass. a_fromAir relaxes the grounded checks
		// and extends the mantle reach by Settings::airGrabExtraReach.
		// Fills the DebugDraw working frame as it goes (rays + markers +
		// reject reasons) when debug is enabled.
		LedgeCandidate Scan(RE::PlayerCharacter* a_player, bool a_fromAir);
	}
}
