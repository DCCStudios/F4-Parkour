#include "PCH.h"
#include "AuthoredCurve.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace
{
	constexpr const char* kMantleHighPath =
		"Data/F4SE/Plugins/F4Parkour/Curves/mantle_high.curve.json";

	bool ParseCurve(const char* a_path, F4Parkour::AuthoredCurve& a_out)
	{
		std::ifstream file(a_path);
		if (!file.is_open()) return false;

		json j;
		try {
			file >> j;
		} catch (...) {
			logger::warn("[AuthoredCurve] '{}' is not valid JSON", a_path);
			return false;
		}

		try {
			a_out.duration = j.at("duration").get<float>();
			const auto& end = j.at("nominalEnd");
			const float ex = end.at(0).get<float>();
			const float ey = end.at(1).get<float>();
			const float ez = end.at(2).get<float>();

			a_out.nominalFwd = std::sqrt(ex * ex + ey * ey);
			a_out.nominalUp = ez;
			if (a_out.duration < 0.2f || a_out.nominalUp < 1.0e-3f) {
				logger::warn("[AuthoredCurve] '{}' degenerate (duration={:.2f}, rise={:.2f})",
					a_path, a_out.duration, a_out.nominalUp);
				return false;
			}

			// "Forward" is wherever the animation ended up horizontally; a
			// (nearly) straight-up climb has no forward axis, so horizontal
			// progress follows the rise instead.
			const bool hasFwd = a_out.nominalFwd > 1.0e-3f;
			const float fx = hasFwd ? ex / a_out.nominalFwd : 0.0f;
			const float fy = hasFwd ? ey / a_out.nominalFwd : 0.0f;

			a_out.samples.clear();
			for (const auto& s : j.at("samples")) {
				const auto& r = s.at("root");
				const float x = r.at(0).get<float>();
				const float y = r.at(1).get<float>();
				const float z = r.at(2).get<float>();
				F4Parkour::AuthoredCurve::Sample out;
				out.t = s.at("t").get<float>();
				out.up = z / a_out.nominalUp;
				out.fwd = hasFwd ? (x * fx + y * fy) / a_out.nominalFwd
				                 : std::clamp(out.up, 0.0f, 1.0f);
				out.lat = hasFwd ? (x * fy - y * fx) : 0.0f;
				a_out.samples.push_back(out);
			}
		} catch (const std::exception& e) {
			logger::warn("[AuthoredCurve] '{}' schema error: {}", a_path, e.what());
			return false;
		}

		if (a_out.samples.size() < 2) {
			logger::warn("[AuthoredCurve] '{}' has fewer than 2 samples", a_path);
			return false;
		}

		// Apex = the highest authored point; the mover's commit window and
		// endpoint-refinement glide key off this fraction.
		float bestUp = a_out.samples.front().up;
		float bestT = a_out.samples.front().t;
		for (const auto& s : a_out.samples) {
			if (s.up > bestUp) {
				bestUp = s.up;
				bestT = s.t;
			}
		}
		a_out.apexS = std::clamp(bestT / a_out.duration, 0.15f, 0.95f);
		return true;
	}
}

namespace F4Parkour
{
	AuthoredCurve::Sample AuthoredCurve::At(float a_time) const
	{
		if (samples.empty()) return {};
		if (a_time <= samples.front().t) return samples.front();
		if (a_time >= samples.back().t) return samples.back();

		// Samples are uniformly spaced by the exporter, but a linear scan
		// stays correct if they ever are not; typical curves are ~80 frames.
		for (std::size_t i = 1; i < samples.size(); ++i) {
			if (a_time <= samples[i].t) {
				const Sample& a = samples[i - 1];
				const Sample& b = samples[i];
				const float span = b.t - a.t;
				const float f = span > 1.0e-6f ? (a_time - a.t) / span : 1.0f;
				Sample out;
				out.t = a_time;
				out.fwd = a.fwd + (b.fwd - a.fwd) * f;
				out.lat = a.lat + (b.lat - a.lat) * f;
				out.up = a.up + (b.up - a.up) * f;
				return out;
			}
		}
		return samples.back();
	}

	const AuthoredCurve* AuthoredCurves::MantleHigh()
	{
		// Hot reload keyed on mtime so the user can re-export from Blender
		// and test the very next mantle without restarting the game.
		static AuthoredCurve s_curve;
		static bool s_ok = false;
		static std::filesystem::file_time_type s_loadedStamp{};
		static bool s_loggedMissing = false;

		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(kMantleHighPath, ec);
		if (ec) {
			if (!s_loggedMissing) {
				logger::info("[AuthoredCurve] no '{}' - high mantles use the procedural arcs",
					kMantleHighPath);
				s_loggedMissing = true;
			}
			s_ok = false;
			return nullptr;
		}
		s_loggedMissing = false;

		if (!s_ok || stamp != s_loadedStamp) {
			s_loadedStamp = stamp;
			s_ok = ParseCurve(kMantleHighPath, s_curve);
			if (s_ok) {
				logger::info(
					"[AuthoredCurve] loaded '{}': {} samples, {:.2f}s, rise={:.2f}u fwd={:.2f}u (exporter units), apexS={:.2f}",
					kMantleHighPath, s_curve.samples.size(), s_curve.duration,
					s_curve.nominalUp, s_curve.nominalFwd, s_curve.apexS);
			}
		}
		return s_ok ? &s_curve : nullptr;
	}
}
