#pragma once

#include "ParkourTypes.h"
#include "Curves.h"
#include "AuthoredCurve.h"

namespace F4Parkour
{
	// ============================================================
	// The Mover executes one vault or mantle: a curve-driven position
	// update between geometry-derived keypoints, with the vault→mantle
	// intent conversion window before the apex, an in-flight
	// obstruction watchdog, and momentum restore on vault exit.
	//
	// THREADING: the Mover copies the active FeelPreset by value at
	// Start, so menu-thread curve edits and preset switches can never
	// touch the vectors a live move is evaluating.
	// ============================================================
	class Mover
	{
	public:
		static Mover* GetSingleton()
		{
			static Mover singleton;
			return &singleton;
		}

		// Start a move. a_kind must be eligible on the candidate.
		// Returns false if activation-time path validation failed.
		// a_dryRun runs the full setup + validation WITHOUT activating —
		// the manager uses it every detection tick so the indicator and
		// the jump decision can never disagree with a real press.
		bool Start(RE::PlayerCharacter* a_player, const LedgeCandidate& a_candidate, MoveKind a_kind, bool a_dryRun = false);

		// Per-frame update while active. a_dt is real seconds.
		void Update(RE::PlayerCharacter* a_player, float a_dt);

		// Per-frame tick while IDLE (the manager calls this when no move is
		// active): finishes post-move graph work — currently the jumpLand
		// retry that resolves a hijacked jump's animation state under live
		// simulation, where the graph provably accepts the event.
		void PostMoveTick(RE::PlayerCharacter* a_player, float a_dt);

		// Abort and restore. Player returns to the start position if
		// before the apex, else settles on the ledge top.
		void Cancel(RE::PlayerCharacter* a_player);

		// State wipe on game load. When a_player is given, also clears
		// kNoSim, camera dip residue, and velocity so a mid-move load
		// can never leave the controller wedged.
		void HardReset(RE::PlayerCharacter* a_player);

		bool IsActive() const { return active; }

		// Landing-guard handoff: set by Finish (not Cancel). The manager
		// consumes it and watches the first ~0.6s after the move — if the
		// player gets shoved backward or drops below the verified landing,
		// RescueTo snaps them to where the move intended, never backward.
		bool ConsumeLandingGuard(RE::NiPoint3& a_outPos, RE::NiPoint3& a_outDir)
		{
			if (!landingGuardValid) return false;
			landingGuardValid = false;
			a_outPos = landingGuardPos;
			a_outDir = landingGuardDir;
			return true;
		}
		static void RescueTo(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_pos);

		// Landing-guard correction: a short smoothstep GLIDE to the target
		// instead of a teleport (a snap every move end reads as constant
		// rubber-banding). No animation, no keywords, no guard re-arm.
		void StartCorrection(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_target, float a_duration);
		MoveKind ActiveKind() const { return kind; }
		MovePhase Phase() const { return phase; }
		float Progress() const { return duration > 0.0f ? std::min(1.0f, t / duration) : 0.0f; }

	private:
		Mover() = default;

		RE::NiPoint3 SamplePath(float a_s) const;  // position at eased progress s
		bool ValidatePath(const RE::NiPoint3& a_ledge) const;
		void Finish(RE::PlayerCharacter* a_player);
		void ApplyConversionToMantle();
		void OnCommitted(RE::PlayerCharacter* a_player);  // apex crossed
		float ArcPeakX() const;  // argmax of the blended arc for this move

		bool         active{ false };
		MoveKind     kind{ MoveKind::None };
		MovePhase    phase{ MovePhase::Idle };
		LedgeCandidate cand{};
		FeelPreset   preset{};        // by-value snapshot taken at Start

		RE::NiPoint3 startPos{};
		RE::NiPoint3 endPos{};
		RE::NiPoint3 dir{};           // approach direction (flat, normalized)
		float        apexZ{ 0.0f };   // peak height the path must reach
		float        apexS{ 0.55f };  // eased-progress fraction of the apex
		float        t{ 0.0f };       // elapsed seconds
		float        duration{ 0.5f };
		float        moveHeight{ 0.0f };  // ledge height driving arc/tier blending
		int          moveTier{ 0 };       // nearest tier (2 = high: camera levels)
		float        entrySpeed{ 0.0f };
		float        endBlend{ 1.0f };     // endpoint blend after conversion
		RE::NiPoint3 preConvertEnd{};
		float        watchdogTimer{ 0.0f };
		RE::NiPoint3 lastPos{};            // velocity-follow source
		bool         hasLastPos{ false };
		bool         earlySneakSent{ false };
		bool         startedInAir{ false };  // real jump anim is playing
		bool         jumpLandFired{ false }; // one-shot graph "jumpLand" per move
		float        jumpLandRetryT{ 0.0f };    // post-move nudge window (air starts)
		float        jumpLandRetryTick{ 0.0f };
		// Authored-curve mode: a high mantle rides the animation's own
		// exported root trajectory in lockstep (duration = clip length,
		// no easing, no align glide — the measured-vs-nominal scaling
		// absorbs start-distance and height variance). Pointer targets
		// AuthoredCurves' static storage, which can only reload from
		// Start, and Start never runs while a move is active.
		bool                authoredMode{ false };
		const AuthoredCurve* authored{ nullptr };
		// Curve-space origin: placed so the clip's authored wall distance
		// is preserved 1:1 (forward scaled by the SAME factor as the
		// rise). The player's real start offset decays toward it over the
		// clip's early crouch/grab window (approach blend).
		RE::NiPoint3        authoredAnchor{};
		float               authoredApproachT{ 0.0f };
		bool         correctionMode{ false };  // guard glide, not a real move
		bool         aligning{ false };        // pre-move walk-back glide
		float        alignT{ 0.0f };
		float        alignDuration{ 0.0f };
		RE::NiPoint3 alignFrom{};
		float        correctionLift{ 0.0f };   // parabolic lift so the glide arcs over lips
		bool         landingGuardValid{ false };
		RE::NiPoint3 landingGuardPos{};
		RE::NiPoint3 landingGuardDir{};
	};
}
