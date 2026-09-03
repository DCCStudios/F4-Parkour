#pragma once

#include <atomic>
#include <cstdint>

namespace F4Parkour
{
	// ============================================================
	// Move classification
	// ============================================================
	enum class MoveKind : std::uint8_t
	{
		None = 0,
		Vault,   // up and over — momentum preserved
		Mantle,  // up and on top — momentum cleared
	};

	// Headroom above the on-top point of a mantle candidate.
	enum class Headroom : std::uint8_t
	{
		None = 0,    // cannot fit even crouched — reject
		CrouchOnly,  // fits crouched — mantle completes into sneak
		Stand,       // fits standing
	};

	// ============================================================
	// Candidate produced by the detection pass. Vault and mantle keep
	// fully separate geometry — the two scans measure different ledges
	// on the same obstacle and must never share fields (a stepped top
	// gives them different heights). All positions are world-space,
	// heights are relative to the player's feet.
	// ============================================================
	struct LedgeCandidate
	{
		// ---- vault (up and over) ----
		bool         vaultEligible{ false };
		RE::NiPoint3 vaultLedge{};      // highest point of the obstacle top
		float        vaultHeight{ 0.0f };
		float        vaultTopDepth{ 0.0f };
		RE::NiPoint3 vaultLanding{};    // far-side ground point
		float        vaultDrop{ 0.0f }; // ledge top -> far-side ground
		Headroom     vaultHeadroom{ Headroom::None }; // over the landing: CrouchOnly -> the Mover crouches the player on arrival
		bool         vaultNoFloor{ false };  // no floor in reach (or too deep to glide to): the landing IS the release point, always release

		// ---- mantle (up on top) ----
		bool         mantleEligible{ false };
		RE::NiPoint3 mantleLedge{};     // front lip of the ledge
		float        mantleHeight{ 0.0f };
		float        mantleDepth{ 0.0f };
		RE::NiPoint3 mantleTarget{};    // standing point on top
		Headroom     headroom{ Headroom::None };

		// ---- shared ----
		RE::NiPoint3 approachDir{};     // flattened player forward at detection
		bool         fromAir{ false };

		bool IsValid() const { return vaultEligible || mantleEligible; }

		// The reference ledge for aiming/look-cone/debug: prefer the move
		// the decision logic would prefer.
		const RE::NiPoint3& PrimaryLedge() const
		{
			return vaultEligible ? vaultLedge : mantleLedge;
		}

		void Reset() { *this = LedgeCandidate{}; }
	};

	// ============================================================
	// Mover phases. The apex is the vault/mantle commit point.
	// ============================================================
	enum class MovePhase : std::uint8_t
	{
		Idle = 0,
		Rising,    // before apex — conversion to mantle still allowed
		Committed, // past apex — the move always completes
	};
}
