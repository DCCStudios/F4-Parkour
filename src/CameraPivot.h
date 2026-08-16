#pragma once

namespace F4Parkour
{
	// ============================================================
	// First-person camera pitch offset via an inserted NiNode above
	// the FP "Camera" bone — the FPCameraOverhaul technique. The
	// player's aim angles are NEVER touched (writing data.angle.x is
	// what caused the "camera rotates way upward" reports: that is the
	// aim pitch, and the engine + anim graph both react to it).
	//
	// The inserted node composes cleanly with FPCameraOverhaul's own
	// inserted bone when both mods are active (each searches for the
	// "Camera" bone and re-parents above it; transforms stack).
	// ============================================================
	namespace CameraPivot
	{
		// Apply a camera-space pitch offset in degrees (+ = look down).
		// First person only; no-ops silently elsewhere. Call every frame
		// while a move is active.
		void SetPitchDeg(RE::PlayerCharacter* a_player, float a_degrees);

		// Reset the inserted node to identity. Safe when the node was
		// never created (no-op).
		void Clear(RE::PlayerCharacter* a_player);
	}
}
