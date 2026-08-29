#pragma once

#include "ParkourTypes.h"

#include <atomic>
#include <chrono>

namespace F4Parkour
{
	// ============================================================
	// Animation integration: pre-armed runtime keywords + the hijacked
	// melee action. OAR conditions (HasKeyword) select the replacement
	// clip; movement never depends on any of this succeeding.
	//
	// Keyword editor IDs (created at kGameDataReady, reused if an ESP
	// already defines them — the SuperSprint pattern):
	//   AnimsParkourKeyword             any parkour move
	//   AnimsParkourVaultKeyword        any vault
	//   AnimsParkourMantleKeyword       any mantle
	//   AnimsParkourVaultLowKeyword     + one per height tier, chosen by
	//   AnimsParkourVaultMidKeyword       the nearest preset tier height,
	//   AnimsParkourVaultHighKeyword      so OAR can assign a different
	//   AnimsParkourMantleLowKeyword      animation to each tier
	//   AnimsParkourMantleMidKeyword
	//   AnimsParkourMantleHighKeyword
	// Graph variables set alongside: F4Parkour_Type (-1 none, 0 vault,
	// 1 mantle), F4Parkour_Height (ledge height, game units).
	// ============================================================
	class AnimHijack
	{
	public:
		static AnimHijack* GetSingleton()
		{
			static AnimHijack singleton;
			return &singleton;
		}

		void Init();  // create/find keywords + the ActionMelee form
		void OnGameLoaded();

		// Mover callbacks. Keywords are added BEFORE the melee action
		// fires — OAR evaluates conditions at clip activation (verified
		// in the FPGO ADS-reload work), so arming must come first.
		// a_tier: 0 low / 1 mid / 2 high (nearest preset tier height).
		void OnMoveStart(RE::PlayerCharacter* a_player, MoveKind a_kind, float a_height, int a_tier);
		void OnMoveConverted(RE::PlayerCharacter* a_player, int a_mantleTier);  // vault→mantle swap
		void OnMoveEnd(RE::PlayerCharacter* a_player, MoveKind a_kind);

		// Crouch-only mantle completion: enter sneak through the normal
		// input path (UneducatedShooter and friends keep owning the
		// collision shape).
		void RequestSneak(RE::PlayerCharacter* a_player);

		// Called from the player anim-graph event hook (main thread).
		// Currently a no-op: the equip-skip it used to run on "IdleStop"
		// is RETIRED (2026-08-27) — the post-idle fast-equip is handled
		// OAR-side now, and the UpdateAnimation(1000) fast-forward
		// integrated the huge delta into player movement (the post-vault
		// teleport). The hook stays installed for future event needs.
		void OnAnimEvent(const RE::BSFixedString& a_tag);

		// One retry for a test idle the engine refused at move start
		// (called at the apex by the Mover).
		void RetryTestIdle(RE::PlayerCharacter* a_player);

	private:
		AnimHijack() = default;

		void AddKeyword(RE::PlayerCharacter* a_player, RE::BGSKeyword* a_kw);
		void RemoveKeyword(RE::PlayerCharacter* a_player, RE::BGSKeyword* a_kw);
		void RemoveAll(RE::PlayerCharacter* a_player);
		void InstallAnimEventHook();  // player+0x38 ProcessEvent slot 1

		RE::BGSKeyword* kwParkour{ nullptr };
		RE::BGSKeyword* kwVault{ nullptr };
		RE::BGSKeyword* kwMantle{ nullptr };
		RE::BGSKeyword* kwVaultTier[3]{};   // low / mid / high
		RE::BGSKeyword* kwMantleTier[3]{};
		RE::BGSAction*  actionMelee{ nullptr };
		RE::TESIdleForm* testIdle{ nullptr };  // runtime idle for the test toggles

		bool animEventHookInstalled{ false };
		bool idleRetryPending{ false };
	};
}
