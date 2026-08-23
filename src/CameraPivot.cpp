#include "PCH.h"
#include "CameraPivot.h"
#include "Raycast.h"

// ============================================================
// Ported from FPCameraOverhaul's CameraSettle FP-skeleton path:
// insert an NiNode between the FP "Camera" bone and its parent,
// write local.rotate + a rotate-in-place translate on it. All the
// empirical conventions below were verified in game by that mod —
// do not "fix" them without retesting:
//   * EulerToMatrix(a_pitch,...) puts a_pitch in the Rx slot and
//     positive a_pitch reads as pitch DOWN after the -x negation
//     FPCameraOverhaul applies for its "+ = up" sliders. Our dip is
//     "+ = down", so it feeds the Rx slot unnegated.
//   * NiMatrix3 rows are local basis axes in parent coords: local ->
//     parent transforms use M^T * v. The rotate-in-place pivot is
//     therefore T = q - R^T*q (using R*q over-rotates).
// ============================================================
namespace
{
	constexpr const char* kInsertedBoneName = "F4Parkour_CameraInserted1st";
	constexpr const char* kPivotBoneName = "Camera";
	constexpr float kDegToRad = 3.14159265f / 180.0f;

	RE::NiMatrix3 EulerToMatrix(float a_pitch, float a_roll, float a_yaw)
	{
		const float cx = std::cos(a_pitch);
		const float sx = std::sin(a_pitch);
		const float cy = std::cos(a_yaw);
		const float sy = std::sin(a_yaw);
		const float cz = std::cos(a_roll);
		const float sz = std::sin(a_roll);

		RE::NiMatrix3 result;
		result.entry[0][0] = cy * cz;
		result.entry[0][1] = -cy * sz;
		result.entry[0][2] = sy;
		result.entry[0][3] = 0.0f;
		result.entry[1][0] = sx * sy * cz + cx * sz;
		result.entry[1][1] = -sx * sy * sz + cx * cz;
		result.entry[1][2] = -sx * cy;
		result.entry[1][3] = 0.0f;
		result.entry[2][0] = -cx * sy * cz + sx * sz;
		result.entry[2][1] = cx * sy * sz + sx * cz;
		result.entry[2][2] = cx * cy;
		result.entry[2][3] = 0.0f;
		return result;
	}

	RE::NiMatrix3 Transpose(const RE::NiMatrix3& a_matrix)
	{
		RE::NiMatrix3 result;
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				result.entry[r][c] = a_matrix.entry[c][r];
			}
			result.entry[r][3] = 0.0f;
		}
		return result;
	}

	RE::NiPoint3 MatMulPoint(const RE::NiMatrix3& m, const RE::NiPoint3& p)
	{
		return {
			m.entry[0][0] * p.x + m.entry[0][1] * p.y + m.entry[0][2] * p.z,
			m.entry[1][0] * p.x + m.entry[1][1] * p.y + m.entry[1][2] * p.z,
			m.entry[2][0] * p.x + m.entry[2][1] * p.y + m.entry[2][2] * p.z
		};
	}

	bool IsFirstPerson()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->currentState) return false;
		const auto id = camera->currentState->id.get();
		return id == RE::CameraStates::kFirstPerson || id == RE::CameraStates::kIronSights;
	}

	// Find (or insert) our node above the FP Camera bone. Name lookups
	// every call: this only runs during the ~1s a move is active, and
	// pointer caching across game loads is exactly the class of stale-
	// pointer bug this module doesn't need.
	//
	// Safe insertion (FPCameraOverhaul recipe): pre-detach the pivot via
	// the NiPointer overload to keep it alive while its slot frees, so
	// AttachChild can never drop its refcount to 0 mid-insertion.
	RE::NiNode* FindOrInsertPivot(RE::PlayerCharacter* a_player, bool a_createIfMissing)
	{
		auto* fp3D = a_player ? a_player->Get3D(true) : nullptr;
		auto* fpRoot = fp3D ? fp3D->IsNode() : nullptr;
		if (!fpRoot) return nullptr;

		const RE::BSFixedString insertedName{ kInsertedBoneName };
		if (auto* existing = fpRoot->GetObjectByName(insertedName)) {
			if (auto* node = existing->IsNode()) {
				return node;
			}
		}
		if (!a_createIfMissing) return nullptr;

		const RE::BSFixedString pivotName{ kPivotBoneName };
		RE::NiNode* pivotBone = nullptr;
		if (auto* obj = fpRoot->GetObjectByName(pivotName)) {
			pivotBone = obj->IsNode();
		}
		if (!pivotBone) return nullptr;

		auto* inserted = new RE::NiNode(1);
		if (!inserted) return nullptr;
		inserted->name = insertedName;
		inserted->local.translate = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
		inserted->local.rotate.MakeIdentity();

		RE::NiNode* parent = pivotBone->parent;
		if (parent) {
			RE::NiPointer<RE::NiAVObject> pivotRef;
			parent->DetachChild(pivotBone, pivotRef);  // detach + keep alive
			pivotBone->parent = nullptr;               // prevent double-detach
			parent->AttachChild(inserted, true);
			inserted->parent = parent;
		}
		inserted->AttachChild(pivotBone, true);

		logger::info("[CameraPivot] Inserted '{}' above '{}' in FP skeleton", kInsertedBoneName, kPivotBoneName);
		return inserted;
	}
}

namespace F4Parkour::CameraPivot
{
	void SetPitchDeg(RE::PlayerCharacter* a_player, float a_degrees)
	{
		if (!IsFirstPerson()) return;
		if (std::fabs(a_degrees) < 0.01f) {
			Clear(a_player);
			return;
		}

		auto* inserted = FindOrInsertPivot(a_player, true);
		if (!inserted) return;

		// + degrees = look down: feeds the Rx slot unnegated (see header).
		const RE::NiMatrix3 R = EulerToMatrix(a_degrees * kDegToRad, 0.0f, 0.0f);

		// Rotate in place around the Camera bone's own position.
		RE::NiPoint3 q{ 0.0f, 0.0f, 0.0f };
		const RE::BSFixedString pivotName{ kPivotBoneName };
		if (auto* cam = inserted->GetObjectByName(pivotName)) {
			q = cam->local.translate;
		}
		const RE::NiMatrix3 Rt = Transpose(R);
		const RE::NiPoint3 rotatedQ = MatMulPoint(Rt, q);
		inserted->local.translate = { q.x - rotatedQ.x, q.y - rotatedQ.y, q.z - rotatedQ.z };
		inserted->local.rotate = R;
	}

	void Collision(RE::PlayerCharacter* a_player, float a_skin)
	{
		if (!IsFirstPerson() || !a_player) return;
		auto* inserted = FindOrInsertPivot(a_player, true);
		if (!inserted || !inserted->parent) return;

		// The view point is the FP "Camera" bone (now a child of the
		// inserted node). Read its world position.
		const RE::BSFixedString pivotName{ kPivotBoneName };
		auto* camObj = inserted->GetObjectByName(pivotName);
		if (!camObj) return;
		const RE::NiPoint3 camWorld = camObj->world.translate;

		// Anchor: chest height above the feet — provably inside the capsule
		// and therefore clear of the geometry we are climbing.
		const RE::NiPoint3 anchor{
			a_player->data.location.x,
			a_player->data.location.y,
			a_player->data.location.z + 40.0f
		};

		RE::NiPoint3 delta{ camWorld.x - anchor.x, camWorld.y - anchor.y, camWorld.z - anchor.z };
		const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

		RE::NiPoint3 targetLocal{ 0.0f, 0.0f, 0.0f };
		if (len > 1.0f) {
			const RE::NiPoint3 dir{ delta.x / len, delta.y / len, delta.z / len };
			Raycast::RayHit hit{};
			if (Raycast::CastDir(anchor, dir, len + a_skin, hit) && hit.hit && hit.distance < len) {
				// Pull the view in toward the anchor so it sits a skin's
				// width off the surface. World offset, then expressed in the
				// inserted node's PARENT frame (this codebase's NiMatrix3
				// convention: world->parentFrame = M * v, see header).
				const float pull = (len - hit.distance) + a_skin;
				const RE::NiPoint3 worldOffset{ -dir.x * pull, -dir.y * pull, -dir.z * pull };
				targetLocal = MatMulPoint(inserted->parent->world.rotate, worldOffset);
			}
		}

		// Ease toward the target offset so the view never pops.
		const RE::NiPoint3 cur = inserted->local.translate;
		const float k = 0.35f;
		inserted->local.translate = {
			cur.x + (targetLocal.x - cur.x) * k,
			cur.y + (targetLocal.y - cur.y) * k,
			cur.z + (targetLocal.z - cur.z) * k
		};
		inserted->local.rotate.MakeIdentity();
	}

	void Clear(RE::PlayerCharacter* a_player)
	{
		auto* inserted = FindOrInsertPivot(a_player, false);
		if (!inserted) return;
		inserted->local.translate = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
		inserted->local.rotate.MakeIdentity();
	}
}
