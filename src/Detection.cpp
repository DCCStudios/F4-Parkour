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
	constexpr int   kSweepIterations = 20;       // 100 units of forward coverage - a vault is a close, running-over move; the mantle reach can be longer without a ring/activation mismatch because a vault's ring and activation both come from THIS sweep
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
	// Vault landings pass an embed tolerance: a hit closer than this means
	// the probe STARTED inside geometry (a raised curb or slope under an
	// off-centre ray, all rows share the landing's z). No real ceiling sits
	// that low over a landing a clean down-ray just found.
	constexpr float kLandingEmbedTolerance = 8.0f;
	Headroom MeasureHeadroom(const RE::NiPoint3& a_lip, const RE::NiPoint3& a_top, const RE::NiPoint3& a_dir,
		float a_embedTolerance = 0.0f)
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
				const bool embedded = hit.hit && hit.distance < a_embedTolerance;
				const float c = (hit.hit && !embedded) ? hit.distance : Detection::kPlayerHeight;
				DbgRay(start, up, Detection::kPlayerHeight, hit,
					c >= Detection::kCrouchHeight + kCrouchMargin, "head");
				clearance = std::min(clearance, c);
			}
		}

		if (clearance >= Detection::kPlayerHeight * 0.95f) return Headroom::Stand;
		if (clearance >= Detection::kCrouchHeight + kCrouchMargin) return Headroom::CrouchOnly;
		return Headroom::None;
	}

	bool SegmentClear(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const char* a_label)
	{
		const RE::NiPoint3 d{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
		const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		if (len <= 1.0f) return true;
		Raycast::RayHit hit{};
		const bool blocked = Raycast::Cast(a_from, a_to, hit) && hit.hit;
		DbgRay(a_from, { d.x / len, d.y / len, d.z / len }, len, hit, !blocked, a_label);
		return !blocked;
	}

	enum class Corridor { Clear, Blocked, NoFloor };

	// FAR-SIDE CORRIDOR from PROVEN-FREE origins. Every far-side probe (the
	// landing down-ray from lip-2, the back-clearance ray, the headroom
	// grid) ORIGINATES past the far edge at or below lip level, exactly
	// where a second, taller structure would stand. Rays that start inside
	// collision report nothing, so a low wall flush against a building read
	// as "landing on the floor inside the building" and the vault passed
	// THROUGH the wall. Every origin here is a point an earlier ray passed
	// through: the over-the-top check proved 5..65u above the lip, and the
	// sweep's own down-ray passed through the air 5u above the LAST TOP
	// SAMPLE (still on the obstacle, never past its face, so it cannot be
	// inside a structure flush behind it).
	//   Leg 1, level at lip+50 from the lip to over the landing: anything
	//          taller than the vault's own crossing.
	//   Leg 2a, level at lip+5 from the last top sample to just past the
	//          far face: a wall flush behind the obstacle, of ANY height
	//          above the top, crosses this line. Nothing descends until
	//          this has proven the air past the face.
	//   Leg 2b, descending from that proven point to chest height at the
	//          landing: a structure set back behind the obstacle.
	//   Leg 3 (floor known), straight down onto the floor from over the
	//          landing: mass sitting on or just over the landing. A hit
	//          high enough to crouch under is an awning (legs 2a/2b proved
	//          the space beneath); MeasureHeadroom then rules on the exact
	//          headroom. A floor that cannot be confirmed from free air
	//          returns NoFloor so the caller can fall back to a release.
	//   Release column (no floor), from over the landing down to 10u below
	//          the release point: nothing may sit in the column the player
	//          drops through. The fall itself is the engine's business.
	Corridor FarSideCorridorClear(const RE::NiPoint3& a_lip, const RE::NiPoint3& a_lastTop,
		const RE::NiPoint3& a_pastEdge, const RE::NiPoint3& a_landing, const char** a_why, bool a_floorKnown)
	{
		constexpr float kLift = 50.0f;
		constexpr float kSkim = 5.0f;    // the over-the-top / sweep rays proved this height
		constexpr float kChest = 40.0f;  // the height the back-clearance ray already uses
		const RE::NiPoint3 origin{ a_lip.x, a_lip.y, a_lip.z + kLift };
		const RE::NiPoint3 overLanding{ a_landing.x, a_landing.y, a_lip.z + kLift };
		if (!SegmentClear(origin, overLanding, "far corridor")) {
			*a_why = "solid mass between the lip and the landing (would vault through it)";
			return Corridor::Blocked;
		}
		const RE::NiPoint3 topSkim{ a_lastTop.x, a_lastTop.y, a_lip.z + kSkim };
		const RE::NiPoint3 pastEdge{ a_pastEdge.x, a_pastEdge.y, a_lip.z + kSkim };
		if (!SegmentClear(topSkim, pastEdge, "far edge skim")) {
			*a_why = "solid mass flush behind the obstacle (would vault into it)";
			return Corridor::Blocked;
		}
		const RE::NiPoint3 chest{ a_landing.x, a_landing.y, a_landing.z + kChest };
		if (!SegmentClear(pastEdge, chest, "far descent")) {
			*a_why = "solid mass behind the obstacle (would vault into it)";
			return Corridor::Blocked;
		}
		if (!a_floorKnown) {
			// The column is checked over the capsule's FOOTPRINT (centre plus
			// a ring just inside the capsule radius), the same idea as
			// FootprintClear for floor landings: a railing post or ledge
			// trim beside the release point is exactly what a single
			// centre ray misses, and this is the geometry release vaults
			// live on.
			constexpr float kRingR = Detection::kCapsuleRadius - 3.0f;
			for (int i = -1; i < 6; ++i) {
				const float ang = static_cast<float>(i) * 1.0471976f;  // 60 deg
				const float ox = i < 0 ? 0.0f : std::cos(ang) * kRingR;
				const float oy = i < 0 ? 0.0f : std::sin(ang) * kRingR;
				const RE::NiPoint3 top{ a_landing.x + ox, a_landing.y + oy, a_lip.z + kLift };
				const RE::NiPoint3 below{ a_landing.x + ox, a_landing.y + oy, a_landing.z - 10.0f };
				if (!SegmentClear(top, below, i < 0 ? "release column" : nullptr)) {
					*a_why = "solid mass in the release column (would drop onto it)";
					return Corridor::Blocked;
				}
			}
			return Corridor::Clear;
		}
		const RE::NiPoint3 floor{ a_landing.x, a_landing.y, a_landing.z - 6.0f };
		Raycast::RayHit drop{};
		Raycast::Cast(overLanding, floor, drop);
		const float surfaceZ = drop.hit ? overLanding.z - drop.distance : -1.0e9f;
		const bool ok = drop.hit &&
			(surfaceZ <= a_landing.z + 12.0f ||
			 surfaceZ >= a_landing.z + Detection::kCrouchHeight + 6.0f);
		DbgRay(overLanding, { 0.0f, 0.0f, -1.0f }, overLanding.z - floor.z, drop, ok, "far floor");
		if (!drop.hit) {
			*a_why = "landing floor not reachable from free air";
			return Corridor::NoFloor;
		}
		if (!ok) {
			*a_why = "solid mass on the landing (would vault into it)";
			return Corridor::NoFloor;
		}
		return Corridor::Clear;
	}

	// Approach-angle clamp (Dying Light: climb along the player's forward
	// vector; reject approaches too oblique to land). Probes the wall
	// face below the ledge lip; a missing face (thin railing) skips the
	// clamp rather than failing it.
	bool ApproachAngleOK(const RE::NiPoint3& a_playerPos, const RE::NiPoint3& a_dir,
		const RE::NiPoint3& a_ledge, float a_minHeightAboveFeet,
		float* a_facingOut = nullptr, const char* a_what = "mantle")
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
		if (a_facingOut) *a_facingOut = facing;
		const float minFacing = std::cos(settings->maxApproachAngleDeg * 3.14159265f / 180.0f);
		if (facing < minFacing) {
			DbgReject(a_what, "approach too oblique ({:.0f} deg)",
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
		int   missRunAfterTop = 0;
		RE::NiPoint3 ledgePoint{};

		for (int i = 0; i < kSweepIterations; ++i) {
			const float iDist = static_cast<float>(i) * kSweepStep;
			RE::NiPoint3 start = Add(playerPos, Mul(a_dir, iDist));
			start.z = fwdStart.z;

			Raycast::RayHit hit{};
			Raycast::CastDir(start, down, sweepDepth, hit);
			if (!hit.hit) {
				DbgRay(start, down, sweepDepth, hit, true, nullptr);
				// Past the obstacle, a run of misses means the far side
				// drops beyond the sweep's reach: the release-vault shape
				// (a balcony, a wall onto much lower ground). After 30u of
				// open air stop the sweep, so a second structure farther
				// out cannot merge into this obstacle's top across the
				// chasm. Shorter gaps (a post gap between fence panels)
				// keep the old merge behaviour.
				if (foundTop && ++missRunAfterTop >= 6) break;
				continue;
			}
			missRunAfterTop = 0;

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
		// NO FLOOR IN REACH is not a rejection: it is the balcony shape. The
		// vault crosses the obstacle and RELEASES the player into the
		// engine's own fall just past the lip; the far side only has to be
		// open air (corridor, headroom, release column below). The landing
		// point then IS the release point.
		bool noFloor = !foundLanding || landingIdx < 0;

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

		// Approach angle against the near face, and the OBLIQUENESS SCALE for
		// every far-side margin: the sweep and the landing offset both run
		// along the approach direction, so at 45 degrees a 24u margin along
		// the ray is only 17u perpendicular to the face, inside the 16u
		// capsule. Scale the margins by 1/cos so the PERPENDICULAR clearance
		// stays what the numbers say.
		float facing = 1.0f;
		if (!ApproachAngleOK(playerPos, a_dir, ledgePoint, settings->minVaultHeight, &facing, "vault")) {
			return;
		}
		const float obliqueScale = 1.0f / std::max(facing, 0.6f);

		// 3. Landing validation (the "behind the object" rules). The
		// landing point sits a full capsule radius + margin past the far
		// edge — landing at the first clear ray put the 16-unit capsule
		// in overlap with the far face, and the depenetration solver
		// shoved the player straight back off the obstacle when
		// simulation resumed.
		const float farEdgeDist = static_cast<float>(lastTopIdx + 1) * kSweepStep;
		const float landingDist = std::max(
			static_cast<float>(landingIdx) * kSweepStep,
			farEdgeDist + (Detection::kCapsuleRadius + 8.0f) * obliqueScale);
		RE::NiPoint3 landing = Add(playerPos, Mul(a_dir, landingDist));
		landing.z = playerPos.z + landingHeight;
		if (noFloor) {
			landing.z = ledgePoint.z - Detection::kReleaseDescent;
			landingHeight = landing.z - playerPos.z;
		}
		if (!noFloor) {
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

		float drop = ledgePoint.z - landing.z;
		if (!noFloor && drop > settings->maxVaultDrop) {
			// Too deep to glide to: release over it instead.
			noFloor = true;
			landing.z = ledgePoint.z - Detection::kReleaseDescent;
			drop = ledgePoint.z - landing.z;
			landingHeight = landing.z - playerPos.z;
		}
		if (!noFloor && landingHeight > 80.0f) {
			DbgReject("vault", "far side rises {:.0f} above start", landingHeight);
			return;
		}
		if (BelowWater(a_player, landing)) {
			if (!noFloor) {
				// Fall into the water rather than glide into it pinned grounded.
				noFloor = true;
				landing.z = ledgePoint.z - Detection::kReleaseDescent;
				drop = ledgePoint.z - landing.z;
				landingHeight = landing.z - playerPos.z;
			}
			if (BelowWater(a_player, landing)) {
				DbgReject("vault", "release point is underwater");
				return;
			}
		}

		// FOOTPRINT: the thin rays above accepted this landing, but the
		// capsule is 16u wide — on jagged rock the point can thread a gap
		// while the capsule overlaps a bulge ("end the vault clipped into
		// the rock"). Step the landing forward until the full footprint
		// fits; if it never does, this is not a landing.
		const RE::NiPoint3 landingBase = landing;
		if (!noFloor) {
			int fpTries = 0;
			while (!Detection::FootprintClear(landing) && ++fpTries <= 3) {
				landing.x += a_dir.x * 10.0f;
				landing.y += a_dir.y * 10.0f;
				RE::NiPoint3 lStart = landing;
				lStart.z = ledgePoint.z - 2.0f;
				Raycast::RayHit lHit{};
				if (Raycast::CastDir(lStart, down, settings->maxVaultDrop + 20.0f, lHit) && lHit.hit) {
					landing.z = lStart.z - lHit.distance;
				}
			}
			if (fpTries > 3) {
				// The capsule never fits on that floor: release over it and
				// let physics settle the player wherever it lands.
				noFloor = true;
				landing = landingBase;
				landing.z = ledgePoint.z - Detection::kReleaseDescent;
				drop = ledgePoint.z - landing.z;
				landingHeight = landing.z - playerPos.z;
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

		// Far-side corridor from proven-free air. Runs LAST on purpose: the
		// rail check above just proved 5..65u over the lip, so every origin
		// it casts from has been verified by the time it runs.
		{
			// Last top sample (the sweep proved the air above it) and a point
			// 8u past the true far face ((lastTopIdx + 1) * step, the same
			// convention the landing margin uses).
			const RE::NiPoint3 lastTop = Add(playerPos, Mul(a_dir, static_cast<float>(lastTopIdx) * kSweepStep));
			const RE::NiPoint3 pastEdge = Add(playerPos, Mul(a_dir, static_cast<float>(lastTopIdx + 1) * kSweepStep + 8.0f));
			const char* why = nullptr;
			Corridor c = FarSideCorridorClear(ledgePoint, lastTop, pastEdge, landing, &why, !noFloor);
			if (c == Corridor::NoFloor) {
				// The floor could not be confirmed from free air: release over
				// the obstacle instead of gliding to it.
				noFloor = true;
				landing = landingBase;
				landing.z = ledgePoint.z - Detection::kReleaseDescent;
				drop = ledgePoint.z - landing.z;
				landingHeight = landing.z - playerPos.z;
				c = FarSideCorridorClear(ledgePoint, lastTop, pastEdge, landing, &why, false);
			}
			if (c != Corridor::Clear) {
				DbgReject("vault", "{}", why);
				return;
			}
		}

		// Back clearance: room for the capsule past the far edge, checked
		// at crouch height above the landing or the release point. Runs
		// after the corridor so a late no-floor conversion is measured at
		// the release height, not the deep floor.
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

		// Headroom AT the landing, or at the release point for a no-floor
		// vault (never vault INTO an object), classified
		// Stand / CrouchOnly / None over the whole capsule footprint with the
		// mantle's 3x3 grid instead of one centre ray, so an overhang offset
		// from the landing centre cannot slip through. CrouchOnly is ALLOWED
		// and recorded on the candidate: the Mover crouches the player as it
		// lands, exactly like a crouch-only mantle.
		const Headroom landHeadroom = MeasureHeadroom(
			Add(landing, Mul(a_dir, -10.0f)), landing, a_dir, kLandingEmbedTolerance);
		if (landHeadroom == Headroom::None) {
			DbgReject("vault", "no headroom at the landing (not even crouched)");
			return;
		}

		a_out.vaultEligible = true;
		a_out.vaultLedge = ledgePoint;
		a_out.vaultHeight = topHeight;
		a_out.vaultTopDepth = topDepth;
		a_out.vaultLanding = landing;
		a_out.vaultDrop = ledgePoint.z - landing.z;
		a_out.vaultNoFloor = noFloor;
		a_out.vaultHeadroom = landHeadroom;

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
				// BURIED-lip test only: solid mass within ~30u straight
				// above the lip. The original full-crouch-height version
				// rejected every lip under a rounded crest or mid-face
				// bulge - the stacked "lip sky" columns on the Sanctuary
				// rock - including CROUCH-ONLY candidates that
				// MeasureHeadroom's footprint grid would have accepted.
				// Real headroom judgement (stand / crouch / none) stays
				// with that grid, inside the heavy budget this precheck
				// exists to protect.
				constexpr float kBuriedLip = 30.0f;
				RE::NiPoint3 skyStart = Add(ledgePoint, RE::NiPoint3{ 0.0f, 0.0f, 5.0f });
				Raycast::RayHit sky{};
				Raycast::CastDir(skyStart, up, kBuriedLip, sky);
				if (sky.hit && sky.distance < kBuriedLip) {
					DbgRay(skyStart, up, kBuriedLip, sky, false, "lip sky");
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
			float mantleFacing = 1.0f;
			if (!ApproachAngleOK(playerPos, a_dir, ledgePoint, minLedgeHeight, &mantleFacing)) {
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
				// it is NEAR-VERTICAL — a walkable OR clamberable rise
				// (natural slopes, domed rock crests that curve up past
				// the lip) is a terrain step: mantle onto it, then mantle
				// again (SkyParkour climbs hillsides the same way, as
				// chained steps). 0.35 ~= surfaces steeper than ~70deg
				// block; everything shallower is treated as standable.
				const bool wall = obs.hit && obs.distance < minSpace && obs.normal.z < 0.35f;
				DbgRay(obsStart, a_dir, obsBack + 15.0f, obs, !wall, "lip clr");
				if (wall) {
					DbgReject("mantle", "wall behind lip at step {} - trying further", i);
					continue;
				}
			}

			// Top depth — the thin-top rule.
			const float topDepth = MeasureTopDepth(ledgePoint, a_dir, std::max(settings->minMantleDepth * 2.0f, 60.0f));
			if (topDepth < settings->minMantleDepth) {
				// THIN-WALL VAULT-OVER (the chain-link fence rule): a lip
				// you cannot STAND on but with ground on the far side is a
				// VAULT, not a dead end. These walls sit above the vault
				// scan's height cap (its sweep aborts on the too-high run),
				// so without this branch a tall fence produced NOTHING and
				// could only be cleared by jumping first (air-vault height
				// math). The lip already passed the head-LOS check above.
				constexpr float kMaxThinWallVaultHeight = 135.0f;
				const float relHeight = ledgePoint.z - playerPos.z;
				if (!a_out.vaultEligible && relHeight <= kMaxThinWallVaultHeight) {
					const RE::NiPoint3 upv{ 0.0f, 0.0f, 1.0f };
					const RE::NiPoint3 downv{ 0.0f, 0.0f, -1.0f };
					// Room to pass over the top.
					RE::NiPoint3 overStart = Add(ledgePoint, RE::NiPoint3{ 0.0f, 0.0f, 5.0f });
					Raycast::RayHit over{};
					Raycast::CastDir(overStart, upv, 60.0f, over);
					if (!over.hit) {
						// Far-side ground, margined a full capsule past the wall.
						// Margin scaled by the approach obliqueness (see VaultScan).
						const float pastWall = std::max(topDepth, 4.0f) + (Detection::kCapsuleRadius + 12.0f) / std::max(mantleFacing, 0.6f);
						RE::NiPoint3 landing = Add(ledgePoint, Mul(a_dir, pastWall));
						landing.z = ledgePoint.z - 2.0f;
						Raycast::RayHit lg{};
						Raycast::CastDir(landing, downv, settings->maxVaultDrop + 20.0f, lg);
						// No floor in reach, or one too deep to glide to: a RELEASE
						// vault over the wall into the engine's own fall. The landing
						// then IS the release point just past the lip.
						bool fenceNoFloor = !lg.hit;
						if (lg.hit) {
							landing.z = (ledgePoint.z - 2.0f) - lg.distance;
							if (ledgePoint.z - landing.z > settings->maxVaultDrop) fenceNoFloor = true;
						}
						if (fenceNoFloor) landing.z = ledgePoint.z - Detection::kReleaseDescent;
						// Back clearance for the capsule past the wall.
						RE::NiPoint3 clrStart = Add(ledgePoint, Mul(a_dir, std::max(topDepth, 4.0f)));
						clrStart.z = landing.z + 40.0f;
						Raycast::RayHit clr{};
						Raycast::CastDir(clrStart, a_dir, settings->minBackClearance, clr);
						const char* fenceWhy = nullptr;
						const RE::NiPoint3 fencePastEdge = Add(ledgePoint, Mul(a_dir, std::max(topDepth, 4.0f) + 8.0f));
						Corridor fc = FarSideCorridorClear(ledgePoint, ledgePoint, fencePastEdge, landing, &fenceWhy, !fenceNoFloor);
						if (fc == Corridor::NoFloor) {
							fenceNoFloor = true;
							landing.z = ledgePoint.z - Detection::kReleaseDescent;
							fc = FarSideCorridorClear(ledgePoint, ledgePoint, fencePastEdge, landing, &fenceWhy, false);
							if (fc == Corridor::Clear) fenceWhy = nullptr;
						}
						const float fenceDrop = ledgePoint.z - landing.z;
						const float landRise = landing.z - playerPos.z;
						// Headroom AT the landing or the release point (never vault
						// INTO an object), classified over the capsule footprint;
						// CrouchOnly is allowed and the Mover crouches the player.
						const Headroom fenceHeadroom = MeasureHeadroom(
							Add(landing, Mul(a_dir, -10.0f)), landing, a_dir, kLandingEmbedTolerance);
						const bool floorOK = fenceNoFloor ||
							(fenceDrop >= 25.0f && landRise <= 80.0f && Detection::FootprintClear(landing));
						if (fc == Corridor::Clear && floorOK && fenceHeadroom != Headroom::None && !clr.hit &&
							!BelowWater(a_player, landing)) {
							a_out.vaultEligible = true;
							a_out.vaultLedge = ledgePoint;
							a_out.vaultHeight = relHeight;
							a_out.vaultTopDepth = topDepth;
							a_out.vaultLanding = landing;
							a_out.vaultDrop = fenceDrop;
							a_out.vaultHeadroom = fenceHeadroom;
							a_out.vaultNoFloor = fenceNoFloor;
							if (settings->debugEnabled) {
								DebugDraw::GetSingleton()->AddMarker(ledgePoint, 0xFF00E060, "fence vault");
								DebugDraw::GetSingleton()->AddMarker(landing, 0xFFFFB020, fenceNoFloor ? "release" : "landing");
								DebugDraw::GetSingleton()->Event(std::format(
									"thin-wall vault accepted: h={:.0f} depth={:.0f} drop={:.0f}{}",
									relHeight, topDepth, fenceDrop, fenceNoFloor ? " (no floor: release)" : ""));
							}
						} else if (fenceWhy) {
							DbgReject("vault", "thin wall: {}", fenceWhy);
						}
					}
				}
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

			// FOOTPRINT at the stand point — same rock-bulge rule as vault
			// landings: the thin rays accepted the spot, but the 16u capsule
			// must fit without overlapping a face that rises inside it.
			if (!Detection::FootprintClear(target)) {
				DbgReject("mantle", "stand footprint blocked at step {} - trying further", i);
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
	bool FootprintClear(const RE::NiPoint3& a_center)
	{
		constexpr float kRingR = kCapsuleRadius - 3.0f;
		for (int i = 0; i < 6; ++i) {
			const float ang = static_cast<float>(i) * 1.0471976f;  // 60 deg
			const RE::NiPoint3 start{
				a_center.x + std::cos(ang) * kRingR,
				a_center.y + std::sin(ang) * kRingR,
				a_center.z + 45.0f
			};
			Raycast::RayHit hit{};
			Raycast::CastDir(start, { 0.0f, 0.0f, -1.0f }, 80.0f, hit);
			if (hit.hit && (start.z - hit.distance) > a_center.z + 16.0f) {
				return false;  // bulge/wall inside the capsule footprint
			}
		}
		return true;
	}

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

	Headroom HeadroomAt(const RE::NiPoint3& a_pos, const RE::NiPoint3& a_dir)
	{
		return MeasureHeadroom(Add(a_pos, Mul(a_dir, -10.0f)), a_pos, a_dir, kLandingEmbedTolerance);
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

		// Vault scans on the ground always; in the air only when air-vault
		// is enabled (a mid-flight clear of a low obstacle). Mantle scans
		// in both — air mantle is the primary air move.
		if (!a_fromAir || settings->allowAirVault) {
			VaultScan(a_player, dir, out);
		}
		MantleScan(a_player, dir, a_fromAir, out);

		if (settings->debugEnabled) dbg->CommitFrame();
		return out;
	}
}
