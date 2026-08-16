#include "PCH.h"
#include "DebugDraw.h"
#include "Settings.h"
#include "F4SEMenuFramework.h"

using namespace ImGuiMCP;

namespace
{
	// World -> normalized screen position. Same math OAR's BoneDebugViz
	// uses (worldToCam row-vector projection + perspective divide).
	bool WorldToScreenNorm(const RE::NiPoint3& a_world, float& a_nx, float& a_ny)
	{
		auto* cam = RE::Main::WorldRootCamera();
		if (!cam) {
			return false;
		}
		const auto& m = cam->worldToCam;

		const float trace = a_world.x * m[3][0] + a_world.y * m[3][1] + a_world.z * m[3][2] + m[3][3];
		if (trace <= 0.00001f) {
			return false;  // behind the camera
		}
		const float inv = 1.0f / trace;
		const float x = (a_world.x * m[0][0] + a_world.y * m[0][1] + a_world.z * m[0][2] + m[0][3]) * inv;
		const float y = (a_world.x * m[1][0] + a_world.y * m[1][1] + a_world.z * m[1][2] + m[1][3]) * inv;
		a_nx = (x + 1.0f) * 0.5f;
		a_ny = (y + 1.0f) * 0.5f;
		return true;
	}

	bool WorldToScreenPx(const RE::NiPoint3& a_world, ImVec2& a_out)
	{
		auto* io = ImGuiMCP::GetIO();
		if (!io) return false;
		float nx = 0.0f, ny = 0.0f;
		if (!WorldToScreenNorm(a_world, nx, ny)) return false;
		a_out.x = nx * io->DisplaySize.x;
		a_out.y = (1.0f - ny) * io->DisplaySize.y;
		return true;
	}

	constexpr ImU32 kColPass = IM_COL32(60, 230, 90, 220);
	constexpr ImU32 kColFail = IM_COL32(235, 70, 60, 220);
	constexpr ImU32 kColMiss = IM_COL32(150, 150, 150, 140);
	constexpr ImU32 kColPath = IM_COL32(90, 170, 255, 235);
	constexpr float kMaxEventAge = 12.0f;
	constexpr std::size_t kMaxEvents = 40;
}

namespace F4Parkour
{
	void DebugDraw::BeginFrame()
	{
		std::scoped_lock l(lock);
		workRays.clear();
		workMarkers.clear();
	}

	void DebugDraw::AddRay(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, bool a_hit, bool a_passed, const char* a_label)
	{
		std::scoped_lock l(lock);
		DebugRay r{};
		r.a = a_a;
		r.b = a_b;
		r.hit = a_hit;
		r.passed = a_passed;
		if (a_label) {
			std::snprintf(r.label, sizeof(r.label), "%s", a_label);
		}
		workRays.push_back(r);
	}

	void DebugDraw::AddMarker(const RE::NiPoint3& a_pos, std::uint32_t a_color, const char* a_label)
	{
		std::scoped_lock l(lock);
		DebugMarker m{};
		m.pos = a_pos;
		m.color = a_color;
		if (a_label) {
			std::snprintf(m.label, sizeof(m.label), "%s", a_label);
		}
		workMarkers.push_back(m);
	}

	void DebugDraw::AddPath(const std::vector<RE::NiPoint3>& a_points)
	{
		std::scoped_lock l(lock);
		path = a_points;
	}

	void DebugDraw::ClearPath()
	{
		std::scoped_lock l(lock);
		path.clear();
	}

	void DebugDraw::Event(std::string a_text)
	{
		std::scoped_lock l(lock);
		// Collapse immediate repeats so a 20 Hz reject doesn't flood the log.
		if (!events.empty() && events.front().text == a_text) {
			events.front().age = 0.0f;
			return;
		}
		events.push_front(DebugEvent{ 0.0f, std::move(a_text) });
		while (events.size() > kMaxEvents) {
			events.pop_back();
		}
	}

	void DebugDraw::CommitFrame()
	{
		std::scoped_lock l(lock);
		shownRays = workRays;
		shownMarkers = workMarkers;
	}

	void DebugDraw::Tick(float a_dt)
	{
		std::scoped_lock l(lock);
		for (auto& e : events) {
			e.age += a_dt;
		}
		while (!events.empty() && events.back().age > kMaxEventAge) {
			events.pop_back();
		}
	}

	void DebugDraw::SetStateText(std::string a_text)
	{
		std::scoped_lock l(lock);
		stateText = std::move(a_text);
	}

	std::string DebugDraw::GetStateText()
	{
		std::scoped_lock l(lock);
		return stateText;
	}

	std::vector<DebugEvent> DebugDraw::GetEvents()
	{
		std::scoped_lock l(lock);
		return { events.begin(), events.end() };
	}

	void DebugDraw::SetIndicator(const RE::NiPoint3& a_pos, std::uint32_t a_color)
	{
		std::scoped_lock l(lock);
		indicatorOn = true;
		indicator.pos = a_pos;
		indicator.color = a_color;
	}

	void DebugDraw::ClearIndicator()
	{
		std::scoped_lock l(lock);
		indicatorOn = false;
	}

	void DebugDraw::Render()
	{
		auto* settings = Settings::GetSingleton();

		// The parkour indicator is a player-facing cue (SkyParkour's HUD
		// icon equivalent), so it renders even with the debug overlay off.
		{
			bool on = false;
			DebugMarker ind{};
			{
				std::scoped_lock l(lock);
				on = indicatorOn;
				ind = indicator;
			}
			if (on) {
				if (auto* dl = ImGuiMCP::GetBackgroundDrawList()) {
					ImVec2 p{};
					if (WorldToScreenPx(ind.pos, p)) {
						ImDrawListManager::AddCircle(dl, p, 22.0f, ind.color, 28, 4.5f);
						ImDrawListManager::AddCircle(dl, p, 4.0f, ind.color, 12, 3.0f);
					}
				}
			}
		}

		if (!settings->debugEnabled) return;

		auto* drawList = ImGuiMCP::GetBackgroundDrawList();
		if (!drawList) return;

		std::vector<DebugRay>    rays;
		std::vector<DebugMarker> markers;
		std::vector<RE::NiPoint3> pathCopy;
		{
			std::scoped_lock l(lock);
			rays = shownRays;
			markers = shownMarkers;
			pathCopy = path;
		}

		if (settings->drawRays) {
			for (const auto& r : rays) {
				if (settings->drawOnlyFailed && r.passed) continue;
				ImVec2 a{}, b{};
				if (!WorldToScreenPx(r.a, a) || !WorldToScreenPx(r.b, b)) continue;
				const ImU32 col = !r.hit ? kColMiss : (r.passed ? kColPass : kColFail);
				ImDrawListManager::AddLine(drawList, a, b, col, 2.0f);
				if (r.label[0]) {
					ImDrawListManager::AddText(drawList, ImVec2{ b.x + 4.0f, b.y - 4.0f }, col, r.label);
				}
			}
		}

		for (const auto& m : markers) {
			ImVec2 p{};
			if (!WorldToScreenPx(m.pos, p)) continue;
			ImDrawListManager::AddCircle(drawList, p, 6.0f, m.color, 12, 2.0f);
			if (m.label[0]) {
				ImDrawListManager::AddText(drawList, ImVec2{ p.x + 8.0f, p.y - 8.0f }, m.color, m.label);
			}
		}

		if (settings->drawPath && pathCopy.size() >= 2) {
			for (std::size_t i = 0; i + 1 < pathCopy.size(); ++i) {
				ImVec2 a{}, b{};
				if (!WorldToScreenPx(pathCopy[i], a) || !WorldToScreenPx(pathCopy[i + 1], b)) continue;
				ImDrawListManager::AddLine(drawList, a, b, kColPath, 3.0f);
			}
		}
	}
}
