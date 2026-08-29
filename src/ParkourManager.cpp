#include "PCH.h"
#include "ParkourManager.h"
#include "Curves.h"
#include "Detection.h"
#include "Mover.h"
#include "Input.h"
#include "Settings.h"
#include "DebugDraw.h"

namespace
{
	using namespace F4Parkour;

	bool CameraAllowed()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->currentState) return false;
		const auto id = camera->currentState->id.get();
		if (id == RE::CameraStates::kFirstPerson || id == RE::CameraStates::kIronSights) {
			return true;
		}
		if (id == RE::CameraStates::k3rdPerson) {
			return Settings::GetSingleton()->allowThirdPerson;
		}
		return false;
	}

	bool InMenu()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->menuMode != 0;
	}
}

namespace F4Parkour
{
	bool ParkourManager::PreconditionsPass(RE::PlayerCharacter* a_player) const
	{
		auto* settings = Settings::GetSingleton();
		if (!settings->enabled) return false;
		if (!CameraAllowed()) return false;
		if (InMenu()) return false;
		if (a_player->IsDead(true)) return false;
		if (Detection::IsInPowerArmor(a_player)) return false;
		// Aiming: blocked unless vault-while-aiming is on, in which case
		// detection runs and ContextualIntent scopes it to vaults only.
		if (Detection::IsSighted(a_player) && !settings->allowVaultWhileAiming) return false;
		return true;
	}

	bool ParkourManager::ContextualIntent(RE::PlayerCharacter* a_player) const
	{
		auto* settings = Settings::GetSingleton();
		// Forward input gates VAULT intent only. The old waiver list
		// (mantle-only candidates, sub-60-speed standstill) kept missing
		// cases — releasing W a beat before pressing jump left speed
		// above the standstill threshold and the press fell through to a
		// vanilla jump ("standing mantle randomly refuses"). DecideKind
		// already picks Mantle exactly when momentum intent is absent,
		// so that decision IS the gate.
		if (settings->requireForward && !Detection::IsForwardHeld() &&
			DecideKind(a_player) == MoveKind::Vault) return false;

		// While aiming, only VAULTS are allowed (the toggle that let us get
		// this far is vault-scoped) — never break ADS to climb a ledge.
		if (Detection::IsSighted(a_player) && DecideKind(a_player) != MoveKind::Vault) {
			return false;
		}

		// Look cone: the candidate must be roughly where the player faces.
		if (candidate.IsValid()) {
			const RE::NiPoint3 dir = Detection::DirFlat(a_player);
			const RE::NiPoint3& ledge = candidate.PrimaryLedge();
			RE::NiPoint3 to{
				ledge.x - a_player->data.location.x,
				ledge.y - a_player->data.location.y,
				0.0f
			};
			const float len = std::sqrt(to.x * to.x + to.y * to.y);
			if (len > 1.0f) {
				to.x /= len;
				to.y /= len;
				const float dot = dir.x * to.x + dir.y * to.y;
				if (dot < std::cos(settings->lookConeDeg * 3.14159265f / 180.0f)) {
					return false;
				}
			}
		}
		return true;
	}

	MoveKind ParkourManager::DecideKind(RE::PlayerCharacter* a_player) const
	{
		if (!candidate.IsValid()) return MoveKind::None;

		auto* settings = Settings::GetSingleton();

		// Geometry vetoes first.
		if (!candidate.mantleEligible) return MoveKind::Vault;
		if (!candidate.vaultEligible) return MoveKind::Mantle;

		// Hold-to-mantle option: a held jump commits to on-top.
		if (settings->holdToMantle && Input::JumpHeld()) return MoveKind::Mantle;

		// Intent: moving with speed means over, otherwise on top.
		if (Detection::IsSprinting(a_player) || Detection::IsForwardHeld()) {
			return MoveKind::Vault;
		}
		return MoveKind::Mantle;
	}

	bool ParkourManager::TryActivate(RE::PlayerCharacter* a_player, MoveKind a_forced)
	{
		const MoveKind kind = (a_forced != MoveKind::None) ? a_forced : DecideKind(a_player);
		if (kind == MoveKind::None) return false;

		// Final ADS scope, whichever path reached here: aiming allows
		// VAULTS only. A mantle while sighted never fires.
		if (kind != MoveKind::Vault && Detection::IsSighted(a_player)) return false;

		if (Mover::GetSingleton()->Start(a_player, candidate, kind)) {
			Input::SetSuppressed(true);
			candidate.Reset();
			jumpWouldParkour.store(false, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	void ParkourManager::Update(RE::PlayerCharacter* a_player, float a_dt)
	{
		auto* settings = Settings::GetSingleton();
		auto* mover = Mover::GetSingleton();

		DebugDraw::GetSingleton()->Tick(a_dt);
		Input::Update(a_dt);

		// Idle-time mover work (post-move jumpLand retry) — must run when
		// NO move is active; Mover::Update is gated behind IsActive below.
		if (!mover->IsActive() && !InMenu()) {
			mover->PostMoveTick(a_player, a_dt);
		}

		// Move in progress: only the mover runs (freeze detection).
		if (mover->IsActive()) {
			// A request queued during the move (menu test click, or a
			// press racing the activation) must not fire a surprise move
			// the frame this one ends.
			pendingRequest.store(kRequestNone, std::memory_order_relaxed);
			jumpWouldParkour.store(false, std::memory_order_relaxed);
			DebugDraw::GetSingleton()->ClearIndicator();

			// Collapsed preconditions end the move: never keep steering a
			// corpse (or a scripted PA entry) along the arc.
			if (a_player->IsDead(true)) {
				mover->Cancel(a_player);
			} else if (!InMenu()) {
				mover->Update(a_player, a_dt);
			}

			if (mover->IsActive()) {
				// Control handback (Dying Light): input returns before the
				// move visually ends.
				const float liveFrom = 1.0f - settings->controlHandback;
				Input::SetSuppressed(mover->Progress() < liveFrom);
			} else {
				Input::SetSuppressed(false);
				if (mover->ConsumeLandingGuard(guardPos, guardDir)) {
					guardActive = true;
					guardTimer = 0.45f;
				}
			}
			PublishDebugState(a_player, a_dt);
			return;
		}

		// Landing guard: for a short window after every completed move,
		// a shove BACKWARD past the landing or a drop BELOW it means the
		// physics hand-off failed — snap to the intended landing spot
		// (never rubber-band the player back to where they came from).
		if (guardActive) {
			guardTimer -= a_dt;
			const RE::NiPoint3 pos{ a_player->data.location.x, a_player->data.location.y, a_player->data.location.z };
			const float alongDir = (pos.x - guardPos.x) * guardDir.x + (pos.y - guardPos.y) * guardDir.y;

			// ORDER MATTERS: clean forward progress disarms FIRST. With
			// restored sprint momentum the player can be 100+ units past
			// the landing (and, on a downhill, far below it) within a few
			// frames — that is success, not a failed hand-off, and gliding
			// them back dragged players INTO the obstacle they had just
			// cleared.
			if (alongDir > 40.0f) {
				guardActive = false;
			} else if (guardTimer <= 0.0f) {
				guardActive = false;
			} else {
				const bool shovedBack = alongDir < -25.0f;
				// "Fell below" only counts while horizontally AT the
				// landing — a hand-off failure drops you in place; a
				// hillside drops you while you travel.
				const bool fellBelow = pos.z < guardPos.z - 60.0f && std::fabs(alongDir) < 40.0f;
				if (shovedBack || fellBelow) {
					logger::warn("[Manager] Landing guard fired ({}): gliding to the intended landing",
						shovedBack ? "shoved backward" : "fell below landing");
					DebugDraw::GetSingleton()->Event(std::format("landing guard: {} - gliding to landing",
						shovedBack ? "shoved backward" : "fell below"));
					mover->StartCorrection(a_player, guardPos, 0.2f);
					guardActive = false;
				}
			}
		}

		if (!PreconditionsPass(a_player)) {
			candidate.Reset();
			jumpWouldParkour.store(false, std::memory_order_relaxed);
			DebugDraw::GetSingleton()->ClearIndicator();
			// A jump press consumed by the hook just before preconditions
			// collapsed still deserves its jump.
			if (pendingRequest.exchange(kRequestNone, std::memory_order_acquire) == kRequestFromJump) {
				Input::ForwardJumpTap();
			}
			PublishDebugState(a_player, a_dt);
			return;
		}

		// Detection at the configured cadence.
		detectionTimer += a_dt;
		if (detectionTimer >= settings->detectionInterval && !settings->freezeDetection) {
			detectionTimer = 0.0f;

			// Physical ground under the feet counts as grounded even when
			// the controller state flickers kInAir (slopes, gravel, the
			// jagged bases of boulders) — the same flicker that used to
			// misclassify air starts also starved the grounded scan here.
			const bool grounded = Detection::IsOnGround(a_player) ||
				Input::TimeSinceGrounded() <= settings->coyoteWindow ||
				Detection::GroundWithin(a_player, 25.0f);
			// Just-jumped carve-out: once a real engine jump has launched,
			// arm the air scan even while GroundWithin(25) is briefly still
			// true — an ascending jump clears 25u fast and this is the
			// window where a mid-air grab should start looking early.
			const bool justJumped = Input::TimeSinceEngineJump() < 0.6f && Detection::IsInAir(a_player);
			const bool airborne = (!grounded || justJumped) && Detection::IsInAir(a_player);

			// Scan UNCONDITIONALLY while grounded (SkyParkour's model).
			// Every intent gate ever placed above the scan produced the
			// same bug: some downstream consumer (standstill mantles,
			// the mantle-only waiver) silently starved of candidates —
			// and invisibly so in debug sessions, because debug forced
			// the scan. Intent filtering belongs at ACTIVATION time
			// (ContextualIntent), never above the scan.
			LedgeCandidate fresh{};
			bool scanned = false;
			if (grounded) {
				fresh = Detection::Scan(a_player, false);
				scanned = true;
			} else if (airborne && settings->allowInAir) {
				fresh = Detection::Scan(a_player, true);
				scanned = true;
			}

			// Candidate persistence: on jagged meshes (boulders, logs) the
			// scan flickers valid/invalid tick to tick as rays graze
			// different lips. A jump landing on an invalid tick made
			// visible candidates randomly dead. A fresh valid scan always
			// replaces; an invalid one only clears after a short grace.
			if (scanned && fresh.IsValid()) {
				candidate = fresh;
				candidateAge = 0.0f;
			} else {
				candidateAge += settings->detectionInterval;
				// Longer grace in the air: a mid-air jump press has to find a
				// candidate that may have flickered off for a tick, and the
				// player crosses the scan volume fast while falling.
				const float grace = airborne ? 0.5f : 0.35f;
				if (candidateAge > grace) {
					candidate.Reset();
				}
			}

			// Pre-evaluate the FULL jump decision for the input hook —
			// including a DRY-RUN of activation (the same path validation
			// a real press runs). The stored flag, the indicator, and the
			// space bar can no longer disagree: a ring on screen IS the
			// promise that the press converts.
			bool would = candidate.IsValid() && ContextualIntent(a_player);
			if (would) {
				const MoveKind kindWould = DecideKind(a_player);
				would = kindWould != MoveKind::None &&
					mover->Start(a_player, candidate, kindWould, true);
			}
			jumpWouldParkour.store(would, std::memory_order_relaxed);

			// SkyParkour-style availability cue: ring the ledge the jump
			// key would take, for everything above the LOW height (knee
			// steps need no telegraph). Gated on the SAME full predicate
			// as the jump key (intent + activation dry-run) — a visible
			// ring that produced a vanilla hop was worse than no ring.
			if (settings->indicatorEnabled && would) {
				const auto& ipreset = Presets::GetSingleton()->Active();
				const MoveKind wouldKind = DecideKind(a_player);
				if (wouldKind == MoveKind::Vault &&
					candidate.vaultHeight > ipreset.vaultHeights[0] + 1.0f) {
					DebugDraw::GetSingleton()->SetIndicator(candidate.vaultLedge, 0xFF46B9FF);
				} else if (wouldKind == MoveKind::Mantle &&
					candidate.mantleHeight > ipreset.mantleHeights[0] + 1.0f) {
					DebugDraw::GetSingleton()->SetIndicator(candidate.mantleLedge, 0xFF82E65A);
				} else {
					DebugDraw::GetSingleton()->ClearIndicator();
				}
			} else {
				DebugDraw::GetSingleton()->ClearIndicator();
			}
		}

		// Activation requested by the jump hook or a menu test button.
		const int request = pendingRequest.exchange(kRequestNone, std::memory_order_acquire);
		if (request != kRequestNone) {
			const MoveKind forced =
				request == kRequestForceVault ? MoveKind::Vault :
				request == kRequestForceMantle ? MoveKind::Mantle : MoveKind::None;
			const bool started = TryActivate(a_player, forced);
			if (!started && request == kRequestFromJump) {
				// The hook ate a real press and no move came of it (path
				// blocked between ticks). Refund the jump — and drop the
				// candidate so the ring stops promising a move that just
				// proved impossible; the next scan re-earns it.
				Input::ForwardJumpTap();
				candidate.Reset();
				jumpWouldParkour.store(false, std::memory_order_relaxed);
				DebugDraw::GetSingleton()->ClearIndicator();
			}
			PublishDebugState(a_player, a_dt);
			return;
		}

		// Buffered jump press (jump assist). Consumed ONLY for airborne
		// candidates: the grounded case is served synchronously by the
		// hook, and a buffered grounded activation would double-act a
		// press whose engine jump already fired.
		if (candidate.IsValid() && candidate.fromAir && ContextualIntent(a_player)) {
			if (Input::ConsumeBufferedPress()) {
				TryActivate(a_player, MoveKind::None);
				PublishDebugState(a_player, a_dt);
				return;
			}
				// Auto air-grab (default OFF): once falling past the arm
				// delay, grab a valid air candidate with no input - the
				// Dying Light "reach and it catches" assist. SkyParkour does
				// this for its Grab type at fallTime > 0.5.
				if (settings->autoAirGrab &&
					Input::TimeSinceGrounded() > settings->airAutoGrabDelay) {
					TryActivate(a_player, MoveKind::None);
					PublishDebugState(a_player, a_dt);
					return;
				}
		}

		// Auto triggers (both default OFF). Auto step-up covers only
		// knee-height obstacles — invisible below the FOV when close.
		if (candidate.vaultEligible && Detection::IsSprinting(a_player) && Detection::IsForwardHeld()) {
			const float dist = [&] {
				const float dx = candidate.vaultLedge.x - a_player->data.location.x;
				const float dy = candidate.vaultLedge.y - a_player->data.location.y;
				return std::sqrt(dx * dx + dy * dy);
			}();
			const auto& preset = Presets::GetSingleton()->Active();
			const bool stepHeight = candidate.vaultHeight <= preset.vaultHeights[0];
			if ((settings->autoParkourSprint || (settings->autoStepUp && stepHeight)) &&
				dist <= settings->autoEngageDistance) {
				TryActivate(a_player, MoveKind::Vault);
			}
		}

		PublishDebugState(a_player, a_dt);
	}

	void ParkourManager::PublishDebugState(RE::PlayerCharacter* a_player, float a_dt)
	{
		auto* settings = Settings::GetSingleton();
		if (!settings->debugEnabled) return;

		// The readout feeds a ~30fps ImGui panel; rebuilding the string
		// every frame is pure waste.
		debugTextTimer += a_dt;
		if (debugTextTimer < 0.15f) return;
		debugTextTimer = 0.0f;

		auto* mover = Mover::GetSingleton();

		std::string text;
		if (mover->IsActive()) {
			text = std::format(
				"MOVE  kind={}  phase={}  t={:.0f}%\n",
				mover->ActiveKind() == MoveKind::Vault ? "vault" : "mantle",
				mover->Phase() == MovePhase::Rising ? "rising" : "committed",
				mover->Progress() * 100.0f);
		} else if (candidate.IsValid()) {
			text = std::format(
				"CANDIDATE  vault={} (h={:.0f} depth={:.0f})  mantle={} (h={:.0f} depth={:.0f} headroom={})\n",
				candidate.vaultEligible, candidate.vaultHeight, candidate.vaultTopDepth,
				candidate.mantleEligible, candidate.mantleHeight, candidate.mantleDepth,
				candidate.headroom == Headroom::Stand ? "stand" :
					candidate.headroom == Headroom::CrouchOnly ? "crouch" : "-");
		} else {
			text = "no candidate\n";
		}

		text += std::format(
			"grounded={}  air={}  sprint={}  fwd={}  sighted={}  jumpCtx={}",
			Detection::IsOnGround(a_player), Detection::IsInAir(a_player),
			Detection::IsSprinting(a_player), Detection::IsForwardHeld(),
			Detection::IsSighted(a_player), jumpWouldParkour.load(std::memory_order_relaxed));

		DebugDraw::GetSingleton()->SetStateText(std::move(text));
	}

	void ParkourManager::OnGameLoaded()
	{
		Mover::GetSingleton()->HardReset(RE::PlayerCharacter::GetSingleton());
		Input::SetSuppressed(false);
		candidate.Reset();
		detectionTimer = 0.0f;
		jumpWouldParkour.store(false, std::memory_order_relaxed);
		pendingRequest.store(kRequestNone, std::memory_order_relaxed);
		guardActive = false;
		guardTimer = 0.0f;
		DebugDraw::GetSingleton()->ClearIndicator();
	}

	// ============================================================
	// Per-frame hook: PlayerCharacter::UpdateAnimation vfunc 0x9F —
	// the FPGO-proven runtime-agnostic main-thread frame driver.
	// ============================================================
	namespace
	{
		struct PlayerUpdateAnimationHook
		{
			static void Thunk(RE::PlayerCharacter* a_player, float a_delta)
			{
				Original(a_player, a_delta);

				static thread_local bool updating = false;
				if (updating) return;
				updating = true;

				static auto lastClock = std::chrono::high_resolution_clock::now();
				const auto now = std::chrono::high_resolution_clock::now();
				float realDelta = std::chrono::duration<float>(now - lastClock).count();
				lastClock = now;
				realDelta = std::clamp(realDelta, 0.0001f, 0.1f);

				float timeMult = RE::BSTimer::QGlobalTimeMultiplier();
				if (timeMult <= 0.0f || timeMult > 100.0f) timeMult = 1.0f;

				ParkourManager::GetSingleton()->Update(a_player, realDelta * timeMult);

				updating = false;
			}

			static void Install()
			{
				REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE::PlayerCharacter[0] };
				Original = vtable.write_vfunc(0x9F, Thunk);
			}

			inline static REL::Relocation<decltype(Thunk)> Original;
		};
	}

	void InstallFrameHook()
	{
		PlayerUpdateAnimationHook::Install();
		logger::info("[F4Parkour] Frame hook installed: PlayerCharacter::UpdateAnimation vfunc 0x9F");
	}
}
