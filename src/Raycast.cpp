#include "PCH.h"
#include "Raycast.h"

// ============================================================
// bhkPickData ray helper — ported from FPGunplayOverhaul's
// ContextualLean (the in-game-verified FO4 Havok pick recipe):
//   * kLOS query layer, per-hit COL_LAYER filtered ourselves
//   * allow-list of solid layers (unknown layers fail, never pass)
//   * actor/self hits skipped by re-casting past them
//   * fraction sanity rejection (stale results read as point-blank)
// ============================================================
namespace
{
	void SetCollisionLayer(RE::bhkPickData& a_pick, std::uint32_t a_layer)
	{
		a_pick.castQuery.m_filterData.m_collisionFilterInfo = a_layer;
	}

	// kLOS = 41 — "what blocks line of sight". Registers actor bodies too
	// (verified in-game 2026-07-27 in the ContextualLean work), so actor
	// hits are skipped explicitly in Cast.
	constexpr std::uint32_t kColLayerLOS = 41;

	// Anything closer than this to the ray start is the player's own
	// collision or a stale result, never real geometry.
	constexpr float kMinPlausibleDistance = 2.0f;

	// Does this hit belong to any actor's skeleton (including the player)?
	bool IsActorHit(RE::NiAVObject* a_obj, RE::NiAVObject* a_selfRootA, RE::NiAVObject* a_selfRootB)
	{
		for (auto* p = a_obj; p; p = p->parent) {
			if ((a_selfRootA && p == a_selfRootA) || (a_selfRootB && p == a_selfRootB)) {
				return true;
			}
			if (p->name.c_str() && _stricmp(p->name.c_str(), "skeleton.nif") == 0) {
				return true;
			}
		}
		return false;
	}
}

namespace F4Parkour::Raycast
{
	const char* LayerName(RE::COL_LAYER a_layer)
	{
		static constexpr const char* kNames[47] = {
			"Unidentified", "Static", "AnimStatic", "Transparent", "Clutter",
			"Weapon", "Projectile", "Spell", "Biped", "Trees",
			"Props", "Water", "Trigger", "Terrain", "Trap",
			"NonCollidable", "CloudTrap", "Ground", "Portal", "DebrisSmall",
			"DebrisLarge", "AcousticSpace", "ActorZone", "ProjectileZone", "GasTrap",
			"ShellCasing", "TransparentSmall", "InvisibleWall", "TransparentSmallAnim", "ClutterLarge",
			"CharController", "StairHelper", "DeadBip", "BipedNoCC", "AvoidBox",
			"CollisionBox", "CameraSphere", "DoorDetection", "ConeProjectile", "Camera",
			"ItemPicker", "LOS", "PathingPick", "Unused0", "Unused1",
			"SpellExplosion", "DroppingPick"
		};
		const auto idx = static_cast<std::int32_t>(a_layer);
		return (idx >= 0 && idx < 47) ? kNames[idx] : "DataDefined";
	}

	bool IsSolidLayer(RE::COL_LAYER a_layer)
	{
		switch (a_layer) {
		case RE::COL_LAYER::kStatic:
		case RE::COL_LAYER::kAnimStatic:
		case RE::COL_LAYER::kTransparent:
		case RE::COL_LAYER::kClutter:
		case RE::COL_LAYER::kWeapon:
		case RE::COL_LAYER::kTrees:
		case RE::COL_LAYER::kProps:
		case RE::COL_LAYER::kTerrain:
		case RE::COL_LAYER::kTrap:
		case RE::COL_LAYER::kGround:
		case RE::COL_LAYER::kDebrisLarge:
		case RE::COL_LAYER::kTransparentSmall:
		case RE::COL_LAYER::kTransparentSmallAnim:
		case RE::COL_LAYER::kClutterLarge:
			return true;
		default:
			return false;
		}
	}

	bool Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, RayHit& a_out)
	{
		a_out = RayHit{};

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;
		auto* cell = player->parentCell;
		if (!cell) return false;

		// Self roots fetched lazily: only the rare actor-hit path needs
		// them, and a full scan fires dozens of casts.
		RE::NiAVObject* selfRootA = nullptr;
		RE::NiAVObject* selfRootB = nullptr;
		bool selfRootsFetched = false;

		const RE::NiPoint3 seg{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
		const float segLen = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
		a_out.distance = segLen;
		if (segLen < 0.5f) return false;
		const float epsFrac = 4.0f / segLen;  // step past skipped hits

		float consumed = 0.0f;
		for (int attempt = 0; attempt < 3; ++attempt) {
			RE::NiPoint3 start{
				a_from.x + seg.x * consumed,
				a_from.y + seg.y * consumed,
				a_from.z + seg.z * consumed
			};
			RE::bhkPickData pick{};
			SetCollisionLayer(pick, kColLayerLOS);
			pick.SetStartEnd(start, a_to);
			RE::NiAVObject* hitObj = cell->Pick(pick);
			if (!pick.HasHit() && !hitObj) return false;

			const float embedded = pick.GetHitFraction();

			float           segFraction = -1.0f;
			RE::NiAVObject* obj = nullptr;
			RE::COL_LAYER   layer{ RE::COL_LAYER::kUnidentified };
			RE::NiPoint3    normal{};

			const std::int32_t hitCount = pick.GetAllCollectorRayHitSize();
			if (hitCount > 0) {
				float         bestFrac = 2.0f;
				RE::COL_LAYER bestLayer{ RE::COL_LAYER::kUnidentified };
				RE::NiPoint3  bestNormal{};
				for (std::int32_t i = 0; i < hitCount; ++i) {
					RE::hknpCollisionResult res{};
					if (!pick.GetAllCollectorRayHitAt(static_cast<std::uint32_t>(i), res)) {
						continue;
					}
					const float frac = res.fraction.val();
					if (!std::isfinite(frac) || frac <= 0.001f || frac > 1.0f) continue;
					const auto hitLayer = res.hitBodyInfo.shapeCollisionFilterInfo.val().GetCollisionLayer();
					if (!IsSolidLayer(hitLayer)) continue;
					if (frac < bestFrac) {
						bestFrac = frac;
						bestLayer = hitLayer;
						bestNormal = {
							res.normal[0],
							res.normal[1],
							res.normal[2]
						};
					}
				}
				if (bestFrac > 1.0f) {
					// Every hit along the ray was a non-solid helper volume.
					return false;
				}
				segFraction = bestFrac;
				layer = bestLayer;
				normal = bestNormal;
				// The engine's NiAVObject names its CLOSEST hit; it only
				// describes our chosen hit when fractions match.
				obj = (std::fabs(bestFrac - embedded) < 0.005f) ? hitObj : nullptr;
			} else {
				if (!std::isfinite(embedded) || embedded <= 0.001f || embedded > 1.0f) {
					return false;
				}
				// bhkPickData's collector is never configured, so this is the
				// path every ray takes in practice. The embedded result still
				// describes the closest hit; its filter feeds the same
				// allow-list.
				segFraction = embedded;
				layer = pick.result.hitBodyInfo.shapeCollisionFilterInfo.val().GetCollisionLayer();
				normal = {
					pick.result.normal[0],
					pick.result.normal[1],
					pick.result.normal[2]
				};
				obj = hitObj;
			}

			const float overallFraction = consumed + (1.0f - consumed) * segFraction;

			bool actorHit = false;
			if (obj) {
				if (!selfRootsFetched) {
					selfRootA = player->Get3D(false);
					selfRootB = player->Get3D(true);
					selfRootsFetched = true;
				}
				actorHit = IsActorHit(obj, selfRootA, selfRootB);
			}
			const bool helperHit = !IsSolidLayer(layer);
			const bool tooClose  = (overallFraction * segLen) < kMinPlausibleDistance;
			if (actorHit || helperHit || tooClose) {
				consumed = overallFraction + epsFrac;
				if (consumed >= 1.0f) return false;
				continue;
			}

			a_out.hit = true;
			a_out.distance = overallFraction * segLen;
			a_out.point = {
				a_from.x + seg.x * overallFraction,
				a_from.y + seg.y * overallFraction,
				a_from.z + seg.z * overallFraction
			};
			a_out.normal = normal;
			a_out.layer = layer;
			a_out.obj = obj;
			return true;
		}
		// Three skipped hits in a row — treat as no solid geometry.
		return false;
	}

	bool CastDir(const RE::NiPoint3& a_start, const RE::NiPoint3& a_dir, float a_maxDist, RayHit& a_out)
	{
		const RE::NiPoint3 end{
			a_start.x + a_dir.x * a_maxDist,
			a_start.y + a_dir.y * a_maxDist,
			a_start.z + a_dir.z * a_maxDist
		};
		return Cast(a_start, end, a_out);
	}
}
