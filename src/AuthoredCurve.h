#pragma once

#include <vector>

namespace F4Parkour
{
	// ============================================================
	// One offline-exported animation trajectory (see
	// tools/export_root_curve.py): the animation's root-bone displacement
	// per frame, decomposed at load into (forward fraction, lateral, up
	// fraction) so the mover can map it onto measured geometry — forward
	// scaled to the measured run, up to the measured rise. The exporter's
	// FINAL displacement defines "forward", so any Blender rig orientation
	// and any unit scale work: both cancel in the fractions.
	//
	// This is the authored-curve mantle architecture: the same curve the
	// animation was built on drives the player, so code and animation
	// cannot disagree (the STALKER ledge-grab model).
	// ============================================================
	struct AuthoredCurve
	{
		struct Sample
		{
			float t{ 0.0f };    // seconds from move start
			float fwd{ 0.0f };  // 0..1 fraction of the nominal forward run
			float lat{ 0.0f };  // exporter units, style only
			float up{ 0.0f };   // fraction of the nominal rise (dips negative)
		};

		float duration{ 0.0f };
		float nominalFwd{ 0.0f };  // exporter units (for logs / lat scaling)
		float nominalUp{ 0.0f };
		float apexS{ 0.9f };       // time fraction of the highest point
		std::vector<Sample> samples;

		// Linear interpolation at a_time seconds, clamped to the ends.
		Sample At(float a_time) const;
	};

	namespace AuthoredCurves
	{
		// The high-mantle curve, reloaded automatically when the file's
		// modification time changes (cheap stat — call at move start, never
		// per frame). Returns nullptr when the file is missing or unusable;
		// callers fall back to the procedural arcs.
		const AuthoredCurve* MantleHigh();
	}
}
