#pragma once

#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace F4Parkour
{
	// ============================================================
	// Debug recorder + world-space overlay renderer.
	//
	// Detection/Mover record rays, markers, and decision text on the
	// game thread; the F4SEMF HUD callback drains a snapshot on the
	// render thread and projects it with NiCamera::WorldPtToScreenPt3.
	// ============================================================
	struct DebugRay
	{
		RE::NiPoint3 a{};
		RE::NiPoint3 b{};        // actual end (hit point or full length)
		bool         hit{ false };
		bool         passed{ true };  // did this ray's CHECK pass (colors green/red)
		char         label[40]{};
	};

	struct DebugMarker
	{
		RE::NiPoint3 pos{};
		std::uint32_t color{ 0xFF00FF00 };  // ABGR (ImGui ImU32)
		char          label[40]{};
	};

	struct DebugEvent
	{
		float       age{ 0.0f };  // seconds since recorded
		std::string text;
	};

	class DebugDraw
	{
	public:
		static DebugDraw* GetSingleton()
		{
			static DebugDraw singleton;
			return &singleton;
		}

		// ---- game thread: recording ----
		// Begin a new detection frame (clears the working ray/marker sets).
		void BeginFrame();
		void AddRay(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, bool a_hit, bool a_passed, const char* a_label);
		void AddMarker(const RE::NiPoint3& a_pos, std::uint32_t a_color, const char* a_label);
		void AddPath(const std::vector<RE::NiPoint3>& a_points);  // planned move path
		void ClearPath();
		void Event(std::string a_text);  // decision log line ("vault rejected: ...")
		void CommitFrame();              // publish the working sets for rendering
		void Tick(float a_dt);           // age the event log

		// ---- player-facing indicator (SkyParkour-style) ----
		// A small ring at the candidate ledge, rendered even with the
		// debug overlay off. Set/cleared by ParkourManager each tick.
		void SetIndicator(const RE::NiPoint3& a_pos, std::uint32_t a_color);
		void ClearIndicator();

		// ---- render thread ----
		void Render();  // draws overlays; call from the HUD callback

		// Live state text lines for the popout (set by ParkourManager).
		void SetStateText(std::string a_text);
		std::string GetStateText();
		std::vector<DebugEvent> GetEvents();

	private:
		DebugDraw() = default;

		std::mutex lock;

		// double-buffered: work* filled during a scan, shown* rendered
		std::vector<DebugRay>    workRays, shownRays;
		std::vector<DebugMarker> workMarkers, shownMarkers;
		std::vector<RE::NiPoint3> path;

		std::deque<DebugEvent> events;
		std::string stateText;

		bool        indicatorOn{ false };
		DebugMarker indicator{};
	};
}
