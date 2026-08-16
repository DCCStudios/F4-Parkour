#include "PCH.h"
#include "Curves.h"

#include <nlohmann/json.hpp>

#include <fstream>

using json = nlohmann::json;

namespace
{
	constexpr const char* kPresetDir = "Data/F4SE/Plugins/F4Parkour/Presets";

	// Catmull-Rom segment interpolation with duplicated end tangents.
	float CatmullRom(float p0, float p1, float p2, float p3, float t)
	{
		const float t2 = t * t;
		const float t3 = t2 * t;
		return 0.5f * ((2.0f * p1) +
			(-p0 + p2) * t +
			(2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
			(-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
	}

	json CurveToJson(const F4Parkour::Curve& a_curve)
	{
		json arr = json::array();
		for (const auto& p : a_curve.points) {
			arr.push_back({ p[0], p[1] });
		}
		return arr;
	}

	bool CurveFromJson(const json& a_json, F4Parkour::Curve& a_out)
	{
		if (!a_json.is_array()) return false;
		std::vector<std::array<float, 2>> pts;
		for (const auto& e : a_json) {
			if (!e.is_array() || e.size() < 2) continue;
			if (!e[0].is_number() || !e[1].is_number()) continue;
			pts.push_back({ e[0].get<float>(), e[1].get<float>() });
		}
		if (pts.size() < 2) return false;
		a_out.points = std::move(pts);
		return true;
	}

	template <std::size_t N>
	void ArrayFromJson(const json& a_json, std::array<float, N>& a_out)
	{
		if (!a_json.is_array() || a_json.size() != N) return;
		for (std::size_t i = 0; i < N; ++i) {
			if (a_json[i].is_number()) a_out[i] = a_json[i].get<float>();
		}
	}
}

namespace F4Parkour
{
	// ============================================================
	// Curve
	// ============================================================
	float Curve::Evaluate(float a_x) const
	{
		const auto n = points.size();
		if (n == 0) return 0.0f;
		if (n == 1) return points[0][1];

		a_x = std::clamp(a_x, points.front()[0], points.back()[0]);

		// Find the segment containing a_x.
		std::size_t i = 0;
		while (i + 2 < n && a_x > points[i + 1][0]) ++i;

		const float x1 = points[i][0];
		const float x2 = points[i + 1][0];
		const float span = x2 - x1;
		const float t = (span > 1e-6f) ? (a_x - x1) / span : 0.0f;

		const float p1 = points[i][1];
		const float p2 = points[i + 1][1];
		const float p0 = (i > 0) ? points[i - 1][1] : p1;
		const float p3 = (i + 2 < n) ? points[i + 2][1] : p2;

		return CatmullRom(p0, p1, p2, p3, t);
	}

	void Curve::Sanitize(bool a_monotonic, float a_yMin, float a_yMax)
	{
		if (points.size() < 2) {
			points = { { 0.0f, a_yMin }, { 1.0f, a_yMax } };
		}
		std::sort(points.begin(), points.end(),
			[](const auto& a, const auto& b) { return a[0] < b[0]; });

		for (auto& p : points) {
			p[0] = std::clamp(p[0], 0.0f, 1.0f);
			p[1] = std::clamp(p[1], a_yMin, a_yMax);
		}
		points.front()[0] = 0.0f;
		points.back()[0] = 1.0f;

		if (a_monotonic) {
			for (std::size_t i = 1; i < points.size(); ++i) {
				if (points[i][1] < points[i - 1][1]) {
					points[i][1] = points[i - 1][1];
				}
			}
		}
	}

	// ============================================================
	// FeelPreset
	// ============================================================
	FeelPreset::FeelPreset()
	{
		// Smooth defaults. Vault arcs: rise, cross, descend to the far
		// side. Mantle arcs: rise and settle on top, slowing near the
		// crest (Brink's climb rule — the pull-up decelerates; it reads
		// as weight, not lag). Higher tiers peak slightly later: the body
		// gathers before a bigger climb.
		vaultArc[0].points  = { { 0.0f, 0.0f }, { 0.40f, 1.0f }, { 0.70f, 0.95f }, { 1.0f, 0.1f } };
		vaultArc[1].points  = { { 0.0f, 0.0f }, { 0.45f, 1.0f }, { 0.72f, 0.95f }, { 1.0f, 0.1f } };
		vaultArc[2].points  = { { 0.0f, 0.0f }, { 0.50f, 1.0f }, { 0.75f, 0.95f }, { 1.0f, 0.1f } };
		mantleArc[0].points = { { 0.0f, 0.0f }, { 0.55f, 0.90f }, { 1.0f, 1.0f } };
		mantleArc[1].points = { { 0.0f, 0.0f }, { 0.60f, 0.92f }, { 1.0f, 1.0f } };
		mantleArc[2].points = { { 0.0f, 0.0f }, { 0.30f, 0.45f }, { 0.65f, 0.93f }, { 1.0f, 1.0f } };
		cameraDip.points    = { { 0.0f, 0.0f }, { 0.15f, 1.5f }, { 0.5f, -0.5f }, { 1.0f, 0.0f } };
	}

	void FeelPreset::TierBlend(bool a_vault, float a_height, int& a_lo, int& a_hi, float& a_t) const
	{
		const auto& heights = a_vault ? vaultHeights : mantleHeights;
		if (a_height <= heights[0]) { a_lo = a_hi = 0; a_t = 0.0f; return; }
		if (a_height >= heights[2]) { a_lo = a_hi = 2; a_t = 0.0f; return; }
		a_hi = (a_height <= heights[1]) ? 1 : 2;
		a_lo = a_hi - 1;
		const float span = heights[a_hi] - heights[a_lo];
		a_t = (span > 1e-3f) ? (a_height - heights[a_lo]) / span : 0.0f;
	}

	float FeelPreset::DurationFor(bool a_vault, float a_height) const
	{
		const auto& durations = a_vault ? vaultDurations : mantleDurations;
		int lo = 0, hi = 0;
		float t = 0.0f;
		TierBlend(a_vault, a_height, lo, hi, t);
		return durations[lo] + (durations[hi] - durations[lo]) * t;
	}

	float FeelPreset::ArcAt(bool a_vault, float a_height, float a_x) const
	{
		const Curve* arcs = a_vault ? vaultArc : mantleArc;
		int lo = 0, hi = 0;
		float t = 0.0f;
		TierBlend(a_vault, a_height, lo, hi, t);
		const float yLo = arcs[lo].Evaluate(a_x);
		return (lo == hi) ? yLo : yLo + (arcs[hi].Evaluate(a_x) - yLo) * t;
	}

	float FeelPreset::EasedProgress(bool a_vault, float a_t) const
	{
		const float ease = a_vault ? vaultEase : mantleEase;
		const float tt = std::clamp(a_t, 0.0f, 1.0f);
		const float smooth = tt * tt * (3.0f - 2.0f * tt);  // smoothstep
		return tt + (smooth - tt) * std::clamp(ease, 0.0f, 1.0f);
	}

	void FeelPreset::Sanitize()
	{
		speedMult = std::clamp(speedMult, 0.25f, 4.0f);
		sprintDurationScale = std::clamp(sprintDurationScale, 0.4f, 1.5f);
		apexClearance = std::clamp(apexClearance, 0.0f, 40.0f);

		auto fixTiers = [](std::array<float, 3>& a_h, float a_min, float a_max) {
			for (auto& h : a_h) h = std::clamp(h, a_min, a_max);
			std::sort(a_h.begin(), a_h.end());
			// Keep tiers separated so blending never divides by ~0.
			if (a_h[1] < a_h[0] + 5.0f) a_h[1] = a_h[0] + 5.0f;
			if (a_h[2] < a_h[1] + 5.0f) a_h[2] = a_h[1] + 5.0f;
		};
		fixTiers(vaultHeights, 20.0f, 200.0f);
		fixTiers(mantleHeights, 30.0f, 200.0f);

		for (auto& d : vaultDurations) d = std::clamp(d, 0.15f, 3.0f);
		for (auto& d : mantleDurations) d = std::clamp(d, 0.15f, 3.0f);

		// Arcs are free shapes within 0..1 height (the runtime pins the
		// endpoint to the real landing); easing is a slider so progress
		// stays monotonic by construction (comfort guardrail).
		for (int i = 0; i < 3; ++i) {
			vaultArc[i].Sanitize(false, 0.0f, 1.0f);
			mantleArc[i].Sanitize(false, 0.0f, 1.0f);
		}
		vaultEase = std::clamp(vaultEase, 0.0f, 1.0f);
		mantleEase = std::clamp(mantleEase, 0.0f, 1.0f);
		cameraDip.Sanitize(false, -10.0f, 10.0f);
	}

	// ============================================================
	// JSON round trip
	// ============================================================
	static json PresetToJson(const FeelPreset& a_p)
	{
		json j;
		j["name"] = a_p.name;
		j["description"] = a_p.description;
		j["global"] = {
			{ "speedMult", a_p.speedMult },
			{ "sprintDurationScale", a_p.sprintDurationScale },
			{ "apexClearance", a_p.apexClearance },
			{ "cameraDipEnabled", a_p.cameraDipEnabled },
		};
		j["heights"] = {
			{ "vault", a_p.vaultHeights },
			{ "mantle", a_p.mantleHeights },
		};
		j["durations"] = {
			{ "vault", a_p.vaultDurations },
			{ "mantle", a_p.mantleDurations },
		};
		j["easing"] = {
			{ "vault", a_p.vaultEase },
			{ "mantle", a_p.mantleEase },
		};
		j["curves"] = {
			{ "vault.arc.low", CurveToJson(a_p.vaultArc[0]) },
			{ "vault.arc.mid", CurveToJson(a_p.vaultArc[1]) },
			{ "vault.arc.high", CurveToJson(a_p.vaultArc[2]) },
			{ "mantle.arc.low", CurveToJson(a_p.mantleArc[0]) },
			{ "mantle.arc.mid", CurveToJson(a_p.mantleArc[1]) },
			{ "mantle.arc.high", CurveToJson(a_p.mantleArc[2]) },
			{ "camera.dip", CurveToJson(a_p.cameraDip) },
		};
		return j;
	}

	static bool PresetFromJson(const json& a_j, FeelPreset& a_out)
	{
		if (!a_j.is_object()) return false;
		FeelPreset p{};

		if (a_j.contains("name") && a_j["name"].is_string()) p.name = a_j["name"].get<std::string>();
		if (a_j.contains("description") && a_j["description"].is_string()) p.description = a_j["description"].get<std::string>();

		if (a_j.contains("global") && a_j["global"].is_object()) {
			const auto& g = a_j["global"];
			if (g.contains("speedMult") && g["speedMult"].is_number()) p.speedMult = g["speedMult"].get<float>();
			if (g.contains("sprintDurationScale") && g["sprintDurationScale"].is_number()) p.sprintDurationScale = g["sprintDurationScale"].get<float>();
			if (g.contains("apexClearance") && g["apexClearance"].is_number()) p.apexClearance = g["apexClearance"].get<float>();
			if (g.contains("cameraDipEnabled") && g["cameraDipEnabled"].is_boolean()) p.cameraDipEnabled = g["cameraDipEnabled"].get<bool>();
		}
		if (a_j.contains("heights") && a_j["heights"].is_object()) {
			const auto& h = a_j["heights"];
			if (h.contains("vault")) ArrayFromJson(h["vault"], p.vaultHeights);
			if (h.contains("mantle")) ArrayFromJson(h["mantle"], p.mantleHeights);
		}
		if (a_j.contains("durations") && a_j["durations"].is_object()) {
			const auto& d = a_j["durations"];
			if (d.contains("vault")) ArrayFromJson(d["vault"], p.vaultDurations);
			if (d.contains("mantle")) ArrayFromJson(d["mantle"], p.mantleDurations);
		}
		if (a_j.contains("easing") && a_j["easing"].is_object()) {
			const auto& e = a_j["easing"];
			if (e.contains("vault") && e["vault"].is_number()) p.vaultEase = e["vault"].get<float>();
			if (e.contains("mantle") && e["mantle"].is_number()) p.mantleEase = e["mantle"].get<float>();
		}
		if (a_j.contains("curves") && a_j["curves"].is_object()) {
			const auto& c = a_j["curves"];
			auto load = [&](const char* a_key, Curve& a_curve) {
				if (c.contains(a_key)) CurveFromJson(c[a_key], a_curve);
			};
			load("vault.arc.low", p.vaultArc[0]);
			load("vault.arc.mid", p.vaultArc[1]);
			load("vault.arc.high", p.vaultArc[2]);
			load("mantle.arc.low", p.mantleArc[0]);
			load("mantle.arc.mid", p.mantleArc[1]);
			load("mantle.arc.high", p.mantleArc[2]);
			load("camera.dip", p.cameraDip);

			// Legacy (pre-tier) preset migration: the old vertical curves
			// seed all three tiers so user presets keep their feel.
			if (!c.contains("vault.arc.low") && c.contains("vault.vertical")) {
				Curve legacy{};
				if (CurveFromJson(c["vault.vertical"], legacy)) {
					p.vaultArc[0] = legacy;
					p.vaultArc[1] = legacy;
					p.vaultArc[2] = legacy;
				}
			}
			if (!c.contains("mantle.arc.low") && c.contains("mantle.vertical")) {
				Curve legacy{};
				if (CurveFromJson(c["mantle.vertical"], legacy)) {
					p.mantleArc[0] = legacy;
					p.mantleArc[1] = legacy;
					p.mantleArc[2] = legacy;
				}
			}
		}

		p.Sanitize();
		a_out = std::move(p);
		return true;
	}

	// ============================================================
	// Presets store
	// ============================================================
	void Presets::Init(const std::string& a_activeName)
	{
		std::error_code ec;
		std::filesystem::create_directories(kPresetDir, ec);

		WriteShippedPresets();
		RefreshList();

		if (!a_activeName.empty() && LoadPreset(a_activeName)) {
			logger::info("[Presets] Loaded active preset '{}'", a_activeName);
		} else if (LoadPreset("Smooth")) {
			logger::info("[Presets] Loaded default preset 'Smooth'");
		} else {
			active = FeelPreset{};
			active.Sanitize();
			logger::warn("[Presets] No preset files readable - using built-in defaults");
		}
		hasUnsavedChanges = false;
	}

	void Presets::RefreshList()
	{
		names.clear();
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(kPresetDir, ec)) {
			if (!entry.is_regular_file()) continue;
			auto path = entry.path();
			if (path.extension() != ".json") continue;
			names.push_back(path.stem().string());
		}
		std::sort(names.begin(), names.end());
	}

	bool Presets::LoadPreset(const std::string& a_name)
	{
		const auto path = std::filesystem::path(kPresetDir) / (a_name + ".json");
		std::ifstream in(path);
		if (!in.is_open()) return false;

		json j;
		try {
			in >> j;
		} catch (const std::exception& e) {
			logger::warn("[Presets] Failed to parse '{}': {}", a_name, e.what());
			return false;
		}

		FeelPreset p{};
		if (!PresetFromJson(j, p)) return false;
		p.name = a_name;
		active = std::move(p);
		hasUnsavedChanges = false;
		return true;
	}

	bool Presets::SavePreset(const std::string& a_name)
	{
		if (a_name.empty()) return false;
		std::error_code ec;
		std::filesystem::create_directories(kPresetDir, ec);

		FeelPreset copy = active;
		copy.name = a_name;
		copy.Sanitize();

		const auto path = std::filesystem::path(kPresetDir) / (a_name + ".json");
		std::ofstream out(path);
		if (!out.is_open()) {
			logger::warn("[Presets] Cannot write '{}'", path.string());
			return false;
		}
		out << PresetToJson(copy).dump(2);
		logger::info("[Presets] Saved '{}' -> {}", a_name,
			std::filesystem::absolute(path, ec).string());
		active.name = a_name;
		hasUnsavedChanges = false;
		RefreshList();
		return true;
	}

	bool Presets::CopyPreset(const std::string& a_source, const std::string& a_dest)
	{
		if (a_source.empty() || a_dest.empty() || a_source == a_dest) return false;
		const auto srcPath = std::filesystem::path(kPresetDir) / (a_source + ".json");
		const auto dstPath = std::filesystem::path(kPresetDir) / (a_dest + ".json");
		std::error_code ec;
		if (!std::filesystem::exists(srcPath, ec)) return false;
		if (std::filesystem::exists(dstPath, ec)) return false;  // never clobber

		// Round-trip through the parser so the copy carries the new name
		// inside the file (and gets sanitized), not just a new filename.
		std::ifstream in(srcPath);
		if (!in.is_open()) return false;
		json j;
		try {
			in >> j;
		} catch (const std::exception&) {
			return false;
		}
		FeelPreset p{};
		if (!PresetFromJson(j, p)) return false;
		p.name = a_dest;

		std::ofstream out(dstPath);
		if (!out.is_open()) return false;
		out << PresetToJson(p).dump(2);
		RefreshList();
		logger::info("[Presets] Copied preset '{}' -> '{}'", a_source, a_dest);
		return true;
	}

	bool Presets::DeletePreset(const std::string& a_name)
	{
		const auto path = std::filesystem::path(kPresetDir) / (a_name + ".json");
		std::error_code ec;
		const bool ok = std::filesystem::remove(path, ec) && !ec;
		RefreshList();
		return ok;
	}

	void Presets::WriteShippedPresets()
	{
		struct Shipped
		{
			const char* name;
			const char* description;
			float speedMult, sprintScale, apex;
			std::array<float, 3> vaultDur, mantleDur;
		};
		// All four share tier heights; they differ in pacing and feel.
		static constexpr Shipped kShipped[] = {
			{ "Smooth",     "Long, floaty arcs. Forgiving timing.",                 1.0f,  0.85f, 8.0f,
				{ 0.50f, 0.65f, 0.85f }, { 0.60f, 0.85f, 1.15f } },
			{ "Snappy",     "Short, hard-easing moves. Modern-shooter feel.",       1.15f, 0.80f, 6.0f,
				{ 0.35f, 0.45f, 0.60f }, { 0.45f, 0.60f, 0.85f } },
			{ "Deliberate", "Slower, weighty climbs with a pronounced camera dip.", 0.85f, 0.95f, 10.0f,
				{ 0.60f, 0.80f, 1.05f }, { 0.75f, 1.05f, 1.45f } },
			{ "Arcade",     "Fast flowing parkour. Strong momentum carry.",         1.30f, 0.70f, 6.0f,
				{ 0.30f, 0.40f, 0.55f }, { 0.40f, 0.55f, 0.75f } },
		};

		for (const auto& s : kShipped) {
			const auto path = std::filesystem::path(kPresetDir) / (std::string(s.name) + ".json");
			std::error_code ec;
			if (std::filesystem::exists(path, ec)) continue;  // never clobber user edits

			FeelPreset p{};
			p.name = s.name;
			p.description = s.description;
			p.speedMult = s.speedMult;
			p.sprintDurationScale = s.sprintScale;
			p.apexClearance = s.apex;
			p.vaultDurations = s.vaultDur;
			p.mantleDurations = s.mantleDur;
			if (std::string_view(s.name) == "Snappy" || std::string_view(s.name) == "Arcade") {
				// Snappier pacing: closer to constant speed.
				p.vaultEase = 0.25f;
				p.mantleEase = 0.35f;
			}
			if (std::string_view(s.name) == "Deliberate") {
				p.cameraDip.points = { { 0.0f, 0.0f }, { 0.2f, 2.5f }, { 0.55f, -1.0f }, { 1.0f, 0.0f } };
			}
			p.Sanitize();

			std::ofstream out(path);
			if (out.is_open()) {
				out << PresetToJson(p).dump(2);
			}
		}
	}
}
