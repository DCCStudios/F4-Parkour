#include "PCH.h"
#include "Detection.h"
#include "Raycast.h"
#include "Settings.h"
#include "DebugDraw.h"

// ============================================================
// Detection: the runtime geometry scan.
//
// Structure ported from SkyParkourNG (vault down-sweep, mantle
// air-step + down-ray scan, obstruction-behind-lip, railing side
// rays) with the F4Parkour additions: measured top depth (thin-top
// rule), far-side back clearance (vault landing room), stand-or-
// crouch headroom, approach-angle clamp, and the airborne variant.
// All rays go through Raycast::Cast (solid-layer allow-list).
//
// NOTE on the mantle scan shape: SkyParkourNG's forward rays do NOT
// look for the wall — they verify CLEAR AIR while stepping forward
// above the obstacle, then cast DOWN to find the ledge surface. The
// forward clearance is what guarantees the down-ray starts outside
// geometry.
// ============================================================
namespace
{
	using namespace F4Parkour;

	constexpr float kVaultLength = 100.0f;       // forward reach of the vault sweep
	constexpr float kSweepStep = 5.0f;           // down-sweep spacing
	constexpr int   kSweepIterations = 40;       // 200 units of forward coverage - matches the mantle reach so the indicator never promises a vault the sweep can't deliver
	constexpr int   kMantleFwdIterations = 40;   // 200 units of mantle coverage - a leaning rock at max height holds its crest far forward
	constexpr float kMinLedgeFlatness = 0.5f;    // normal.z for a standable top

	RE::NiPoint3 Add(const RE::NiPoint3& a, const RE::NiPoint3& b)
	{
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	}

	RE::NiPoint3 Mul(const RE::NiPoint3& a, float s)
	{
		return { a.x * s, a.y * s, a.z * s };
	}

	float LenFlat(const RE::NiPoint3& a)
	{
		return std::sqrt(a.x * a.x + a.y * a.y);
	}

	// Record a ray for the debug overlay. The end point shown is the hit
	// point when the ray hit, else the queried end.
	void DbgRay(const RE::NiPoint3& a_start, const RE::NiPoint3& a_dir, float a_len,
		const Raycast::RayHit& a_hit, bool a_passed, const char* a_label)
	{
		if (!Settings::GetSingleton()->debugEnabled) return;
		const RE::NiPoint3 end = a_hit.hit
			? a_hit.point
			: Add(a_start, Mul(a_dir, a_len));
		DebugDraw::GetSingleton()->AddRay(a_start, end, a_hit.hit, a_passed, a_label);
	}

	// Reject-reason log line. Formats only when debug is enabled — with
	// it off (the shipping config) a reject costs one bool test.
	template <class... Args>
	void DbgReject(const char* a_what, std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		if (!Settings::GetSingleton()->debugEnabled) return;
		DebugDraw::GetSingleton()->Event(std::format("{} rejected: {}",
			a_what, std::format(a_fmt, std::forward<Args>(a_args)...)));
	}

	// Best-effort form type of a hit: walk the scene graph for a userData
	// pointer (TESObjectREFR* by engine convention).
	bool HitIsDoor(const Raycast::RayHit& a_hit)
	{
		for (auto* p = a_hit.obj; p; p = p->parent) {
			if (p->userData) {
				auto* ref = reinterpret_cast<RE::TESObjectREFR*>(p->userData);
				auto* base = ref->data.objectReference;
				return base && base->GetFormType() == RE::ENUM_FORM_ID::kDOOR;
			}
		}
		return false;
	}

	// Water rule: never produce a ledge below the cell's water surface.
	bool BelowWater(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_point)
	{
		auto* cell = a_player->parentCell;
		if (!cell || !cell->HasWater()) return false;
		const float wh = cell->waterHeight;
		if (!std::isfinite(wh) || std::fabs(wh) > 1.0e9f) return false;
		return a_point.z < wh - 10.0f;
	}

	// Measured thickness of the obstacle top along the approach dir,
	// starting at the ledge point. Down rays from above the top; counted
	// while the surface stays within 15 units of the ledge height.
	float MeasureTopDepth(const RE::NiPoint3& a_ledge, const RE::NiPoint3& a_dir, float a_maxDepth)
	{
		const RE::NiPoint3 down{ 0.0f, 0.0f, -1.0f };
		float depth = 0.0f;
		const int steps = static_cast<int>(a_maxDepth / kSweepStep) + 1;
		// FOLLOW the terrain instead of demanding a flat shelf at lip
		// height: on natural slopes (rocky hillsides) the surface keeps
		// rising, which is standable continuation, not a thin top. The
		// per-step band still breaks on cliffs, drops, and true walls
		// (>15u jump over a 5u step = steeper than ~71 degrees).
		float refZ = a_ledge.z;
		for (int i = 1; i <= steps; ++i) {
			RE::NiPoint3 start = Add(a_ledge, Mul(a_dir, static_cast<float>(i) * kSweepStep));
			start.z = refZ + 30.0f;
			Raycast::RayHit hit{};
			Raycast::CastDir(start, down, 60.0f, hit);
			const float surfZ = start.z - hit.distance;
			const bool onTop = hit.hit && std::fabs(surfZ - refZ) <= 15.0f;
			DbgRay(start, down, 60.0f, hit, onTop, nullptr);
			if (!onTop) break;
			refZ = surfZ;
			depth = static_cast<float>(i) * kSweepStep;
		}
		return depth;
	}

	// Headroom above the on-top area. Probes a 3x3-ish grid over the
	// whole footprint the capsule occupies (lip, standing point, and a
	// step past it, each with lateral offsets) so a beam or overhang
	// BETWEEN two thin rays can no longer slip through, plus a safety
	// margin over the crouched capsule height.
	Headroom MeasureHeadroom(const RE::NiPoint3& a_lip, const RE::NiPoint3& a_top, const RE::NiPoint3& a_dir)
	{
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
		const RE::NiPoint3 side{ a_dir.y, -a_dir.x, 0.0f };
		constexpr float kCrouchMargin = 6.0f;  // above the crouched capsule

		float clearance = 10000.0f;
		const RE::NiPoint3 rows[3] = {
			a_lip,
			a_top,
			Add(a_top, Mul(a_dir, 12.0f)),
		};
		for (const auto& row : rows) {
			for (int i = -1; i <= 1; ++i) {
				RE::NiPoint3 start = Add(row, Mul(side, static_cast<float>(i) * 12.0f));
				start.z = a_top.z + 5.0f;
				Raycast::RayHit hit{};
				Raycast::CastDir(start, up, Detection::kPlayerHeight, hit);
				const float c = hit.hit ? hit.distance : Detection::kPlayerHeight;
				DbgRay(start, up, Detection::kPlayerHeight, hit,
					c >= Detection::kCrouchHeight + kCrouchMargin, "head");
				clearance = std::min(clearance, c);
			}
		}

		if (clearance >= Detection::kPlayerHeight * 0.95f) return Headroom::Stand;
		if (clearance >= Detection::kCrouchHeight + kCrouchMargin) return Headroom::CrouchOnly;
		return Headroom::None;
	}

	// Approach-angle clamp (Dying Light: climb along the player's forward
	// vector; reject approaches too oblique to land). Probes the wall
	// face below the ledge lip; a missing face (thin railing) skips the
	// clamp rather than failing it.
	bool ApproachAngleOK(const RE::NiPoint3& a_playerPos, const RE::NiPoint3& a_dir,
		const RE::NiPoint3& a_ledge, float a_minHeightAboveFeet)
	{
		auto* settings = Settings::GetSingleton();

		RE::NiPoint3 probeStart = a_playerPos;
		probeStart.z = std::max(a_playerPos.z + a_minHeightAboveFeet * 0.5f, a_ledge.z - 20.0f);

		RE::NiPoint3 toLedge{ a_ledge.x - a_playerPos.x, a_ledge.y - a_playerPos.y, 0.0f };
		const float flatDist = LenFlat(toLedge);
		if (flatDist < 1.0f) return true;

		Raycast::RayHit face{};
		Raycast::CastDir(probeStart, a_dir, flatDist + 10.0f, face);
		if (!face.hit) return true;  // no face to measure against

		RE::NiPoint3 n = face.normal;
		n.z = 0.0f;
		const float nl = LenFlat(n);
		if (nl < 0.01f) return true;
		n = Mul(n, 1.0f / nl);
		const float facing = -(a_dir.x * n.x + a_dir.y * n.y);  // 1 = head-on
		const float minFacing = std::cos(settings->maxApproachAngleDeg * 3.14159265f / 180.0f);
		if (facing < minFacing) {
			DbgReject("mantle", "approach too oblique ({:.0f} deg)",
				std::acos(std::clamp(facing, -1.0f, 1.0f)) * 180.0f / 3.14159265f);
			return false;
		}
		return true;
	}

	// ============================================================
	// Vault scan
	// ============================================================
	void VaultScan(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_dir, LedgeCandidate& a_out)
	{
		auto* settings = Settings::GetSingleton();
		const RE::NiPoint3 playerPos = { a_player->data.location.x, a_player->data.location.y, a_player->data.location.z };
		const RE::NiPoint3 down{ 0.0f, 0.0f, -1.0f };
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };

		// 0. The head-altitude ray origin must itself be FREE AIR: under
		// an overhang (leaning rock, arch) it sits inside collision, and
		// rays cast from inside convex shapes report NOTHING - the whole
		// sweep then tunnels through the mass and offers a vault THROUGH
		// it. Chest height is provably free (the capsule occupies it).
		{
			const RE::NiPoint3 chest = { playerPos.x, playerPos.y, playerPos.z + 60.0f };
			Raycast::RayHit headFree{};
			if (Raycast::CastDir(chest, up, Detection::kPlayerHeight - 55.0f, headFree)) {
				DbgRay(chest, up, Detection::kPlayerHeight - 55.0f, headFree, false, "origin blocked");
				return;
			}
		}

		// 1. Head-height forward ray: the whole approach must be clear of
		// anything tall (a wall past the vaulter means this is not a vault).
		const RE::NiPoint3 fwdStart = { playerPos.x, playerPos.y, playerPos.z + Detection::kPlayerHeight };
		const float minSpace = 2.0f * kVaultLength;
		Raycast::RayHit fwdRay{};
		Raycast::CastDir(fwdStart, a_dir, minSpace, fwdRay);
		DbgRay(fwdStart, a_dir, minSpace, fwdRay, !fwdRay.hit, "vault fwd");
		if (fwdRay.hit) {
			// A head-height wall simply means this is not vault geometry
			// (usually it IS the obstacle, taller than vault range = mantle
			// territory). Common case: no decision-log entry, the ray
			// overlay already shows it.
			return;
		}

		// 2. Down sweep: find the obstacle top, its far edge, and the far-
		// side landing.
		const float sweepDepth = Detection::kPlayerHeight + 100.0f;
		bool  foundTop = false;
		float topHeight = -10000.0f;
		int   firstTopIdx = -1;
		int   lastTopIdx = -1;
		bool  foundLanding = false;
		float landingHeight = 10000.0f;
		int   landingIdx = -1;
		int   tooHighRun = 0;
		RE::NiPoint3 ledgePoint{};

		for (int i = 0; i < kSweepIterations; ++i) {
			const float iDist = static_cast<float>(i) * kSweepStep;
			RE::NiPoint3 start = Add(playerPos, Mul(a_dir, iDist));
			start.z = fwdStart.z;

			Raycast::RayHit hit{};
			Raycast::CastDir(start, down, sweepDepth, hit);
			if (!hit.hit) {
				DbgRay(start, down, sweepDepth, hit, true, nullptr);
				continue;
			}

			const float hitHeight = (start.z - hit.distance) - playerPos.z;

			if (hitHeight > settings->maxVaultHeight) {
				// A single too-high hit is often a pencil-thin spike (a
				// leaning picket tip) that one ray happens to clip — one
				// such hit used to abort the ENTIRE scan and made broken
				// fences undetectable. Only a persisting too-high face
				// (a real wall: consecutive steps) ends the sweep.
				++tooHighRun;
				DbgRay(start, down, sweepDepth, hit, false, "too high");
				if (tooHighRun >= 2) {
					DbgReject("vault", "wall {:.0f} above max {:.0f}", hitHeight, settings->maxVaultHeight);
					return;
				}
				continue;
			}
			tooHighRun = 0;

			DbgRay(start, down, sweepDepth, hit, true, nullptr);

			// A surface materially below the obstacle top counts as the
			// far-side landing even when it is still raised relative to
			// the player — that is exactly the truck-bed / container /
			// raised-platform shape: vault OVER the lip, land INSIDE.
			constexpr float kMinLipDrop = 25.0f;
			const bool isLanding = foundTop && hitHeight <= topHeight - kMinLipDrop;
			const bool isTop = !isLanding && hitHeight > settings->minVaultHeight;

			if (isTop) {
				// NO door exclusion here: a door at vault height is a GATE,
				// the most vaultable thing in the game (the old abort made
				// the Sanctuary picket gate undetectable). Real doors are
				// taller than maxVaultHeight and never reach this branch;
				// the MANTLE scan keeps its door exclusion so nobody ends
				// up standing on a swinging gate.
				if (foundLanding) {
					// Solid again past the gap: the "landing" was a slot in
					// the obstacle, not open ground. Rescind it fully so a
					// later real landing is measured fresh.
					foundLanding = false;
					landingHeight = 10000.0f;
					landingIdx = -1;
				}
				if (hitHeight >= topHeight) {
					topHeight = hitHeight;
					ledgePoint = { start.x, start.y, start.z - hit.distance };
				}
				if (!foundTop) firstTopIdx = i;
				lastTopIdx = i;
				foundTop = true;
			} else if (isLanding) {
				if (hitHeight < landingHeight) {
					landingHeight = hitHeight;
					landingIdx = i;
				}
				foundLanding = true;
			}
		}

		if (!foundTop) return;  // nothing vaultable in front (common case, no log)
		if (!foundLanding || landingIdx < 0) {
			DbgReject("vault", "no far-side landing within sweep");
			return;
		}

		const float topDepth = static_cast<float>(lastTopIdx - firstTopIdx + 1) * kSweepStep;
		ledgePoint.z = playerPos.z + topHeight;

		// Line to the ledge from the player's HEAD - a provably free
		// origin. If solid mass sits between the head and the crossing
		// point, the candidate came from tunneled rays (origins inside
		// overhanging collision) and the "vault" would pass THROUGH the
		// obstruction. Aimed 8u above the lip, stopped 12u short, so a
		// flush wall or fence top never self-occludes.
		{
			const RE::NiPoint3 head = { playerPos.x, playerPos.y, playerPos.z + Detection::kPlayerHeight - 10.0f };
			RE::NiPoint3 to = {
				ledgePoint.x - head.x,
				ledgePoint.y - head.y,
				(ledgePoint.z + 8.0f) - head.z
			};
			const float len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
			if (len > 14.0f) {
				to.x /= len;
				to.y /= len;
				to.z /= len;
				Raycast::RayHit los{};
				if (Raycast::CastDir(head, to, len - 12.0f, los)) {
					DbgRay(head, to, len - 12.0f, los, false, "ledge occluded");
					DbgReject("vault", "no line to the ledge (mass in between)");
					return;
				}
			}
		}

		// 3. Landing validation (the "behind the object" rules). The
		// landing point sits a full capsule radius + margin past the far
		// edge — landing at the first clear ray put the 16-unit capsule
		// in overlap with the far face, and the depenetration solver
		// shoved the player straight back off the obstacle when
		// simulation resumed.
		const float farEdgeDist = static_cast<float>(lastTopIdx + 1) * kSweepStep;
		const float landingDist = std::max(
			static_cast<float>(landingIdx) * kSweepStep,
			farEdgeDist + Detection::kCapsuleRadius + 8.0f);
		RE::NiPoint3 landing = Add(playerPos, Mul(a_dir, landingDist));
		landing.z = playerPos.z + landingHeight;
		{
			// Ground height at the margined point (the sweep measured it
			// closer in; re-measure where the player will actually stand).
			RE::NiPoint3 lStart = landing;
			lStart.z = ledgePoint.z - 2.0f;
			Raycast::RayHit lHit{};
			Raycast::CastDir(lStart, down, settings->maxVaultDrop + 20.0f, lHit);
			DbgRay(lStart, down, settings->maxVaultDrop + 20.0f, lHit, lHit.hit, nullptr);
			if (lHit.hit) {
				landing.z = lStart.z - lHit.distance;
				landingHeight = landing.z - playerPos.z;
			}
		}

		const float drop = ledgePoint.z - landing.z;
		if (drop > settings->maxVaultDrop) {
			DbgReject("vault", "far-side drop {:.0f} exceeds {:.0f}", drop, settings->maxVaultDrop);
			return;
		}
		if (landingHeight > 80.0f) {
			DbgReject("vault", "far side rises {:.0f} above start", landingHeight);
			return;
		}
		if (BelowWater(a_player, landing)) {
			DbgReject("vault", "landing is underwater");
			return;
		}

		// Back clearance: room for the capsule past the far edge, checked
		// at crouch height above the landing.
		{
			RE::NiPoint3 clrStart = Add(playerPos, Mul(a_dir, static_cast<float>(lastTopIdx + 1) * kSweepStep));
			clrStart.z = landing.z + 40.0f;
			Raycast::RayHit clr{};
			Raycast::CastDir(clrStart, a_dir, settings->minBackClearance, clr);
			DbgRay(clrStart, a_dir, settings->minBackClearance, clr, !clr.hit, "back clr");
			if (clr.hit) {
				DbgReject("vault", "blocked {:.0f} past far edge ({})",
					clr.distance, Raycast::LayerName(clr.layer));
				return;
			}
		}

		// 4. Railing / thin-structure checks (SkyParkourNG): an up ray on
		// the ledge detects railing-like geometry; side rays reject
		// horizontally tiny posts.
		{
			RE::NiPoint3 upStart = Add(ledgePoint, RE::NiPoint3{ 0.0f, 0.0f, 5.0f });
			Raycast::RayHit upRay{};
			Raycast::CastDir(upStart, up, Detection::kPlayerHeight * 0.5f, upRay);
			float sideCheck = 15.0f;
			if (upRay.hit) {
				DbgRay(upStart, up, Detection::kPlayerHeight * 0.5f, upRay, upRay.distance >= 55.0f, "rail");
				if (upRay.distance < 55.0f) {
					DbgReject("vault", "no clearance over the obstacle");
					return;
				}
				sideCheck = 30.0f;
			}

			const RE::NiPoint3 sideR{ a_dir.y, -a_dir.x, 0.0f };
			const RE::NiPoint3 sideL{ -a_dir.y, a_dir.x, 0.0f };
			RE::NiPoint3 sideStart = Add(upStart, RE::NiPoint3{ 0.0f, 0.0f, 5.0f });
			Raycast::RayHit rR{}, rL{};
			Raycast::CastDir(sideStart, sideR, sideCheck, rR);
			Raycast::CastDir(sideStart, sideL, sideCheck, rL);
			DbgRay(sideStart, sideR, sideCheck, rR, !rR.hit, nullptr);
			DbgRay(sideStart, sideL, sideCheck, rL, !rL.hit, nullptr);
			if (rR.hit || rL.hit) {
				DbgReject("vault", "structure too narrow sideways");
				return;
			}
		}

		a_out.vaultEligible = true;
		a_out.vaultLedge = ledgePoint;
		a_out.vaultHeight = topHeight;
		a_out.vaultTopDepth = topDepth;
		a_out.vaultLanding = landing;
		a_out.vaultDrop = drop;

		if (settings->debugEnabled) {
			DebugDraw::GetSingleton()->AddMarker(ledgePoint, 0xFF00E060, "vault ledge");
			DebugDraw::GetSingleton()->AddMarker(landing, 0xFFFFB020, "landing");
		}
	}

	// ============================================================
	// Mantle scan — SkyParkourNG semantics: step forward through clear
	// air above the candidate region, cast down at each step, take the
	// first surface that passes the basic ledge rules.
	// ============================================================
	void MantleScan(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_dir, bool a_fromAir, LedgeCandidate& a_out)
	{
		auto* settings = Settings::GetSingleton();
		const RE::NiPoint3 playerPos = { a_player->data.location.x, a_player->data.location.y, a_player->data.location.z };
		const RE::NiPoint3 down{ 0.0f, 0.0f, -1.0f };
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };

		const float maxLedgeHeight = settings->maxMantleHeight + (a_fromAir ? settings->airGrabExtraReach : 0.0f);
		const float minLedgeHeight = a_fromAir ? 20.0f : settings->minMantleHeight;

		// 1. Headroom above the player: room to rise at all.
		const RE::NiPoint3 upStart = { playerPos.x, playerPos.y, playerPos.z + Detection::kPlayerHeight };
		const float wantedScanZ = playerPos.z + maxLedgeHeight + 15.0f;  // just above the tallest legal top
		const float maxUpCheck = std::max(20.0f, wantedScanZ - upStart.z);
		// Only a genuinely cramped space (crawl height right above the
		// head) aborts the scan. The old 40u veto killed HIGH mantles at
		// any building trim/eave/pipe hanging above the player, even
		// though the arc rises FORWARD, not straight up - per-lip sky
		// checks and apex-onward path validation own real clearance now.
		const float minUpCheck = 15.0f;
		Raycast::RayHit upRay{};
		Raycast::CastDir(upStart, up, maxUpCheck, upRay);
		DbgRay(upStart, up, maxUpCheck, upRay, !(upRay.hit && upRay.distance < minUpCheck), "mantle up");
		if (upRay.hit && upRay.distance < minUpCheck) {
			DbgReject("mantle", "no headroom above player");
			return;
		}

		// 2. Scan altitude: just above the tallest legal ledge (the
		// SkyParkour model — their forward sweep always runs at max
		// ledge height). The up-ray caps it ONLY when it hit a flat
		// CEILING (normal facing straight down), which keeps interior
		// scans from discovering the floor above. A sloped overhang —
		// a boulder bulge leaning over the player — must NOT cap the
		// scan: the old unconditional cap pinned scanZ under the bulge,
		// the forward sweep died on the rock face instantly, and big
		// rounded rocks were unmantleable from below.
		float scanZ = wantedScanZ;
		if (upRay.hit && upRay.normal.z <= -0.85f) {
			scanZ = upStart.z + upRay.distance - 5.0f;
		}
		const RE::NiPoint3 fwdStart = { playerPos.x, playerPos.y, scanZ };

		// 3. Down-rays from each clear forward step. EVERY validation runs
		// per step and failure moves to the NEXT step instead of aborting
		// the scan — on curved geometry (fallen trees, car hoods, pipes)
		// the first lip the down-ray finds sits on the front slope and
		// fails the behind-the-lip / steepness checks, while a step or two
		// further the crest passes everything. Aborting on the first bad
		// lip was why big rounded obstacles refused to mantle. The heavy
		// checks (depth probes + headroom grid) are capped to a few
		// attempts per scan to bound the ray budget.
		const float downDist = (scanZ - playerPos.z) + 20.0f;
		int heavyTries = 0;
		constexpr int kMaxHeavyTries = 6;

		for (int i = 1; i <= kMantleFwdIterations; ++i) {
			const float stepDist = kSweepStep * static_cast<float>(i);

			// Per-step forward probe (SkyParkourNG): a blocked step is
			// SKIPPED, not a scan abort — geometry poking into the scan
			// altitude close to the player must not hide a valid lip
			// further along the sweep.
			Raycast::RayHit fwdProbe{};
			Raycast::CastDir(fwdStart, a_dir, stepDist, fwdProbe);
			if (fwdProbe.hit && fwdProbe.distance < stepDist - 1.0f) continue;

			RE::NiPoint3 rayStart = Add(fwdStart, Mul(a_dir, stepDist));
			Raycast::RayHit ledgeRay{};
			Raycast::CastDir(rayStart, down, downDist, ledgeRay);
			DbgRay(rayStart, down, downDist, ledgeRay, ledgeRay.hit, nullptr);
			if (!ledgeRay.hit || ledgeRay.distance < 10.0f) continue;
			if (ledgeRay.normal.z < kMinLedgeFlatness) continue;

			const float h = (rayStart.z - ledgeRay.distance) - playerPos.z;
			if (h < minLedgeHeight || h > maxLedgeHeight) continue;
			if (HitIsDoor(ledgeRay)) continue;

			const RE::NiPoint3 ledgePoint{ rayStart.x, rayStart.y, rayStart.z - ledgeRay.distance };
			if (BelowWater(a_player, ledgePoint)) continue;

			// One-ray sky precheck BEFORE spending a heavy try: under a
			// leaning face (big boulders) the sweep finds shelf after
			// shelf with rock directly above, and those drained the whole
			// heavy budget before the sweep ever reached the crest
			// ("gave up after N lips" with every reject reading "head").
			{
				RE::NiPoint3 skyStart = Add(ledgePoint, RE::NiPoint3{ 0.0f, 0.0f, 5.0f });
				Raycast::RayHit sky{};
				Raycast::CastDir(skyStart, up, Detection::kCrouchHeight, sky);
				if (sky.hit && sky.distance < Detection::kCrouchHeight) {
					DbgRay(skyStart, up, Detection::kCrouchHeight, sky, false, "lip sky");
					continue;
				}
			}

			// Line to the lip from the player's HEAD (provably free
			// origin): mass in between means this lip was found by rays
			// whose origins sat inside overhanging collision - a move to
			// it would TUNNEL through the obstruction (the column bug).
			// One ray, charged before the heavy budget.
			//
			// Judged from where the mantle will actually START: closer
			// than the align distance, the move first glides the capsule
			// back, so visibility is measured from that aligned spot -
			// hugging the wall no longer self-rejects the lip.
			{
				RE::NiPoint3 losBase = { playerPos.x, playerPos.y, playerPos.z };
				const float projLip = (ledgePoint.x - playerPos.x) * a_dir.x +
				                      (ledgePoint.y - playerPos.y) * a_dir.y;
				if (projLip < 32.0f) {
					losBase.x = ledgePoint.x - a_dir.x * 32.0f;
					losBase.y = ledgePoint.y - a_dir.y * 32.0f;
				}
				const RE::NiPoint3 head = { losBase.x, losBase.y, losBase.z + Detection::kPlayerHeight - 10.0f };
				RE::NiPoint3 to = {
					ledgePoint.x - head.x,
					ledgePoint.y - head.y,
					(ledgePoint.z + 8.0f) - head.z
				};
				const float len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
				if (len > 14.0f) {
					to.x /= len;
					to.y /= len;
					to.z /= len;
					Raycast::RayHit los{};
					if (Raycast::CastDir(head, to, len - 12.0f, los)) {
						DbgRay(head, to, len - 12.0f, los, false, "lip occluded");
						continue;
					}
				}
			}

			// Cheap step checks passed — heavy validation, bounded.
			if (++heavyTries > kMaxHeavyTries) {
				DbgReject("mantle", "gave up after {} candidate lips failed validation", kMaxHeavyTries);
				return;
			}

			// Approach angle against the wall face under the lip.
			if (!ApproachAngleOK(playerPos, a_dir, ledgePoint, minLedgeHeight)) {
				continue;
			}

			// Obstruction right behind the lip (SkyParkourNG): pull back
			// and probe forward — a wall rising just past THIS lip means
			// try the next step, where the surface may have crested.
			{
				const float obsBack = 15.0f;
				RE::NiPoint3 obsStart = Add(Add(ledgePoint, RE::NiPoint3{ 0.0f, 0.0f, 5.0f }), Mul(a_dir, -obsBack));
				Raycast::RayHit obs{};
				Raycast::CastDir(obsStart, a_dir, obsBack + 15.0f, obs);
				const float minSpace = obsBack + 3.0f;
				// Rising ground behind the lip only counts as a WALL when
				// it is too steep to stand on — a walkable continuation
				// (natural slopes) is a terrain step: mantle onto it,
				// then mantle again (SkyParkour climbs hillsides the
				// same way, as chained steps).
				const bool wall = obs.hit && obs.distance < minSpace && obs.normal.z < 0.55f;
				DbgRay(obsStart, a_dir, obsBack + 15.0f, obs, !wall, "lip clr");
				if (wall) {
					DbgReject("mantle", "wall behind lip at step {} - trying further", i);
					continue;
				}
			}

			// Top depth — the thin-top rule.
			const float topDepth = MeasureTopDepth(ledgePoint, a_dir, std::max(settings->minMantleDepth * 2.0f, 60.0f));
			if (topDepth < settings->minMantleDepth) {
				DbgReject("mantle", "top depth {:.0f} below {:.0f} at step {} - trying further",
					topDepth, settings->minMantleDepth, i);
				continue;
			}

			// Standing point on top, height MEASURED at the stand point
			// (curved tops: the crest is not at lip height). The capsule
			// is kCapsuleRadius wide: a stand point closer than radius+8
			// to the lip leaves half the capsule hanging over the edge,
			// and the engine slides it straight back off ("mantle up and
			// fall right back down") — stand at least that far in, but
			// never past the measured top depth.
			const float standIn = std::min(
				std::max(topDepth * 0.5f, Detection::kCapsuleRadius + 8.0f),
				std::max(4.0f, topDepth - 4.0f));
			RE::NiPoint3 target = Add(ledgePoint, Mul(a_dir, standIn));
			target.z = ledgePoint.z;
			{
				// Wide measurement band: on rising terrain the stand
				// point sits well ABOVE the lip (the old +/-25 band left
				// the target buried inside the hill).
				RE::NiPoint3 tStart = target;
				tStart.z = ledgePoint.z + 60.0f;
				Raycast::RayHit tHit{};
				Raycast::CastDir(tStart, down, 120.0f, tHit);
				if (tHit.hit && std::fabs((tStart.z - tHit.distance) - ledgePoint.z) <= 45.0f) {
					target.z = tStart.z - tHit.distance;
					if (tHit.normal.z < 0.45f) {
						DbgReject("mantle", "stand point too steep (n.z {:.2f}) at step {} - trying further",
							tHit.normal.z, i);
						continue;
					}
					// A stand point materially BELOW its lip is the front
					// slope pretending to be a landing (seen on boulders:
					// the "stand" marker under the lip marker).
					if (target.z < ledgePoint.z - 10.0f) {
						DbgReject("mantle", "stand point {:.0f} below lip at step {} - trying further",
							ledgePoint.z - target.z, i);
						continue;
					}
				}
			}

			const Headroom headroom = MeasureHeadroom(ledgePoint, target, a_dir);
			if (headroom == Headroom::None) {
				DbgReject("mantle", "no headroom on top at step {} - trying further", i);
				continue;
			}

			// Every check passed: this lip is the mantle.
			a_out.mantleEligible = true;
			a_out.mantleLedge = ledgePoint;
			a_out.mantleHeight = h;
			a_out.mantleDepth = topDepth;
			a_out.mantleTarget = target;
			a_out.headroom = headroom;

			if (settings->debugEnabled) {
				DebugDraw::GetSingleton()->AddMarker(ledgePoint, 0xFFFF9040, "mantle ledge");
				DebugDraw::GetSingleton()->AddMarker(target,
					headroom == Headroom::Stand ? 0xFF00E060 : 0xFF40C0FF,
					headroom == Headroom::Stand ? "stand" : "crouch only");
			}
			return;
		}
		// Nothing in reach (common case, no log).
	}
}

namespace F4Parkour::Detection
{
	static RE::bhkCharacterController* CharCtrl(RE::PlayerCharacter* a_player)
	{
		if (a_player && a_player->currentProcess && a_player->currentProcess->middleHigh) {
			return a_player->currentProcess->middleHigh->charController.get();
		}
		return nullptr;
	}

	bool IsOnGround(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			return cc->context.m_currentState == RE::hknpCharacterState::hknpCharacterStateType::kOnGround;
		}
		return false;
	}

	bool IsInAir(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			const auto s = cc->context.m_currentState;
			return s == RE::hknpCharacterState::hknpCharacterStateType::kInAir ||
			       s == RE::hknpCharacterState::hknpCharacterStateType::kJumping;
		}
		return false;
	}

	bool IsSighted(RE::PlayerCharacter* a_player)
	{
		const auto gs = static_cast<std::uint32_t>(a_player->gunState);
		return gs == 6 || gs == 8;  // sighted / sighted-transition
	}

	bool IsInPowerArmor(RE::PlayerCharacter* a_player)
	{
		return a_player && RE::PowerArmor::ActorInPowerArmor(*a_player);
	}

	bool IsSprinting(RE::PlayerCharacter* a_player)
	{
		return a_player && (a_player->moveMode & 0x100) != 0;
	}

	bool IsForwardHeld()
	{
		auto* pc = RE::PlayerControls::GetSingleton();
		return pc && pc->data.moveInputVec.y > 0.1f;
	}

	RE::NiPoint3 DirFlat(RE::PlayerCharacter* a_player)
	{
		const float yaw = a_player->data.angle.z;
		return { std::sin(yaw), std::cos(yaw), 0.0f };
	}

	float HorizontalSpeed(RE::PlayerCharacter* a_player)
	{
		if (auto* cc = CharCtrl(a_player)) {
			RE::hkVector4f v;
			cc->GetLinearVelocityImpl(v);
			// Havok velocity is in Havok units; convert to game units
			// (1 hk unit = ~69.99 game units).
			const float x = v[0] * 69.9915f;
			const float y = v[1] * 69.9915f;
			return std::sqrt(x * x + y * y);
		}
		return 0.0f;
	}

	bool GroundWithin(RE::PlayerCharacter* a_player, float a_maxDrop)
	{
		RE::NiPoint3 start{ a_player->data.location.x, a_player->data.location.y,
			a_player->data.location.z + 10.0f };
		Raycast::RayHit hit{};
		return Raycast::CastDir(start, { 0.0f, 0.0f, -1.0f }, 10.0f + a_maxDrop, hit);
	}

	LedgeCandidate Scan(RE::PlayerCharacter* a_player, bool a_fromAir)
	{
		LedgeCandidate out{};
		auto* settings = Settings::GetSingleton();
		auto* dbg = DebugDraw::GetSingleton();

		if (settings->debugEnabled) dbg->BeginFrame();

		const RE::NiPoint3 dir = DirFlat(a_player);
		out.approachDir = dir;
		out.fromAir = a_fromAir;

		if (!a_fromAir) {
			VaultScan(a_player, dir, out);
		}
		MantleScan(a_player, dir, a_fromAir, out);

		if (settings->debugEnabled) dbg->CommitFrame();
		return out;
	}
}
