#pragma once

#include "ParkourTypes.h"

namespace F4Parkour
{
	// ============================================================
	// Orchestrator: runs detection at the configured interval while
	// idle, drives the Mover while a move is active, owns the
	// contextual-jump decision, jump assist consumption, and the
	// auto-parkour / auto-step-up triggers.
	//
	// Driven from the PlayerCharacter::UpdateAnimation vfunc hook
	// (main thread, every frame).
	// ============================================================
	class ParkourManager
	{
	public:
		static ParkourManager* GetSingleton()
		{
			static ParkourManager singleton;
			return &singleton;
		}

		void Update(RE::PlayerCharacter* a_player, float a_dt);
		void OnGameLoaded();

		// ---- input hook interface (synchronous, cheap) ----
		// Pre-evaluated each detection tick: would a jump press right now
		// become a parkour move?
		bool JumpWouldParkour() const { return jumpWouldParkour.load(std::memory_order_relaxed); }
		// The hook consumed a press; activate on the next Update. The
		// consumed press is refunded as a synthetic jump if activation
		// then fails (Input::ForwardJumpTap).
		void RequestActivation() { pendingRequest.store(kRequestFromJump, std::memory_order_release); }

		// Menu test buttons (render thread — a single atomic carries both
		// the request and the kind, so the main thread can never observe
		// a request without its kind).
		void ForceActivate(MoveKind a_kind)
		{
			pendingRequest.store(a_kind == MoveKind::Vault ? kRequestForceVault : kRequestForceMantle,
				std::memory_order_release);
		}

		const LedgeCandidate& Candidate() const { return candidate; }

	private:
		ParkourManager() = default;

		static constexpr int kRequestNone = 0;
		static constexpr int kRequestFromJump = 1;    // refund on failure
		static constexpr int kRequestForceVault = 2;
		static constexpr int kRequestForceMantle = 3;

		bool PreconditionsPass(RE::PlayerCharacter* a_player) const;
		bool ContextualIntent(RE::PlayerCharacter* a_player) const;
		MoveKind DecideKind(RE::PlayerCharacter* a_player) const;
		bool TryActivate(RE::PlayerCharacter* a_player, MoveKind a_forced);
		void PublishDebugState(RE::PlayerCharacter* a_player, float a_dt);

		LedgeCandidate candidate{};
		float detectionTimer{ 0.0f };
		float candidateAge{ 0.0f };  // seconds since the last VALID scan
		// Post-move landing guard (see Mover::ConsumeLandingGuard)
		bool  guardActive{ false };
		float guardTimer{ 0.0f };
		RE::NiPoint3 guardPos{};
		RE::NiPoint3 guardDir{};
		std::atomic<bool> jumpWouldParkour{ false };
		std::atomic<int>  pendingRequest{ kRequestNone };
		float debugTextTimer{ 0.0f };
	};

	// Install the per-frame hook (PlayerCharacter::UpdateAnimation).
	void InstallFrameHook();
}
