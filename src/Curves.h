#pragma once

#include <array>
#include <string>
#include <vector>

namespace F4Parkour
{
	// ============================================================
	// A user-editable curve: 2-8 control points in [0,1]x[0..], evaluated
	// with centripetal-free Catmull-Rom (clamped ends). x = normalized
	// time or progress, y = the curve's output.
	// ============================================================
	struct Curve
	{
		std::vector<std::array<float, 2>> points;

		float Evaluate(float a_x) const;

		// Sort by x, clamp into range, and (optionally) force the y values
		// non-decreasing — the motion-sickness guardrail for progress
		// curves (the camera must never move backward during a forward
		// move).
		void Sanitize(bool a_monotonic, float a_yMin, float a_yMax);
	};

	// ============================================================
	// A feel preset: tier heights, per-tier durations, and the shaping
	// curves. Loaded from / saved to
	// Data/F4SE/Plugins/F4Parkour/Presets/<name>.json
	// ============================================================
	struct FeelPreset
	{
		std::string name{ "Smooth" };
		std::string description{};

		// Global
		float speedMult{ 1.0f };            // global duration divisor
		float sprintDurationScale{ 0.85f }; // duration scale at sprint entry speed
		float apexClearance{ 8.0f };        // units above the ledge top at apex

		// Tier reference heights (game units, ascending)
		std::array<float, 3> vaultHeights{ 45.0f, 70.0f, 100.0f };
		std::array<float, 3> mantleHeights{ 60.0f, 105.0f, 150.0f };

		// Per-tier base durations (seconds)
		std::array<float, 3> vaultDurations{ 0.50f, 0.65f, 0.85f };
		std::array<float, 3> mantleDurations{ 0.60f, 0.85f, 1.15f };

		// THE ARC — one curve per height tier, and the whole feel model:
		//   x = normalized time over the move (0 = start, 1 = end)
		//   y = height, 0 = where you started, 1 = the apex over the ledge
		// It is literally the side-view shape of the jump. Vault arcs rise
		// then descend to the far-side landing; mantle arcs rise and stay
		// up. The runtime pins the endpoint to the real landing height, so
		// the curve's END value only shapes the final approach.
		Curve vaultArc[3];   // low / mid / high tier
		Curve mantleArc[3];

		// Speed easing along the ground path: 0 = constant speed all the
		// way, 1 = soft start and stop. Monotonic by construction (the
		// camera can never move backward — comfort guardrail preserved).
		float vaultEase{ 0.5f };
		float mantleEase{ 0.6f };

		// First-person pitch curve (feature currently disabled in-game;
		// data kept for a future pass).
		Curve cameraDip;
		bool  cameraDipEnabled{ false };

		FeelPreset();  // fills the Smooth defaults

		// Duration for a move of this kind at this ledge height, blending
		// linearly between the two nearest tiers.
		float DurationFor(bool a_vault, float a_height) const;

		// Tier blend for a height: fills lo/hi indices and the 0..1 mix.
		void TierBlend(bool a_vault, float a_height, int& a_lo, int& a_hi, float& a_t) const;

		// Blended arc sample at eased-time x for a given height.
		float ArcAt(bool a_vault, float a_height, float a_x) const;

		// Monotonic eased progress for raw time t (0..1).
		float EasedProgress(bool a_vault, float a_t) const;

		void Sanitize();
	};

	// ============================================================
	// Preset store: shipped + user presets on disk, plus the live
	// (possibly unsaved) working preset the mover reads.
	// ============================================================
	class Presets
	{
	public:
		static Presets* GetSingleton()
		{
			static Presets singleton;
			return &singleton;
		}

		// Scan the preset directory, write the shipped presets if the
		// directory is empty, and load the last-active preset.
		void Init(const std::string& a_activeName);

		// The preset the mover uses. Menu edits mutate this in place.
		FeelPreset& Active() { return active; }
		const FeelPreset& Active() const { return active; }
		// Split dirty tracking for the unified save popup: pacing edits
		// (the main <name>.json) vs curve edits (<name>.curves.json).
		bool pacingDirty{ false };
		bool curvesDirty{ false };
		bool AnyUnsaved() const { return pacingDirty || curvesDirty; }

		const std::vector<std::string>& List() const { return names; }
		void RefreshList();

		bool LoadPreset(const std::string& a_name);        // into Active()
		bool SavePreset(const std::string& a_name);        // Active() to disk (both files)
		bool SavePresetMain(const std::string& a_name);    // heights/durations/pacing/easing
		bool SavePresetCurves(const std::string& a_name);  // arc curves side file
		bool DeletePreset(const std::string& a_name);

		// Duplicate a preset file on disk (source untouched, existing
		// destinations never clobbered). The copy becomes a normal user
		// preset that can be loaded, renamed, edited, and saved.
		bool CopyPreset(const std::string& a_source, const std::string& a_dest);

	private:
		Presets() = default;

		void WriteShippedPresets();

		FeelPreset active{};
		std::vector<std::string> names;
	};
}
