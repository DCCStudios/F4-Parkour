#pragma once

#include <string>

namespace F4Parkour
{
	// INI-backed general settings. Feel presets (tiers/durations/curves)
	// live in Curves.h; this holds everything else the menu edits.
	class Settings
	{
	public:
		static Settings* GetSingleton()
		{
			static Settings singleton;
			return &singleton;
		}

		void Load();
		void Save();

		// ---- [General] ----
		bool  enabled{ true };
		bool  playMeleeAnim{ true };        // hijack the melee action for OAR
		bool  autoParkourSprint{ false };   // auto-vault while sprinting
		bool  autoStepUp{ false };          // auto-vault knee-height obstacles
		bool  allowInAir{ true };           // air-mantle after a jump
		bool  allowAirVault{ true };
		// High-tier mantles initiated FROM THE AIR (jump-then-grab) are the
		// most complex hand-off; this lets them be disabled independently
		// while grounded high mantles and low/mid air grabs keep working.
		bool  allowAirHighMantle{ true };
		// Vault while aiming down sights. Mantles stay blocked while
		// sighted either way (climbing a ledge scoped reads wrong); this
		// only lets a VAULT over a low obstacle fire mid-ADS.
		bool  allowVaultWhileAiming{ true };        // also air-VAULT (not just mantle) in the air
		bool  autoAirGrab{ false };         // auto-grab a ledge while falling (Dying Light assist)
		float airAutoGrabDelay{ 0.3f };     // fallTime before auto-grab arms
		bool  allowThirdPerson{ true };
		bool  indicatorEnabled{ true };     // HUD ring on above-LOW candidates
		// High-mantle camera director (first person): keeps the view level
		// and pulls it out of geometry so the rising eye never clips the
		// ledge. Scoped to tier-2 mantles only.
		// Procedural camera collision retired (fought the FP animation;
		// superseded by the authored-curve mantle rework). Off by default,
		// toggle kept for A/B only.
		bool  highMantleCameraDirector{ false };
		// High mantles follow the animation's exported root trajectory
		// (Curves/mantle_high.curve.json) in lockstep with Ledge.hkx.
		// Falls back to the procedural arcs when the file is absent.
		bool  authoredHighMantle{ true };
		// Fine-tune offsets (game units) on the clip's spatial contract:
		// grab offset shifts the anchor BACK from the lip (+ = away from
		// the wall, - = into it); land offset adds to the authored top-out
		// distance past the edge (+ = deeper onto the top, - = closer).
		// Defaults are the user-honed values for the shipped Ledge.hkx clip
		// (2026-08-24 playtest: 25/25 "seems to do it").
		float authoredGrabOffset{ 25.0f };
		float authoredGrabZOffset{ 25.0f };  // raises (+) / lowers (-) the grab height
		float authoredLandOffset{ 0.0f };
		float camCollisionSkin{ 12.0f };    // keep the view this far off surfaces
		bool  requireForward{ true };       // contextual rule: forward input
		float lookConeDeg{ 35.0f };         // must be looking at the ledge
		float detectionInterval{ 0.05f };   // seconds between scans
		std::string activePreset{ "Smooth" };

		// ---- [Input] ----
		float jumpBufferWindow{ 0.20f };    // buffered jump press (s)
		float coyoteWindow{ 0.15f };        // grounded grace after walk-off (s)
		bool  holdToMantle{ false };        // hold jump biases to mantle

		// ---- [Detection] ----
		float minVaultHeight{ 40.0f };
		float maxVaultHeight{ 110.0f };
		float minMantleHeight{ 40.0f };
		float maxMantleHeight{ 200.0f };
		float minMantleDepth{ 25.0f };      // thinner tops are vault-only
		float minBackClearance{ 40.0f };    // room needed past the far edge
		float maxVaultDrop{ 140.0f };       // landing may be this far below the ledge
		float maxApproachAngleDeg{ 45.0f }; // reject oblique approaches
		float autoEngageDistance{ 60.0f };  // auto-parkour trigger range
		float airGrabExtraReach{ 80.0f };   // added mantle height while airborne

		// ---- [Movement] ----
		float momentumKeep{ 1.0f };         // 0..1 of entry speed restored on vault exit
		float vaultSpeedMatch{ 0.5f };      // 0 = tier time always, 1 = full speed-match
		float exitDirBlend{ 0.5f };         // approach dir -> current input dir
		float momentumDropCutoff{ 100.0f }; // zero momentum when landing drop exceeds
		float controlHandback{ 0.25f };     // final fraction of move with input live
		bool  sneakOnCrouchOnly{ true };    // force sneak after crouch-headroom mantles

		// ---- [Comfort] ----
		bool  focusDot{ true };
		float focusDotAlpha{ 0.35f };

		// ---- [Testing] ----
		// Per-tier: play a test idle (PlayIdle) at move start instead of
		// the melee action, so animations can be iterated without OAR.
		bool testIdleVault[3]{ false, false, false };   // low / mid / high
		bool testIdleMantle[3]{ false, false, false };
		// Recipe extracted from Inspectweapons.esl (1959 working weapon-
		// drawn FP idles): animFileName INCLUDES the Meshes\ prefix, the
		// event is dyn_ActivationAllowMovement, and behaviorGraphName must
		// name the FIRST-PERSON root behavior or SetupSpecialIdle refuses.
		std::string testIdlePath{ "Meshes\\Actors\\Character\\Animation\\F4Parkour\\Ledge.hkx" };
		std::string testIdleEvent{ "dyn_ActivationAllowMovement" };
		std::string testIdleBehavior{ "actors\\Character\\_1stPerson\\Behaviors\\RootBehavior.hkx" };

		// Second test-idle slot: plays Mantle.hkx and additionally SKIPS the
		// fast-equip animation that normally plays when an idle ends (the
		// SeamlessInspect technique). Slot 2 wins over slot 1 on a tier that
		// has both on.
		bool testIdle2Vault[3]{ false, false, false };
		bool testIdle2Mantle[3]{ false, false, false };
		std::string testIdle2Path{ "Meshes\\Actors\\Character\\Animations\\F4Parkour\\Vault.hkx" };
		std::string testIdle2Event{ "dyn_ActivationAllowMovement" };

		// ---- [Debug] ----
		bool  debugEnabled{ false };
		bool  drawRays{ true };
		bool  drawPath{ true };
		bool  drawOnlyFailed{ false };
		bool  freezeDetection{ false };
		float moverTimeScale{ 1.0f };       // slow-motion mover for inspection
		bool  watchdogEnabled{ true };
		// Per-frame scene update during a move: false = "moved" (smooth,
		// low culling churn); true = "warp"/teleport path (legacy). Every
		// mover frame passing the teleport path forced the pre-cull/cull
		// caches to rebuild 30-60x per mantle - a plausible trigger for
		// the BSPreCulledObjects render-thread crash. Default false; flip
		// true only if the character mesh visibly trails the capsule.
		bool  warpSceneEachFrame{ false };

	private:
		Settings() = default;
		static constexpr const char* kINIPath = "Data/F4SE/Plugins/F4Parkour.ini";
	};
}
