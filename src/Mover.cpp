#include "PCH.h"
#include "Mover.h"
#include "Detection.h"
#include "Raycast.h"
#include "Settings.h"
#include "DebugDraw.h"
#include "AnimHijack.h"
#include "CameraPivot.h"
#include "Input.h"

namespace
{
	using namespace F4Parkour;

	constexpr float kHavokScaleInv = 1.0f / 69.9915f;  // game units -> havok

	void AssignPoint3A(RE::NiPoint3A& a_dst, const RE::NiPoint3& a_src)
	{
		a_dst.x = a_src.x;
		a_dst.y = a_src.y;
		a_dst.z = a_src.z;
	}

	RE::bhkCharacterController* CharCtrl(RE::PlayerCharacter* a_player)
	{
		if (a_player && a_player->currentProcess && a_player->currentProcess->middleHigh) {
			return a_player->currentProcess->middleHigh->charController.get();
		}
		return nullptr;
	}

	void SetNoSim(RE::PlayerCharacter* a_player, bool a_on)
	{
		if (auto* cc = CharCtrl(a_player)) {
			constexpr auto kNoSim = static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNoSim);
			if (a_on) {
				cc->flags |= kNoSim;
			} else {
				cc->flags &= ~kNoSim;
			}
		}
	}

	// The bhkCharacterController owns its OWN capsule orientation:
	// pitchAngle/rollAngle, integrated by the engine whenever
	// calculatePitch is set (the swim/fall alignment). Feeding it an
	// airborne capsule plus path-follow velocity let that integrator
	// run wild — the literal "extreme rotation of the character
	// collision controller". Pin it flat every mover frame; restored
	// when the move ends.
	void PinControllerPitch(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			cc->pitchAngle = 0.0f;
			cc->rollAngle = 0.0f;
			cc->calculatePitchTimer = 0.0f;
			cc->calculatePitch = false;
		}
	}

	void RestoreControllerPitch(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			cc->calculatePitch = true;  // engine default: recompute from movement
			cc->pitchAngle = 0.0f;
			cc->rollAngle = 0.0f;
		}
	}

	// Zero the controller's fall bookkeeping. An air-start move hands the
	// engine a synthetic downward push so the jump graph gets its landing
	// and the pose resolves - but the controller's fallTime/fallStartHeight
	// still describe the pre-move jump, so the engine's ProcessDamageImpacts
	// (and impact-landing mods hooked there) treat the hand-off as a real
	// hard fall onto authored motion. Clearing them keeps the soft landing
	// (ground contact still resolves the graph) without the phantom impact.
	void ClearFallState(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			cc->fallTime = 0.0f;
			cc->fallStartHeight = a_player->data.location.z;
			cc->inAirPreMove = false;
		}
	}

	// Pin the controller's ENTIRE grounded contract for the whole move.
	// m_currentState alone proved insufficient (rotation persisted on
	// pinned grounded moves): the engine also reads surface support,
	// fall bookkeeping, and the capsule pitch above. A controller left
	// reading airborne/unsupported makes the ENGINE'S OWN behavior
	// graphs run the jump/fall state for the entire move — the
	// third-person jump pose and the first-person jump-loop camera
	// pitch-up that survived disabling every camera mod. Authored
	// motion is grounded motion as far as the rest of the game is
	// concerned. (SkyParkour never fights these signals because a real
	// full-body animation owns its graph; code-driven motion must pin
	// every signal the engine could read.)
	void PinGroundedState(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			cc->context.m_currentState = RE::hknpCharacterState::hknpCharacterStateType::kOnGround;
			cc->context.m_previousState = RE::hknpCharacterState::hknpCharacterStateType::kOnGround;
			cc->surfaceInfo.m_supportedState = RE::hknpCharacterSurfaceInfo::SupportedState::kSupported;
			cc->surfaceInfo.m_surfaceNormal = cc->up;
			cc->fallTime = 0.0f;
			cc->fallStartHeight = a_player->data.location.z;
			cc->inAirPreMove = false;
		}
		PinControllerPitch(a_player);
	}

	void SetVelocity(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_velGameUnits)
	{
		if (auto* cc = CharCtrl(a_player)) {
			RE::hkVector4f v{};
			v[0] = a_velGameUnits.x * kHavokScaleInv;
			v[1] = a_velGameUnits.y * kHavokScaleInv;
			v[2] = a_velGameUnits.z * kHavokScaleInv;
			v[3] = 0.0f;
			cc->SetLinearVelocityImpl(v);
		}
	}

	// Warp the Havok character controller to the same point as the
	// reference. THE critical sync: moving data.location alone leaves the
	// controller at the start position, and the moment simulation resumes
	// the engine snaps the player back to it ("pushed back to where I
	// started") or resolves the mismatch out of bounds (falling through
	// the map). forceWarp teleports instead of sweeping.
	//
	// SELF-CALIBRATING CENTER OFFSET: whether the applyCenterOffset flag
	// means "this position is the actor's feet" is undocumented, and a
	// wrong guess buries the capsule ~a half-height inside the floor —
	// invisible while kNoSim holds, then the resumed solver fails
	// DOWNWARD and the player falls through the map. On first use (the
	// player is standing at a known-good spot) we read the position back
	// through both conventions and keep whichever matches the feet.
	// -1 uncalibrated / 0 false / 1 true. MUST be calibrated while the
	// reference and controller are in sync — i.e. at move START while
	// standing. The old lazy calibration ran on the first warp, which in
	// the warp-once design is at move END, the exact moment the two are
	// intentionally desynced: it read garbage, picked the wrong setter
	// convention, and every subsequent warp misplaced the controller by
	// ~a half-height (the constant landing-guard fires in testing).
	int s_applyOffset = -1;

	void CalibrateControllerOffset(RE::PlayerCharacter* a_player)
	{
		if (s_applyOffset >= 0) return;
		auto* cc = CharCtrl(a_player);
		if (!cc) return;
		RE::hkVector4f withOffset{}, withoutOffset{};
		cc->GetPositionImpl(withOffset, true);
		cc->GetPositionImpl(withoutOffset, false);
		const float feetZ = a_player->data.location.z;
		const float zWith = withOffset[2] / kHavokScaleInv;
		const float zWithout = withoutOffset[2] / kHavokScaleInv;
		const float errWith = std::fabs(zWith - feetZ);
		const float errWithout = std::fabs(zWithout - feetZ);
		// Only accept a clean reading: one convention should match the
		// feet within a few units while standing. Anything else means we
		// sampled at a desynced moment — leave uncalibrated and try again
		// on the next move.
		if (std::min(errWith, errWithout) > 10.0f) {
			logger::warn(
				"[Mover] Offset calibration skipped (desynced sample): feetZ={:.1f} withOffsetZ={:.1f} withoutOffsetZ={:.1f}",
				feetZ, zWith, zWithout);
			return;
		}
		s_applyOffset = (errWith <= errWithout) ? 1 : 0;
		logger::info(
			"[Mover] Controller offset calibration: feetZ={:.1f} withOffsetZ={:.1f} withoutOffsetZ={:.1f} -> applyCenterOffset={}",
			feetZ, zWith, zWithout, s_applyOffset == 1);
	}

	void WarpController(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_posGameUnits)
	{
		if (auto* cc = CharCtrl(a_player)) {
			if (s_applyOffset < 0) {
				// Never calibrated cleanly this session: default to the
				// session-1-proven convention (true = position is feet).
				s_applyOffset = 1;
				logger::warn("[Mover] Warping without clean calibration - defaulting applyCenterOffset=true");
			}
			RE::hkVector4f p{};
			p[0] = a_posGameUnits.x * kHavokScaleInv;
			p[1] = a_posGameUnits.y * kHavokScaleInv;
			p[2] = a_posGameUnits.z * kHavokScaleInv;
			p[3] = 0.0f;
			cc->SetPositionImpl(p, s_applyOffset == 1, true);
			// COMMIT the warp. Without this the queued position never
			// reaches the live body before the next controller step: the
			// first simulated frame still used the pre-move position and
			// snapped the player backward into the obstacle one frame
			// after EVERY vault (log: finish -> guard 24ms later, always
			// "shoved backward"). The read-back "verify" below couldn't
			// see it — it reads the same cached value the setter wrote.
			cc->ApplyMoveImmediately();

			// SELF-VERIFYING: the getter's offset convention was proven by
			// the calibration above, but the SETTER was assumed symmetric —
			// and a mismatched setter buries the capsule a half-height deep
			// at every move end (ejected backwards = "failed vault", or
			// through the floor). Read the position back through the proven
			// getter; on disagreement flip the setter's flag, re-set, and
			// log the whole exchange.
			RE::hkVector4f rb{};
			cc->GetPositionImpl(rb, true);
			const float errZ = std::fabs(rb[2] - p[2]) / kHavokScaleInv;
			if (errZ > 20.0f) {
				const bool flipped = !(s_applyOffset == 1);
				cc->SetPositionImpl(p, flipped, true);
				RE::hkVector4f rb2{};
				cc->GetPositionImpl(rb2, true);
				const float errZ2 = std::fabs(rb2[2] - p[2]) / kHavokScaleInv;
				logger::warn(
					"[Mover] Warp verify: setter flag {} left {:.1f}u error; flipped -> {:.1f}u error",
					s_applyOffset == 1, errZ, errZ2);
				if (errZ2 < errZ) {
					s_applyOffset = flipped ? 1 : 0;  // the setter's real convention
					cc->ApplyMoveImmediately();
				} else {
					// Neither convention lands where the getter reads feet —
					// restore the original attempt and let the reference
					// position lead; better a snap than a burial.
					cc->SetPositionImpl(p, s_applyOffset == 1, true);
					cc->ApplyMoveImmediately();
				}
			}
		}
	}

	// Nearest tier index for a ledge height against the preset's tier
	// reference heights — this picks the per-tier OAR keyword.
	int NearestTier(const std::array<float, 3>& a_heights, float a_height)
	{
		int best = 0;
		float bestDist = std::fabs(a_height - a_heights[0]);
		for (int i = 1; i < 3; ++i) {
			const float d = std::fabs(a_height - a_heights[i]);
			if (d < bestDist) {
				bestDist = d;
				best = i;
			}
		}
		return best;
	}
}

namespace F4Parkour
{
	float Mover::ArcPeakX() const
	{
		// Argmax by sampling the tier-blended arc — cheap, robust against
		// user-edited shapes, and keeps the apex synchronized with the
		// curve editor (the apex is the conversion/commit point AND where
		// the lift amplitude is normalized).
		const bool vault = (kind == MoveKind::Vault);
		float bestX = 0.5f;
		float bestY = -1.0e9f;
		for (int i = 0; i <= 32; ++i) {
			const float x = static_cast<float>(i) / 32.0f;
			const float y = preset.ArcAt(vault, moveHeight, x);
			if (y > bestY) {
				bestY = y;
				bestX = x;
			}
		}
		return std::clamp(bestX, 0.15f, 0.9f);
	}

	bool Mover::Start(RE::PlayerCharacter* a_player, const LedgeCandidate& a_candidate, MoveKind a_kind, bool a_dryRun)
	{
		if (active || a_kind == MoveKind::None) return false;
		if (a_kind == MoveKind::Vault && !a_candidate.vaultEligible) return false;
		if (a_kind == MoveKind::Mantle && !a_candidate.mantleEligible) return false;

		// Snapshot the preset: menu edits must never touch a live move.
		// DRY-RUNS reuse the last snapshot for a short while instead of
		// re-copying the curve vectors 20x/sec - those copies were
		// constant heap churn through the replacement memory manager and
		// an unsynchronized read against the menu thread's edits. A real
		// activation always takes a fresh copy.
		{
			static std::chrono::steady_clock::time_point s_lastCopy{};
			const auto now = std::chrono::steady_clock::now();
			if (!a_dryRun || std::chrono::duration<float>(now - s_lastCopy).count() > 0.25f) {
				preset = Presets::GetSingleton()->Active();
				s_lastCopy = now;
			}
		}

		cand = a_candidate;
		kind = a_kind;
		endBlend = 1.0f;
		watchdogTimer = 0.0f;
		earlySneakSent = false;

		// Calibrate the controller-offset convention NOW, while the
		// reference and controller are guaranteed in sync (standing at
		// the move start, nothing written yet).
		CalibrateControllerOffset(a_player);

		// A move that starts airborne rides a REAL engine jump: its jump
		// animation is legitimately playing. Pinning the grounded state
		// on those moves froze the graph mid-jump (no landing ever
		// delivered) - the persistent "camera and character rotated up".
		// Air starts keep their air state and get a natural landing at
		// Finish instead.
		//
		// Classified by PHYSICAL reality, never the raw controller state:
		// m_currentState flickers kInAir while standing on slopes and
		// jagged bases, and candidate.fromAir only records which scan
		// mode happened to run. Both misclassified STANDING mantles as
		// air starts, which skipped the grounded pin and let the fall
		// graph run the whole move — the "extreme rotation". An air
		// start requires being off the ground AND either a genuinely
		// launched engine jump or no ground anywhere near the feet.
		{
			const bool stateAir = Detection::IsInAir(a_player);
			const bool groundNear = Detection::GroundWithin(a_player, 25.0f);
			const bool recentJump = Input::TimeSinceEngineJump() < 1.2f;
			startedInAir = stateAir && (recentJump || !groundNear);
			if (stateAir && !startedInAir && !a_dryRun) {
				logger::info(
					"[Mover] kInAir flicker overridden: ground under feet, no engine jump -> grounded start");
			}
		}

		startPos = { a_player->data.location.x, a_player->data.location.y, a_player->data.location.z };
		dir = cand.approachDir;

		// Standing too close under the lip is no longer a refusal: the
		// move begins with a short ALIGN glide that walks the capsule
		// back to a clean start distance — SkyParkour's authored start
		// positions (ledge minus a back offset), done smoothly instead
		// of by warp.
		aligning = false;
		if (a_kind == MoveKind::Mantle && !startedInAir) {
			const RE::NiPoint3& lip = cand.mantleLedge;
			const float proj = (lip.x - startPos.x) * dir.x + (lip.y - startPos.y) * dir.y;
			constexpr float kMinStartDist = 32.0f;
			if (proj < kMinStartDist) {
				RE::NiPoint3 ideal = {
					lip.x - dir.x * kMinStartDist,
					lip.y - dir.y * kMinStartDist,
					startPos.z
				};
				RE::NiPoint3 gStart = ideal;
				gStart.z += 40.0f;
				Raycast::RayHit g{};
				if (Raycast::CastDir(gStart, { 0.0f, 0.0f, -1.0f }, 80.0f, g)) {
					ideal.z = gStart.z - g.distance + 0.5f;
				}
				const float adx = ideal.x - startPos.x;
				const float ady = ideal.y - startPos.y;
				const float adz = ideal.z - startPos.z;
				const float alignDist = std::sqrt(adx * adx + ady * ady + adz * adz);
				if (alignDist > 2.0f) {
					alignFrom = startPos;
					startPos = ideal;
					aligning = true;
					alignT = 0.0f;
					alignDuration = std::clamp(alignDist / 120.0f, 0.08f, 0.3f);
				}
			}
		}

		const bool vault = (kind == MoveKind::Vault);
		const RE::NiPoint3& ledge = vault ? cand.vaultLedge : cand.mantleLedge;
		const float height = vault ? cand.vaultHeight : cand.mantleHeight;

		endPos = vault ? cand.vaultLanding : cand.mantleTarget;

		// Apex: clearance above the ledge, tightened when the on-top
		// headroom only fits a crouch (arcing the head into the ceiling
		// is exactly what the clearance must not do).
		float clearance = preset.apexClearance;
		if (!vault && cand.headroom == Headroom::CrouchOnly) {
			clearance = std::min(clearance, 3.0f);
		}
		apexZ = ledge.z + clearance;
		moveHeight = height;
		apexS = ArcPeakX();

		// Belt-and-braces apex headroom probe: if something hangs over
		// the arc's peak, flatten the arc under it rather than colliding.
		{
			RE::NiPoint3 apexProbe = SamplePath(apexS);
			apexProbe.z = apexZ + 2.0f;
			Raycast::RayHit overhead{};
			Raycast::CastDir(apexProbe, { 0.0f, 0.0f, 1.0f }, Detection::kCrouchHeight, overhead);
			if (overhead.hit && overhead.distance < Detection::kCrouchHeight) {
				const float reduce = Detection::kCrouchHeight - overhead.distance;
				apexZ = std::max(ledge.z + 2.0f, apexZ - reduce);
			}
		}

		// Duration: tier blend x sprint scale x global speed — then, at
		// speed, matched to the entry velocity so a sprint carries
		// through at roughly constant horizontal speed instead of
		// "stop, hop, resume" (Brink: duration from distance AND
		// velocity).
		entrySpeed = Detection::HorizontalSpeed(a_player);
		const bool fast = Detection::IsSprinting(a_player) || entrySpeed > 350.0f;
		duration = preset.DurationFor(vault, height);
		if (fast) {
			duration *= preset.sprintDurationScale;
		}
		duration = std::max(0.15f, duration / std::max(0.25f, preset.speedMult));
		// Speed matching, USER-CONTROLLED (Settings::vaultSpeedMatch).
		// The old always-on rule silently compressed fast entries to as
		// little as 55% of the tier time — "some vaults are a lot faster
		// than others" with no slider that could stop it. 0 = uniform
		// vaults (tier time regardless of entry speed), 1 = full
		// constant-ground-speed compression (Brink flow).
		const float match = Settings::GetSingleton()->vaultSpeedMatch;
		if (vault && entrySpeed > 250.0f && match > 0.01f) {
			const float dx = endPos.x - startPos.x;
			const float dy = endPos.y - startPos.y;
			const float flatLen = std::sqrt(dx * dx + dy * dy);
			const float speedMatched = flatLen / entrySpeed;
			// Keep flow without teleporting: never quicker than 0.35s or
			// 55% of the tier time, whichever is larger.
			const float floor_ = std::max(0.35f, duration * 0.55f);
			const float target = std::clamp(speedMatched, floor_, duration);
			duration += (target - duration) * match;
		}

		if (!ValidatePath(ledge)) {
			if (!a_dryRun) {
				DebugDraw::GetSingleton()->Event("activation aborted: path blocked");
				logger::info("[Mover] Activation aborted: path blocked ({} height={:.0f})",
					vault ? "vault" : "mantle", height);
			}
			return false;
		}

		// Dry-run stops here: everything above is scratch state that a
		// real Start reassigns; nothing observable has happened.
		if (a_dryRun) return true;

		t = 0.0f;
		phase = MovePhase::Rising;
		active = true;
		lastPos = startPos;
		hasLastPos = true;

		// Animation first, THEN kNoSim: SetupSpecialIdle refused every
		// attempt when called on an already-frozen controller in testing.
		const int tier = NearestTier(vault ? preset.vaultHeights : preset.mantleHeights, height);
		moveTier = tier;
		AnimHijack::GetSingleton()->OnMoveStart(a_player, kind, height, tier);

		SetNoSim(a_player, true);

		// Prove the warp path immediately, while reference and controller
		// are known to coincide: a no-op warp to the start position runs
		// the setter's read-back verification at the one moment a
		// disagreement can be detected (and the convention flipped) with
		// zero visible consequence.
		WarpController(a_player, startPos);

		if (Settings::GetSingleton()->debugEnabled) {
			std::vector<RE::NiPoint3> pathPts;
			for (int i = 0; i <= 16; ++i) {
				pathPts.push_back(SamplePath(static_cast<float>(i) / 16.0f));
			}
			DebugDraw::GetSingleton()->AddPath(pathPts);
			DebugDraw::GetSingleton()->Event(std::format(
				"{} started: h={:.0f} tier={} dur={:.2f}s speed={:.0f}{}",
				vault ? "vault" : "mantle",
				height, tier, duration, entrySpeed,
				cand.headroom == Headroom::CrouchOnly && !vault ? " (crouch-only)" : ""));
		}

		logger::info("[Mover] {} started: height={:.1f} tier={} duration={:.2f} entrySpeed={:.0f}",
			vault ? "vault" : "mantle", height, tier, duration, entrySpeed);
		return true;
	}

	// Current blended endpoint — retargeting mid-blend must start from
	// here, not from the raw preConvertEnd, or the path jumps.
	static RE::NiPoint3 BlendEnd(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_t)
	{
		return {
			a_from.x + (a_to.x - a_from.x) * a_t,
			a_from.y + (a_to.y - a_from.y) * a_t,
			a_from.z + (a_to.z - a_from.z) * a_t,
		};
	}

	RE::NiPoint3 Mover::SamplePath(float a_s) const
	{
		// Horizontal: straight line along the approach (Dying Light: the
		// player's forward vector, never the ledge normal).
		RE::NiPoint3 end = endPos;
		if (endBlend < 1.0f) {
			end.x = preConvertEnd.x + (endPos.x - preConvertEnd.x) * endBlend;
			end.y = preConvertEnd.y + (endPos.y - preConvertEnd.y) * endBlend;
			end.z = preConvertEnd.z + (endPos.z - preConvertEnd.z) * endBlend;
		}

		RE::NiPoint3 p;
		p.x = startPos.x + (end.x - startPos.x) * a_s;
		p.y = startPos.y + (end.y - startPos.y) * a_s;

		// Vertical: base lerp plus the lift profile, scaled so the peak
		// reaches apexZ. The lift is normalized to END AT ZERO (a curve
		// whose last point is above zero would otherwise leave the final
		// frame hovering, then pop down to the landing at Finish).
		const bool vaultArc = (kind == MoveKind::Vault);
		const float endLift = preset.ArcAt(vaultArc, moveHeight, 1.0f);
		auto lift = [&](float s) { return preset.ArcAt(vaultArc, moveHeight, s) - s * endLift; };
		const float baseZ = startPos.z + (end.z - startPos.z) * a_s;
		const float baseAtApex = startPos.z + (end.z - startPos.z) * apexS;
		const float peak = std::max(0.05f, lift(apexS));
		const float amplitude = std::max(0.0f, apexZ - baseAtApex) / peak;
		p.z = baseZ + lift(a_s) * amplitude;
		return p;
	}

	bool Mover::ValidatePath(const RE::NiPoint3& a_ledge) const
	{
		// MANTLES: detection is the SOLE authority. MantleScan already
		// proved everything about the target — the lip, the stand point,
		// the 3x3 headroom footprint, lip-sky, and behind-lip clearance.
		// This function's up-probe was a SECOND, CONFLICTING validator:
		// it walks the arc's RISING leg and casts straight up, but that
		// leg necessarily passes in FRONT of and BELOW any domed or
		// leaning mass, so the probe hits the obstacle's own overhang and
		// vetoed mantles detection had already blessed (the "mantle ledge
		// + stand markers show but no ring / no trigger" bug). A mantle
		// that detection accepted must never be second-guessed here — the
		// rising leg is under kNoSim and passes through the face by
		// design. Vaults keep the check: their path goes OVER an obstacle
		// and mid-air clearance is real geometry the vault scan's
		// endpoint tests do not fully cover.
		if (kind == MoveKind::Mantle) {
			return true;
		}

		// Sample the VAULT path and probe up (head) at each sample. Brink
		// shipped with endpoint checks only; this is stricter and any
		// solid hit over the crossing vetoes activation.
		constexpr int kSamples = 8;
		for (int i = 1; i < kSamples; ++i) {
			const float s = static_cast<float>(i) / kSamples;
			const RE::NiPoint3 p = SamplePath(s);

			Raycast::RayHit upHit{};
			RE::NiPoint3 upStart = p;
			upStart.z += 10.0f;
			Raycast::CastDir(upStart, { 0.0f, 0.0f, 1.0f }, Detection::kCrouchHeight, upHit);
			if (upHit.hit && upHit.distance < Detection::kCrouchHeight * 0.75f) {
				// Allow near the obstacle itself: the path hugs the ledge
				// at the apex, so only substantial blockage vetoes.
				if (std::fabs(p.z - a_ledge.z) > 20.0f) {
					return false;
				}
			}
		}
		return true;
	}

	void Mover::ApplyConversionToMantle()
	{
		if (kind != MoveKind::Vault || !cand.mantleEligible) return;

		preConvertEnd = endPos;
		endPos = cand.mantleTarget;
		endBlend = 0.0f;  // blend endpoints over the next ~0.1s
		kind = MoveKind::Mantle;

		// Re-anchor the arc to the mantle geometry: its lip may sit at a
		// different height than the vault crest, and the mantle lift curve
		// may peak elsewhere.
		float clearance = preset.apexClearance;
		if (cand.headroom == Headroom::CrouchOnly) {
			clearance = std::min(clearance, 3.0f);
		}
		apexZ = std::max(apexZ, cand.mantleLedge.z + clearance);
		moveHeight = cand.mantleHeight;
		apexS = ArcPeakX();

		const int tier = NearestTier(preset.mantleHeights, cand.mantleHeight);
		moveTier = tier;
		AnimHijack::GetSingleton()->OnMoveConverted(RE::PlayerCharacter::GetSingleton(), tier);
		DebugDraw::GetSingleton()->Event("converted to mantle (forward released before apex)");
		logger::info("[Mover] Vault converted to mantle before apex");
	}

	void Mover::OnCommitted(RE::PlayerCharacter* a_player)
	{
		// A test idle the engine refused at move start gets one retry at
		// the apex - the graph has settled by then.
		AnimHijack::GetSingleton()->RetryTestIdle(a_player);

		// Refine the landing NOW and glide the path into it, instead of
		// warping at Finish — the end-of-move corrections were a visible
		// teleport on nearly every move.
		RE::NiPoint3 adjusted = (endBlend < 1.0f) ? BlendEnd(preConvertEnd, endPos, endBlend) : endPos;
		const RE::NiPoint3 preAdjust = adjusted;

		if (kind == MoveKind::Vault) {
			RE::NiPoint3 probe = adjusted;
			probe.z += 40.0f;
			Raycast::RayHit back{};
			RE::NiPoint3 backDir{ -dir.x, -dir.y, 0.0f };
			const float clearNeeded = Detection::kCapsuleRadius + 4.0f;
			Raycast::CastDir(probe, backDir, clearNeeded, back);
			if (back.hit) {
				const float nudge = clearNeeded - back.distance;
				adjusted.x += dir.x * nudge;
				adjusted.y += dir.y * nudge;
			}
		}
		if (kind == MoveKind::Mantle) {
			// Back-half support probe: on rounded lips the point one
			// capsule radius BEHIND the landing can hang over air — the
			// engine then slides the capsule straight back off the edge
			// ("mantle up and fall right back down"). Shift the landing
			// forward until the trailing edge stands on the top too.
			for (int guard = 0; guard < 3; ++guard) {
				RE::NiPoint3 backPt = adjusted;
				backPt.x -= dir.x * Detection::kCapsuleRadius;
				backPt.y -= dir.y * Detection::kCapsuleRadius;
				backPt.z += 20.0f;
				Raycast::RayHit sup{};
				Raycast::CastDir(backPt, { 0.0f, 0.0f, -1.0f }, 50.0f, sup);
				if (sup.hit) break;
				adjusted.x += dir.x * 8.0f;
				adjusted.y += dir.y * 8.0f;
			}
		}
		{
			RE::NiPoint3 gStart = adjusted;
			gStart.z += 30.0f;
			Raycast::RayHit ground{};
			Raycast::CastDir(gStart, { 0.0f, 0.0f, -1.0f }, 300.0f, ground);
			if (ground.hit) {
				const float groundZ = gStart.z - ground.distance;
				// Both directions: below ground = depenetration shove on
				// hand-off; ABOVE ground = finishing in the air and then
				// falling to the surface ("mantle ends hovering").
				if (adjusted.z < groundZ + 0.5f || adjusted.z > groundZ + 4.0f) {
					adjusted.z = groundZ + 0.5f;
				}
			}
		}

		const float dx = adjusted.x - preAdjust.x;
		const float dy = adjusted.y - preAdjust.y;
		const float dz = adjusted.z - preAdjust.z;
		if (dx * dx + dy * dy + dz * dz > 1.0f) {
			preConvertEnd = preAdjust;
			endPos = adjusted;
			endBlend = 0.0f;  // glide into the corrected landing

			// The glide needs ~0.15s to land; give it that time even
			// when the commit fired late in the move.
			const float remaining = duration - t;
			if (remaining < 0.15f) {
				duration = t + 0.15f;
			}
		}

		// Crouch-only mantle: enter sneak NOW, while still rising toward
		// the top, so the capsule is already crouched when it arrives
		// under the low ceiling (waiting until Finish let the standing
		// capsule clip the overhang for a few frames).
		if (kind == MoveKind::Mantle && cand.headroom == Headroom::CrouchOnly &&
			Settings::GetSingleton()->sneakOnCrouchOnly && !earlySneakSent) {
			earlySneakSent = true;
			AnimHijack::GetSingleton()->RequestSneak(a_player);
		}
	}

	void Mover::Update(RE::PlayerCharacter* a_player, float a_dt)
	{
		if (!active) return;

		auto* settings = Settings::GetSingleton();
		a_dt *= settings->moverTimeScale;

		t += a_dt;
		if (endBlend < 1.0f) {
			endBlend = std::min(1.0f, endBlend + a_dt * 10.0f);
		}

		const float rawT = std::min(1.0f, duration > 0.0f ? t / duration : 1.0f);

		// Guard-correction glide: a bare smoothstep lerp — no arcs, no
		// conversion, no watchdog, no animation side effects.
		if (correctionMode) {
			const float cs = rawT * rawT * (3.0f - 2.0f * rawT);
			RE::NiPoint3 cpos{
				startPos.x + (endPos.x - startPos.x) * cs,
				startPos.y + (endPos.y - startPos.y) * cs,
				startPos.z + (endPos.z - startPos.z) * cs
					+ std::sin(3.14159265f * cs) * correctionLift,
			};
			AssignPoint3A(a_player->data.location, cpos);
			PinGroundedState(a_player);
			WarpController(a_player, cpos);
			a_player->Update3DPosition(Settings::GetSingleton()->warpSceneEachFrame);
			if (hasLastPos && a_dt > 1.0e-4f) {
				SetVelocity(a_player, {
					(cpos.x - lastPos.x) / a_dt,
					(cpos.y - lastPos.y) / a_dt,
					(cpos.z - lastPos.z) / a_dt });
			}
			lastPos = cpos;
			if (rawT >= 1.0f) {
				AssignPoint3A(a_player->data.location, endPos);
				WarpController(a_player, endPos);
				PinGroundedState(a_player);
				a_player->Update3DPosition(true);
				SetNoSim(a_player, false);
				RestoreControllerPitch(a_player);
				SetVelocity(a_player, { 0.0f, 0.0f, 0.0f });
				active = false;
				correctionMode = false;
				phase = MovePhase::Idle;
				kind = MoveKind::None;
				hasLastPos = false;
			}
			return;
		}

		// Align glide first: walk the capsule to the authored start,
		// then let the arc clock begin.
		if (aligning) {
			alignT += a_dt;
			const float ar = std::min(1.0f, alignDuration > 0.0f ? alignT / alignDuration : 1.0f);
			const float as = ar * ar * (3.0f - 2.0f * ar);
			RE::NiPoint3 apos{
				alignFrom.x + (startPos.x - alignFrom.x) * as,
				alignFrom.y + (startPos.y - alignFrom.y) * as,
				alignFrom.z + (startPos.z - alignFrom.z) * as,
			};
			AssignPoint3A(a_player->data.location, apos);
			PinGroundedState(a_player);
			WarpController(a_player, apos);
			a_player->Update3DPosition(Settings::GetSingleton()->warpSceneEachFrame);
			lastPos = apos;
			hasLastPos = true;
			if (ar >= 1.0f) {
				aligning = false;
			}
			t = 0.0f;  // the arc starts after alignment completes
			return;
		}

		const float s = preset.EasedProgress(kind == MoveKind::Vault, rawT);

		// Intent conversion window: vault → mantle while still rising, if
		// forward was released (the player wants on top, not over).
		if (phase == MovePhase::Rising) {
			// Commit no later than 70% progress: arcs that peak at the
			// very end (typical mantle shapes) put apexS at 0.9, so the
			// endpoint-refinement glide started with ~10% of the move
			// left, never finished, and Finish snapped the remainder —
			// the "land, then get teleported" jar.
			if (s >= std::min(apexS, 0.7f)) {
				phase = MovePhase::Committed;
				OnCommitted(a_player);
			} else if (kind == MoveKind::Vault && cand.mantleEligible && settings->requireForward &&
			           !Detection::IsForwardHeld()) {
				ApplyConversionToMantle();
			}
		}

		const RE::NiPoint3 pos = SamplePath(s);

		// In-flight watchdog. Strictly limited so it can NEVER produce
		// the "pushed back to where you started" failure:
		//   * probes only once the capsule is ABOVE the obstacle top —
		//     while rising, the forward probe would hit the obstacle's
		//     own face and false-trigger;
		//   * never cancels — its only remedy is settling on top as a
		//     mantle (Brink shipped with no mid-move collision at all).
		const float activeLedgeZ = (kind == MoveKind::Vault) ? cand.vaultLedge.z : cand.mantleLedge.z;
		watchdogTimer += a_dt;
		if (settings->watchdogEnabled && rawT < 0.9f && watchdogTimer >= 0.05f &&
			pos.z > activeLedgeZ + 5.0f &&
			kind == MoveKind::Vault && cand.mantleEligible) {
			watchdogTimer = 0.0f;
			const RE::NiPoint3 ahead = SamplePath(std::min(1.0f, s + 0.15f));
			RE::NiPoint3 delta{ ahead.x - pos.x, ahead.y - pos.y, ahead.z - pos.z };
			const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			if (len > 4.0f) {
				RE::NiPoint3 probeStart = pos;
				probeStart.z += 40.0f;  // capsule mid-height
				Raycast::RayHit block{};
				RE::NiPoint3 pdir{ delta.x / len, delta.y / len, delta.z / len };
				Raycast::CastDir(probeStart, pdir, len + Detection::kCapsuleRadius, block);
				if (block.hit && block.distance < Detection::kCapsuleRadius) {
					logger::warn("[Mover] Landing blocked mid-move - settling as mantle");
					ApplyConversionToMantle();
				}
			}
		}

		AssignPoint3A(a_player->data.location, pos);
		if (!startedInAir) {
			PinGroundedState(a_player);
		} else {
			// Air starts keep their honest air state, but the capsule
			// pitch integrator still gets pinned — it is pure rotation
			// garbage during authored motion either way.
			PinControllerPitch(a_player);
		}

		// High-tier moves level the camera to face straight forward
		// while the big animation plays (Dying Light does the same on
		// climbs). This is a CONVERGENT absolute lerp of the aim pitch
		// and heading — never an additive per-frame offset, which is
		// what made writing data.angle.x a disaster in round 1.
		if (moveTier == 2) {
			const float k = 1.0f - std::exp(-10.0f * a_dt);
			a_player->data.angle.x += (0.0f - a_player->data.angle.x) * k;
			const float targetYaw = std::atan2(dir.x, dir.y);
			float dYaw = targetYaw - a_player->data.angle.z;
			while (dYaw > 3.14159265f) dYaw -= 6.2831853f;
			while (dYaw < -3.14159265f) dYaw += 6.2831853f;
			a_player->data.angle.z += dYaw * k;

			// High-mantle camera director (first person): keep the rising
			// eye out of the ledge geometry it is climbing onto. Mantle
			// only (the tier-2 leveling above still applies to vaults too).
			if (kind == MoveKind::Mantle && settings->highMantleCameraDirector) {
				CameraPivot::Collision(a_player, settings->camCollisionSkin);
			}
		}

		// Carry the controller WITH the reference every frame — the
		// SkyParkour model: under kNoSim the capsule travels with the
		// move, so the finish hand-off has no distance left to contest.
		// Warping only at Finish parked the capsule at the START for the
		// whole move and handed the resuming simulation one giant
		// teleport to fight; it shoved the player backward on about half
		// of all vaults even with the warp committed. (The historical
		// fall-throughs blamed on per-frame warps were really the
		// uncalibrated center-offset burying the capsule; calibration now
		// happens at Start and every warp self-verifies.)
		WarpController(a_player, pos);

		// Camera dip: DISABLED for now per playtest feedback (the pivot
		// module stays in place for a future pass; nothing calls it).

		// Per-frame scene update uses the "moved" path by default (not the
		// teleport/warp path) — forcing warp 30-60x per mantle churned the
		// pre-cull/cull caches (BSPreCulledObjects render-thread crash).
		// The controller is warped separately above, so the visual still
		// follows; the flag reverts to the teleport path if the mesh trails.
		a_player->Update3DPosition(Settings::GetSingleton()->warpSceneEachFrame);

		// Velocity-follow: give the controller the PATH velocity instead
		// of pinning it to zero. The sprint/move state machinery keeps
		// seeing real speed, so a vault at sprint stays "moving" and the
		// exit hand-off has no dead frame ("stop, hop, resume" fix).
		if (hasLastPos && a_dt > 1.0e-4f) {
			RE::NiPoint3 vel{
				(pos.x - lastPos.x) / a_dt,
				(pos.y - lastPos.y) / a_dt,
				(pos.z - lastPos.z) / a_dt
			};
			// Grounded (pinned) moves feed FLAT velocity only: the
			// vertical component is exactly what the controller's fall
			// bookkeeping and pitch integrator key on, and the sprint
			// state machinery only needs horizontal speed.
			if (!startedInAir) {
				vel.z = 0.0f;
			}
			// A hitch frame turns the follow velocity into a multi-
			// thousand-unit impulse; clamp so the state machinery stays
			// fed without the controller ever seeing a fling.
			const float mag = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
			constexpr float kMaxFollow = 1200.0f;
			if (mag > kMaxFollow) {
				const float k = kMaxFollow / mag;
				vel.x *= k;
				vel.y *= k;
				vel.z *= k;
			}
			SetVelocity(a_player, vel);
		}
		lastPos = pos;

		if (rawT >= 1.0f) {
			Finish(a_player);
		}
	}

	void Mover::Finish(RE::PlayerCharacter* a_player)
	{
		auto* settings = Settings::GetSingleton();

		// Vault landings: verify the capsule is actually clear of the
		// obstacle's far face and nudge forward if not — warping into
		// overlap is what let the depenetration solver throw the player
		// backwards when simulation resumed.
		if (kind == MoveKind::Vault) {
			RE::NiPoint3 probe = endPos;
			probe.z += 40.0f;  // capsule mid-height
			Raycast::RayHit back{};
			RE::NiPoint3 backDir{ -dir.x, -dir.y, 0.0f };
			const float clearNeeded = Detection::kCapsuleRadius + 4.0f;
			Raycast::CastDir(probe, backDir, clearNeeded, back);
			if (back.hit) {
				const float nudge = clearNeeded - back.distance;
				endPos.x += dir.x * nudge;
				endPos.y += dir.y * nudge;
			}
		}

		// Ground-safety snap: the end point must sit ON verified ground
		// before simulation resumes. A hair below the surface reads as
		// penetration (solver can fail downward = through the map); no
		// ground at all means something went badly wrong — in that case
		// the start position is the only known-good spot.
		{
			RE::NiPoint3 gStart = endPos;
			gStart.z += 30.0f;
			Raycast::RayHit ground{};
			Raycast::CastDir(gStart, { 0.0f, 0.0f, -1.0f }, 300.0f, ground);
			if (ground.hit) {
				const float groundZ = gStart.z - ground.distance;
				// Both directions — hovering above the surface at finish
				// reads as "land in the air, then drop".
				if (endPos.z < groundZ + 0.5f || endPos.z > groundZ + 4.0f) {
					endPos.z = groundZ + 0.5f;
				}
			} else {
				logger::warn("[Mover] No ground under the landing - reverting to start position");
				endPos = startPos;
			}
		}

		AssignPoint3A(a_player->data.location, endPos);
		WarpController(a_player, endPos);
		if (!startedInAir) {
			PinGroundedState(a_player);
		}
		CameraPivot::Clear(a_player);
		a_player->Update3DPosition(true);

		SetNoSim(a_player, false);
		RestoreControllerPitch(a_player);

		// Air-start moves: the controller resumes IN AIR a hair above the
		// verified landing with a small downward push, so the engine
		// performs a natural micro-landing - the jump graph receives its
		// real landing and the jump pose (and its camera pitch) resolves
		// instead of sticking.
		if (startedInAir) {
			ClearFallState(a_player);
			SetVelocity(a_player, { 0.0f, 0.0f, -60.0f });
			logger::info("[Mover] Air-start move - resolving with natural landing");
		}

		if (kind == MoveKind::Vault) {
			// Momentum restore, direction blended toward current input /
			// look yaw (Brink playtest rule).
			float keep = entrySpeed * settings->momentumKeep;

			RE::NiPoint3 exitDir = dir;
			const RE::NiPoint3 lookDir = Detection::DirFlat(a_player);
			const float b = settings->exitDirBlend;
			exitDir.x = dir.x * (1.0f - b) + lookDir.x * b;
			exitDir.y = dir.y * (1.0f - b) + lookDir.y * b;
			const float l = std::sqrt(exitDir.x * exitDir.x + exitDir.y * exitDir.y);
			if (l > 0.01f) {
				exitDir.x /= l;
				exitDir.y /= l;
			}

			// Exit-time drop trace (Brink): probe the ground AHEAD along
			// the exit direction — where the momentum would carry the
			// player — and clear it when the world falls away.
			if (keep > 1.0f) {
				RE::NiPoint3 aheadStart = {
					endPos.x + exitDir.x * 50.0f,
					endPos.y + exitDir.y * 50.0f,
					endPos.z + 10.0f
				};
				Raycast::RayHit dropHit{};
				Raycast::CastDir(aheadStart, { 0.0f, 0.0f, -1.0f }, settings->momentumDropCutoff + 10.0f, dropHit);
				if (!dropHit.hit) {
					keep = 0.0f;
					DebugDraw::GetSingleton()->Event("momentum cleared: high drop past landing");
				}
			}

			SetVelocity(a_player, { exitDir.x * keep, exitDir.y * keep, 0.0f });
		} else {
			SetVelocity(a_player, { 0.0f, 0.0f, 0.0f });
			if (cand.headroom == Headroom::CrouchOnly && settings->sneakOnCrouchOnly && !earlySneakSent) {
				AnimHijack::GetSingleton()->RequestSneak(a_player);
			}
		}

		AnimHijack::GetSingleton()->OnMoveEnd(a_player, kind);
		DebugDraw::GetSingleton()->ClearPath();

		logger::info("[Mover] {} finished", kind == MoveKind::Vault ? "vault" : "mantle");

		landingGuardPos = endPos;
		landingGuardDir = dir;
		landingGuardValid = true;

		active = false;
		phase = MovePhase::Idle;
		kind = MoveKind::None;
		hasLastPos = false;
	}

	void Mover::Cancel(RE::PlayerCharacter* a_player)
	{
		if (!active) return;

		if (phase == MovePhase::Committed && cand.mantleEligible) {
			// Past the apex: settle on top of the ledge rather than
			// teleporting backwards.
			AssignPoint3A(a_player->data.location, cand.mantleTarget);
			WarpController(a_player, cand.mantleTarget);
		} else {
			AssignPoint3A(a_player->data.location, startPos);
			WarpController(a_player, startPos);
		}
		CameraPivot::Clear(a_player);
		a_player->Update3DPosition(true);
		SetNoSim(a_player, false);
		RestoreControllerPitch(a_player);
		SetVelocity(a_player, { 0.0f, 0.0f, 0.0f });

		AnimHijack::GetSingleton()->OnMoveEnd(a_player, kind);
		DebugDraw::GetSingleton()->ClearPath();

		logger::warn("[Mover] Move cancelled");

		active = false;
		correctionMode = false;
		phase = MovePhase::Idle;
		kind = MoveKind::None;
		hasLastPos = false;
	}

	void Mover::HardReset(RE::PlayerCharacter* a_player)
	{
		if (a_player) {
			// A load can arrive mid-move (pause menu freezes the mover but
			// keeps it active). Never leave the controller wedged. The
			// camera pivot node is rebuilt with the FP skeleton on load;
			// clearing is a safe no-op when it does not exist.
			SetNoSim(a_player, false);
			RestoreControllerPitch(a_player);
			SetVelocity(a_player, { 0.0f, 0.0f, 0.0f });
			CameraPivot::Clear(a_player);
		}
		active = false;
		correctionMode = false;
		aligning = false;
		phase = MovePhase::Idle;
		kind = MoveKind::None;
		hasLastPos = false;
		DebugDraw::GetSingleton()->ClearPath();
	}

	void Mover::StartCorrection(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_target, float a_duration)
	{
		if (active || !a_player) return;
		correctionMode = true;
		active = true;
		phase = MovePhase::Committed;
		kind = MoveKind::Mantle;  // benign curve source; correction bypasses SamplePath
		startPos = { a_player->data.location.x, a_player->data.location.y, a_player->data.location.z };
		endPos = a_target;
		endBlend = 1.0f;
		t = 0.0f;
		duration = std::max(0.1f, a_duration);
		startedInAir = false;
		earlySneakSent = true;   // never sneak from a correction
		hasLastPos = true;
		lastPos = startPos;
		// Arc over anything between here and the target (a straight-line
		// glide back to a far-side landing passes THROUGH the obstacle).
		{
			const float ddx = endPos.x - startPos.x;
			const float ddy = endPos.y - startPos.y;
			const float flat = std::sqrt(ddx * ddx + ddy * ddy);
			correctionLift = std::clamp(flat * 0.6f, 0.0f, 40.0f);
		}
		SetNoSim(a_player, true);
	}

	void Mover::RescueTo(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_pos)
	{
		if (!a_player) return;
		AssignPoint3A(a_player->data.location, a_pos);
		WarpController(a_player, a_pos);
		a_player->Update3DPosition(true);
		SetVelocity(a_player, { 0.0f, 0.0f, 0.0f });
	}
}
