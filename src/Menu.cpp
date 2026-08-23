#include "PCH.h"
#include "Menu.h"
#include "Settings.h"
#include "Curves.h"
#include "ParkourManager.h"
#include "Mover.h"
#include "DebugDraw.h"
#include "F4SEMenuFramework.h"

using namespace ImGuiMCP;

namespace
{
	using namespace F4Parkour;

	bool s_registered = false;
	bool s_settingsDirty = false;
	MENU_WINDOW s_debugPopout{ nullptr };

	char s_presetNameBuf[96]{ "" };
	char s_copyNameBuf[96]{ "" };
	int  s_copySourceIdx{ 0 };
	std::string s_copyFeedback;

	// ---- small helpers (FPGO idioms) ----
	bool CheckboxTip(const char* a_label, bool* a_v, const char* a_tip)
	{
		const bool changed = ImGuiMCP::Checkbox(a_label, a_v);
		if (ImGuiMCP::IsItemHovered() && a_tip && a_tip[0]) {
			ImGuiMCP::SetTooltip("%s", a_tip);
		}
		if (changed) s_settingsDirty = true;
		return changed;
	}

	bool SliderTip(const char* a_label, float* a_v, float a_min, float a_max, const char* a_fmt, const char* a_tip)
	{
		const bool changed = ImGuiMCP::SliderFloat(a_label, a_v, a_min, a_max, a_fmt);
		if (ImGuiMCP::IsItemHovered() && a_tip && a_tip[0]) {
			ImGuiMCP::SetTooltip("%s", a_tip);
		}
		if (changed) s_settingsDirty = true;
		return changed;
	}

	// Preset-field sibling of SliderTip: marks the PRESET pacing dirty.
	// The easing sliders used SliderTip before, which marked the INI
	// settings dirty instead — easing edits could quietly never save.
	bool PresetSliderTip(const char* a_label, float* a_v, float a_min, float a_max, const char* a_fmt, const char* a_tip)
	{
		const bool changed = ImGuiMCP::SliderFloat(a_label, a_v, a_min, a_max, a_fmt);
		if (ImGuiMCP::IsItemHovered() && a_tip && a_tip[0]) {
			ImGuiMCP::SetTooltip("%s", a_tip);
		}
		if (changed) Presets::GetSingleton()->pacingDirty = true;
		return changed;
	}

	void SectionHeader(const char* a_title, const char* a_tip = nullptr)
	{
		ImGuiMCP::Spacing();
		ImGuiMCP::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "[ %s ]", a_title);
		if (a_tip && ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("%s", a_tip);
		}
		ImGuiMCP::Separator();
	}

	// ONE save button for the whole plugin. Opens a popup listing every
	// store with its dirty state; the user picks what to write.
	void SaveBar()
	{
		auto* settings = Settings::GetSingleton();
		auto* presets = Presets::GetSingleton();
		const bool sDirty = s_settingsDirty;
		const bool pDirty = presets->pacingDirty;
		const bool cDirty = presets->curvesDirty;
		const bool any = sDirty || pDirty || cDirty;

		static bool pickSettings = false, pickPacing = false, pickCurves = false;

		ImGuiMCP::Spacing();
		ImGuiMCP::Separator();
		if (ImGuiMCP::Button("Save...")) {
			pickSettings = sDirty;
			pickPacing = pDirty;
			pickCurves = cDirty;
			ImGuiMCP::OpenPopup("##f4psave", 0);
		}
		ImGuiMCP::SameLine();
		if (any) {
			std::string what = "Unsaved:";
			if (sDirty) what += " settings";
			if (pDirty) what += (sDirty ? ", preset" : " preset");
			if (cDirty) what += (sDirty || pDirty ? ", curves" : " curves");
			ImGuiMCP::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", what.c_str());
		} else {
			ImGuiMCP::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "All saved");
		}

		if (ImGuiMCP::BeginPopup("##f4psave", 0)) {
			auto& p = presets->Active();
			ImGuiMCP::Text("Save what?");
			ImGuiMCP::Separator();

			auto row = [](const char* a_label, bool a_dirty, bool* a_pick) {
				ImGuiMCP::Checkbox(a_label, a_pick);
				ImGuiMCP::SameLine();
				if (a_dirty) {
					ImGuiMCP::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(unsaved)");
				} else {
					ImGuiMCP::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "(saved)");
				}
			};
			row("Plugin settings (INI)", sDirty, &pickSettings);
			const std::string pacingLabel = std::format("Preset '{}' heights & pacing", p.name);
			row(pacingLabel.c_str(), pDirty, &pickPacing);
			const std::string curvesLabel = std::format("Preset '{}' curves", p.name);
			row(curvesLabel.c_str(), cDirty, &pickCurves);

			ImGuiMCP::Separator();
			if (ImGuiMCP::Button("Save selected")) {
				if (pickSettings) {
					settings->Save();
					s_settingsDirty = false;
				}
				if (pickPacing) presets->SavePresetMain(p.name);
				if (pickCurves) presets->SavePresetCurves(p.name);
				ImGuiMCP::CloseCurrentPopup();
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Save everything")) {
				settings->Save();
				s_settingsDirty = false;
				presets->SavePreset(p.name);
				ImGuiMCP::CloseCurrentPopup();
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Cancel")) {
				ImGuiMCP::CloseCurrentPopup();
			}
			ImGuiMCP::EndPopup();
		}
	}

	// ---- arc editing ----
	// One big draggable graph per height tier. The arc IS the move, seen
	// from the side: x = time across the move, y = height from where you
	// started (bottom) up to the apex over the ledge (top). Drag points;
	// drag on empty space to box-select; a selection of points moves
	// together; double-click empty space to add one; right-click a point
	// to remove it. Endpoints stay pinned to the start/end of the move.
	//
	// Only one editor is ever interacted with at a time, so a single
	// global state keyed by the curve pointer suffices.
	enum class ArcMode { None, Point, Multi, Box };
	struct ArcEdit
	{
		// Persistent selection (survives across frames, independent of a
		// live drag).
		Curve*           selCurve{ nullptr };
		std::vector<int> selected;

		// Active drag.
		Curve*  dragCurve{ nullptr };
		ArcMode mode{ ArcMode::None };
		int     pointIdx{ -1 };                        // Point mode
		ImVec2  anchor{};                              // Multi/Box: mouse at start (screen px)
		std::vector<std::array<float, 2>> orig;        // Multi: snapshot of all points at drag start

		bool IsSelected(const Curve* a_c, int a_i) const
		{
			if (selCurve != a_c) return false;
			for (int s : selected) {
				if (s == a_i) return true;
			}
			return false;
		}
		void ClearSelection() { selCurve = nullptr; selected.clear(); }
		void EndDrag()
		{
			dragCurve = nullptr;
			mode = ArcMode::None;
			pointIdx = -1;
			orig.clear();
		}
	};
	ArcEdit s_arc{};

	void DrawArcEditor(const char* a_label, Curve& a_curve, float a_durationSeconds)
	{
		ImGuiMCP::PushID(a_label);
		ImGuiMCP::TextColored(ImVec4(0.85f, 0.85f, 0.95f, 1.0f), "%s", a_label);

		const float w = 520.0f;
		const float h = 190.0f;
		const float padL = 44.0f;   // room for the y-axis labels
		const float padB = 22.0f;   // room for the x-axis labels

		ImVec2 origin{};
		ImGuiMCP::GetCursorScreenPos(&origin);
		const ImVec2 plotMin{ origin.x + padL, origin.y + 6.0f };
		const ImVec2 plotMax{ origin.x + padL + w, origin.y + 6.0f + h };

		auto* dl = ImGuiMCP::GetWindowDrawList();
		if (dl) {
			// Frame + horizontal height gridlines
			ImDrawListManager::AddRectFilled(dl, plotMin, plotMax, IM_COL32(24, 24, 30, 255), 4.0f, 0);
			for (int g = 1; g < 4; ++g) {
				const float gy = plotMin.y + h * (static_cast<float>(g) / 4.0f);
				ImDrawListManager::AddLine(dl, ImVec2{ plotMin.x, gy }, ImVec2{ plotMax.x, gy }, IM_COL32(60, 60, 70, 120), 1.0f);
			}

			// Vertical time gridlines at every 0.5s of the move, labeled.
			char buf[32];
			ImDrawListManager::AddText(dl, ImVec2{ plotMin.x - 4.0f, plotMax.y + 4.0f }, IM_COL32(180, 180, 180, 255), "0s");
			const float dur = std::max(a_durationSeconds, 0.05f);
			for (float tick = 0.5f; tick < dur - 0.02f; tick += 0.5f) {
				const float gx = plotMin.x + (tick / dur) * w;
				ImDrawListManager::AddLine(dl, ImVec2{ gx, plotMin.y }, ImVec2{ gx, plotMax.y }, IM_COL32(60, 60, 70, 140), 1.0f);
				std::snprintf(buf, sizeof(buf), "%.1fs", tick);
				ImDrawListManager::AddText(dl, ImVec2{ gx - 12.0f, plotMax.y + 4.0f }, IM_COL32(180, 180, 180, 255), buf);
			}

			// Axis labels. Y: height from start to apex. X ends at the move duration.
			ImDrawListManager::AddText(dl, ImVec2{ origin.x + 2.0f, plotMin.y - 2.0f }, IM_COL32(150, 200, 150, 255), "apex");
			ImDrawListManager::AddText(dl, ImVec2{ origin.x + 2.0f, plotMax.y - 14.0f }, IM_COL32(180, 180, 180, 255), "start");
			std::snprintf(buf, sizeof(buf), "%.2fs  (time)", a_durationSeconds);
			ImDrawListManager::AddText(dl, ImVec2{ plotMax.x - 78.0f, plotMax.y + 4.0f }, IM_COL32(180, 180, 180, 255), buf);

			// Curve polyline
			const int segs = 64;
			ImVec2 prev{};
			for (int i = 0; i <= segs; ++i) {
				const float x = static_cast<float>(i) / segs;
				const float y = std::clamp(a_curve.Evaluate(x), 0.0f, 1.0f);
				ImVec2 pt{ plotMin.x + x * w, plotMax.y - y * h };
				if (i > 0) {
					ImDrawListManager::AddLine(dl, prev, pt, IM_COL32(90, 170, 255, 255), 2.5f);
				}
				prev = pt;
			}
		}

		// Input capture over the plot area (including the label strips).
		ImGuiMCP::SetCursorScreenPos(ImVec2{ origin.x, origin.y });
		ImGuiMCP::InvisibleButton("##arccanvas", ImVec2{ padL + w, 6.0f + h + padB }, 0);
		const bool hoveredCanvas = ImGuiMCP::IsItemHovered(0);

		ImVec2 mouse{};
		ImGuiMCP::GetMousePos(&mouse);
		const float mx = std::clamp((mouse.x - plotMin.x) / w, 0.0f, 1.0f);
		const float my = std::clamp((plotMax.y - mouse.y) / h, 0.0f, 1.0f);

		// Nearest point in pixel space.
		int nearest = -1;
		float nearestDistSq = 12.0f * 12.0f;  // grab radius
		for (int i = 0; i < static_cast<int>(a_curve.points.size()); ++i) {
			const float px = plotMin.x + a_curve.points[i][0] * w;
			const float py = plotMax.y - std::clamp(a_curve.points[i][1], 0.0f, 1.0f) * h;
			const float dx = mouse.x - px;
			const float dy = mouse.y - py;
			const float d2 = dx * dx + dy * dy;
			if (d2 < nearestDistSq) {
				nearestDistSq = d2;
				nearest = i;
			}
		}

		bool changed = false;
		const int  n = static_cast<int>(a_curve.points.size());
		const bool draggingThis = (s_arc.dragCurve == &a_curve);

		// Double-click empty space: add a point there. Handled before the
		// press-to-start logic so a double-click never also opens a box.
		if (hoveredCanvas && nearest < 0 && ImGuiMCP::IsMouseDoubleClicked(0)) {
			a_curve.points.push_back({ mx, my });
			a_curve.Sanitize(false, 0.0f, 1.0f);
			s_arc.ClearSelection();  // indices shift on structural change
			s_arc.EndDrag();
			changed = true;
		}
		// Right-click a point: remove it (endpoints stay).
		else if (hoveredCanvas && nearest > 0 && nearest + 1 < n &&
			ImGuiMCP::IsMouseClicked(1, false) && a_curve.points.size() > 2) {
			a_curve.points.erase(a_curve.points.begin() + nearest);
			a_curve.Sanitize(false, 0.0f, 1.0f);
			s_arc.ClearSelection();
			changed = true;
		}
		// Press: begin a drag (single point / whole selection / box).
		else if (hoveredCanvas && ImGuiMCP::IsMouseClicked(0, false)) {
			if (nearest >= 0) {
				if (s_arc.IsSelected(&a_curve, nearest) && s_arc.selected.size() > 1) {
					// Grabbed a point that is part of a multi-selection:
					// move the whole selection.
					s_arc.dragCurve = &a_curve;
					s_arc.mode = ArcMode::Multi;
					s_arc.anchor = mouse;
					s_arc.orig.assign(a_curve.points.begin(), a_curve.points.end());
				} else {
					// A lone point: select just it and drag it.
					s_arc.selCurve = &a_curve;
					s_arc.selected.assign(1, nearest);
					s_arc.dragCurve = &a_curve;
					s_arc.mode = ArcMode::Point;
					s_arc.pointIdx = nearest;
				}
			} else {
				// Empty space: start a box-select (or a click that clears).
				s_arc.dragCurve = &a_curve;
				s_arc.mode = ArcMode::Box;
				s_arc.anchor = mouse;
			}
		}

		// --- Point mode: drag one point ---
		if (draggingThis && s_arc.mode == ArcMode::Point && ImGuiMCP::IsMouseDown(0)) {
			const int i = s_arc.pointIdx;
			if (i >= 0 && i < n) {
				auto& pt = a_curve.points[i];
				pt[1] = my;
				if (i > 0 && i + 1 < n) {
					const float loX = a_curve.points[i - 1][0] + 0.02f;
					const float hiX = a_curve.points[i + 1][0] - 0.02f;
					pt[0] = std::clamp(mx, loX, hiX);
				}
				changed = true;
			}
		}

		// --- Multi mode: move the whole selection by one delta ---
		if (draggingThis && s_arc.mode == ArcMode::Multi && ImGuiMCP::IsMouseDown(0) &&
			static_cast<int>(s_arc.orig.size()) == n) {
			float ddy = -(mouse.y - s_arc.anchor.y) / h;  // screen y is inverted
			float ddx = (mouse.x - s_arc.anchor.x) / w;

			// Clamp the shared dy so no selected point leaves [0,1].
			float dyLo = -1.0f, dyHi = 1.0f;
			for (int i : s_arc.selected) {
				if (i < 0 || i >= n) continue;
				dyLo = std::max(dyLo, 0.0f - s_arc.orig[i][1]);
				dyHi = std::min(dyHi, 1.0f - s_arc.orig[i][1]);
			}
			ddy = std::clamp(ddy, dyLo, dyHi);

			// Clamp the shared dx: a selected ENDPOINT pins x entirely
			// (endpoints keep their time); interior selected points may
			// not cross a NON-selected neighbour, which also guarantees
			// the point order never changes, so selection indices stay
			// valid across Sanitize.
			bool  lockX = false;
			float dxLo = -1.0f, dxHi = 1.0f;
			for (int i : s_arc.selected) {
				if (i <= 0 || i >= n - 1) { lockX = true; break; }
				const float lw = s_arc.IsSelected(&a_curve, i - 1) ? -1.0e9f : s_arc.orig[i - 1][0] + 0.02f;
				const float rw = s_arc.IsSelected(&a_curve, i + 1) ? 1.0e9f : s_arc.orig[i + 1][0] - 0.02f;
				dxLo = std::max(dxLo, lw - s_arc.orig[i][0]);
				dxHi = std::min(dxHi, rw - s_arc.orig[i][0]);
			}
			const float dx = lockX ? 0.0f : std::clamp(ddx, dxLo, dxHi);

			for (int i : s_arc.selected) {
				if (i < 0 || i >= n) continue;
				a_curve.points[i][1] = std::clamp(s_arc.orig[i][1] + ddy, 0.0f, 1.0f);
				if (i > 0 && i < n - 1) {
					a_curve.points[i][0] = s_arc.orig[i][0] + dx;
				}
			}
			changed = true;
		}

		// --- Box mode: draw the marquee while dragging ---
		if (draggingThis && s_arc.mode == ArcMode::Box && dl && ImGuiMCP::IsMouseDown(0)) {
			const ImVec2 bmin{ std::min(s_arc.anchor.x, mouse.x), std::min(s_arc.anchor.y, mouse.y) };
			const ImVec2 bmax{ std::max(s_arc.anchor.x, mouse.x), std::max(s_arc.anchor.y, mouse.y) };
			ImDrawListManager::AddRectFilled(dl, bmin, bmax, IM_COL32(90, 170, 255, 40), 0.0f, 0);
			ImDrawListManager::AddRect(dl, bmin, bmax, IM_COL32(120, 200, 255, 200), 0.0f, 0, 1.5f);
		}

		// --- Release: finalize whichever drag was active ---
		if (draggingThis && ImGuiMCP::IsMouseReleased(0)) {
			if (s_arc.mode == ArcMode::Box) {
				const float travel = std::fabs(mouse.x - s_arc.anchor.x) + std::fabs(mouse.y - s_arc.anchor.y);
				if (travel < 4.0f) {
					s_arc.ClearSelection();  // a click on empty space clears
				} else {
					const ImVec2 bmin{ std::min(s_arc.anchor.x, mouse.x), std::min(s_arc.anchor.y, mouse.y) };
					const ImVec2 bmax{ std::max(s_arc.anchor.x, mouse.x), std::max(s_arc.anchor.y, mouse.y) };
					s_arc.selCurve = &a_curve;
					s_arc.selected.clear();
					for (int i = 0; i < n; ++i) {
						const float px = plotMin.x + a_curve.points[i][0] * w;
						const float py = plotMax.y - std::clamp(a_curve.points[i][1], 0.0f, 1.0f) * h;
						if (px >= bmin.x && px <= bmax.x && py >= bmin.y && py <= bmax.y) {
							s_arc.selected.push_back(i);
						}
					}
					if (s_arc.selected.empty()) s_arc.ClearSelection();
				}
			} else if (s_arc.mode == ArcMode::Point || s_arc.mode == ArcMode::Multi) {
				a_curve.Sanitize(false, 0.0f, 1.0f);
				changed = true;
			}
			s_arc.EndDrag();
		}

		// Point markers drawn after input so the hovered/selected ones can
		// highlight. Selected points get a green fill + ring.
		if (dl) {
			for (int i = 0; i < n; ++i) {
				const float px = plotMin.x + a_curve.points[i][0] * w;
				const float py = plotMax.y - std::clamp(a_curve.points[i][1], 0.0f, 1.0f) * h;
				const bool sel = s_arc.IsSelected(&a_curve, i);
				const bool hot = (i == nearest) || (draggingThis && s_arc.mode == ArcMode::Point && i == s_arc.pointIdx);
				const ImU32 col = sel ? IM_COL32(110, 235, 150, 255)
					: hot ? IM_COL32(255, 235, 120, 255)
					      : IM_COL32(255, 210, 80, 255);
				ImDrawListManager::AddCircleFilled(dl, ImVec2{ px, py }, (sel || hot) ? 7.0f : 5.0f, col, 12);
				if (sel) {
					ImDrawListManager::AddCircle(dl, ImVec2{ px, py }, 10.0f, IM_COL32(150, 255, 190, 220), 14, 2.0f);
				}
			}
		}

		// Every point's position, in plain units (seconds, % height).
		std::string coords;
		for (const auto& pt : a_curve.points) {
			char pb[48];
			std::snprintf(pb, sizeof(pb), "(%.2fs, %.0f%%)  ", pt[0] * a_durationSeconds, pt[1] * 100.0f);
			coords += pb;
		}
		ImGuiMCP::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1.0f), "%s", coords.c_str());
		ImGuiMCP::TextColored(ImVec4(0.5f, 0.5f, 0.55f, 1.0f),
			"drag point | drag empty: box-select | selected move together | double-click: add | right-click: remove");
		ImGuiMCP::Spacing();

		if (changed) {
			Presets::GetSingleton()->curvesDirty = true;
		}
		ImGuiMCP::PopID();
	}

	// Cached loose-file existence check for the test-idle paths. Runs
	// inside the game process, so MO2's VFS answers for virtualized
	// files. BA2-archived animations are NOT visible to this check -
	// for the test workflow the file is expected loose.
	void DrawIdleFileStatus(const char* a_id, const std::string& a_relPath)
	{
		struct CacheEntry
		{
			std::string path;
			bool exists{ false };
			float age{ 999.0f };
		};
		static CacheEntry s_cache[2];
		static auto s_lastCheck = std::chrono::steady_clock::now();

		const int slot = (std::strcmp(a_id, "slot2") == 0) ? 1 : 0;
		auto& entry = s_cache[slot];

		const auto now = std::chrono::steady_clock::now();
		const float sinceLast = std::chrono::duration<float>(now - s_lastCheck).count();
		entry.age += sinceLast;
		s_lastCheck = now;

		// Inspect-style paths include the Meshes\ prefix already; pose-
		// style paths are relative to Meshes. Accept both.
		const bool hasMeshesPrefix = a_relPath.size() >= 7 &&
			(_strnicmp(a_relPath.c_str(), "meshes\\", 7) == 0 || _strnicmp(a_relPath.c_str(), "meshes/", 7) == 0);
		if (entry.path != a_relPath || entry.age > 2.0f) {
			entry.path = a_relPath;
			entry.age = 0.0f;
			std::error_code ec;
			const auto full = hasMeshesPrefix
				? std::filesystem::path("Data") / a_relPath
				: std::filesystem::path("Data/Meshes") / a_relPath;
			entry.exists = std::filesystem::exists(full, ec) && !ec;
		}

		if (entry.exists) {
			ImGuiMCP::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
				"file found: Data\\%s%s", hasMeshesPrefix ? "" : "Meshes\\", a_relPath.c_str());
		} else {
			ImGuiMCP::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
				"FILE NOT FOUND: Data\\%s%s", hasMeshesPrefix ? "" : "Meshes\\", a_relPath.c_str());
		}
	}

	// ============================================================
	// Page: General
	// ============================================================
	void __stdcall RenderGeneral()
	{
		auto* s = Settings::GetSingleton();

		// Content scrolls inside a child region; the save bar below it
		// stays pinned on screen at every scroll position.
		ImGuiMCP::BeginChild("##gencontent", ImVec2{ 0.0f, -74.0f }, 0, 0);

		SectionHeader("Vault & Mantle",
			"Code-driven parkour triggered by the jump key. Vault = up and over "
			"(momentum kept). Mantle = up on top (momentum cleared).");
		CheckboxTip("Enable F4Parkour", &s->enabled, "Master toggle.");
		CheckboxTip("Play melee animation during moves", &s->playMeleeAnim,
			"Fires the melee action while a move runs so Open Animation Replacer can swap "
			"in a vault/mantle animation (conditions: HasKeyword AnimsParkourVaultKeyword / "
			"AnimsParkourMantleKeyword). OFF = movement only, no animation.");
		CheckboxTip("Allow in third person", &s->allowThirdPerson, "");
		CheckboxTip("Allow while airborne", &s->allowInAir,
			"Jump again in the air to grab and mantle a ledge (Brink-style air grab).");
		CheckboxTip("Allow air vaults too", &s->allowAirVault,
			"Also vault (not just mantle) while airborne - a mid-flight clear of a low "
			"obstacle. Needs 'Allow while airborne'.");
		CheckboxTip("Auto-grab while falling", &s->autoAirGrab,
			"Automatically grab a ledge you are falling toward, no button press (Dying Light "
			"reach-and-catch assist). Off = you must press jump in the air.");
		CheckboxTip("High-mantle camera director", &s->highMantleCameraDirector,
			"On a HIGH mantle in first person, pulls the view out of the ledge geometry so the "
			"rising camera never clips through it. First person only.");
		CheckboxTip("Show parkour indicator", &s->indicatorEnabled,
			"Small ring on a ledge you can vault or mantle (SkyParkour-style cue). "
			"Only shown for ledges above the LOW height - knee-high steps stay quiet.");

		SectionHeader("Trigger",
			"Jump contextually becomes a vault/mantle when a ledge is detected.");
		CheckboxTip("Require forward input", &s->requireForward,
			"Only parkour when holding forward. Stationary or strafing jumps stay normal jumps. "
			"Also enables the mid-move rule: release forward before the apex to turn a vault "
			"into a mantle.");
		SliderTip("Look cone (degrees)", &s->lookConeDeg, 5.0f, 90.0f, "%.0f",
			"How closely you must face the ledge for jump to trigger parkour.");
		SliderTip("Jump buffer (s)", &s->jumpBufferWindow, 0.0f, 0.6f, "%.2f",
			"A jump pressed slightly before a ledge is detected still counts (Dying Light "
			"jump assist). Also catches air-grabs right after a jump.");
		SliderTip("Coyote time (s)", &s->coyoteWindow, 0.0f, 0.5f, "%.2f",
			"Grounded grace period after walking off an edge.");
		CheckboxTip("Hold jump to prefer mantle", &s->holdToMantle,
			"When both moves are possible: tap = contextual choice, hold = commit to mantling "
			"on top.");

		SectionHeader("Automatic moves", "Both default OFF - manual triggering preserves skill.");
		CheckboxTip("Auto-vault while sprinting", &s->autoParkourSprint,
			"Sprinting at a vaultable obstacle triggers the vault without pressing jump "
			"(Brink SMART-button style).");
		CheckboxTip("Auto step-up (knee-height only)", &s->autoStepUp,
			"Automatically hop obstacles below the lowest vault tier while sprinting - the "
			"kind that hide under your FOV up close (Dying Light does this unconditionally).");
		SliderTip("Auto engage distance", &s->autoEngageDistance, 20.0f, 150.0f, "%.0f",
			"How close (game units) an obstacle must be before an automatic move fires.");

		SectionHeader("Comfort");
		CheckboxTip("Focus dot during moves", &s->focusDot,
			"A faint dot at screen center while a move plays. Sounds silly, works - your eyes "
			"anchor to it and motion reads far smoother (Techland's cheapest anti-nausea fix).");
		SliderTip("Focus dot opacity", &s->focusDotAlpha, 0.05f, 1.0f, "%.2f", "");

		ImGuiMCP::EndChild();
		SaveBar();
	}

	// ============================================================
	// Page: Movement & Curves
	// ============================================================
	void __stdcall RenderMovement()
	{
		auto* s = Settings::GetSingleton();
		auto* presets = Presets::GetSingleton();
		auto& p = presets->Active();

		// Content scrolls inside a child region; the save bar below it
		// stays pinned on screen at every scroll position.
		ImGuiMCP::BeginChild("##movcontent", ImVec2{ 0.0f, -74.0f }, 0, 0);

		SectionHeader("Feel preset",
			"Tier heights, times, and motion curves. Shipped presets: Smooth, Snappy, "
			"Deliberate, Arcade. Edits apply live; the Save button at the bottom of the "
			"page writes them to disk (curves live in their own .curves.json side file).");

		if (ImGuiMCP::BeginCombo("Preset", p.name.c_str(), 0)) {
			for (const auto& name : presets->List()) {
				const bool selected = (name == p.name);
				if (ImGuiMCP::Selectable(name.c_str(), selected, 0, ImVec2{ 0, 0 })) {
					presets->LoadPreset(name);
					s->activePreset = name;
					Settings::GetSingleton()->Save();
				}
			}
			ImGuiMCP::EndCombo();
		}
		ImGuiMCP::SetNextItemWidth(160.0f);
		ImGuiMCP::InputText("##newname", s_presetNameBuf, sizeof(s_presetNameBuf), 0, nullptr, nullptr);
		ImGuiMCP::SameLine();
		if (ImGuiMCP::Button("Save as new preset") && s_presetNameBuf[0]) {
			presets->SavePreset(s_presetNameBuf);
			s->activePreset = s_presetNameBuf;
			Settings::GetSingleton()->Save();
			s_presetNameBuf[0] = '\0';
		}

		// Duplicate any preset (including the shipped ones) into a fresh
		// custom preset: pick a source, name the copy, edit away — the
		// source file is never touched.
		ImGuiMCP::Spacing();
		ImGuiMCP::Text("Duplicate a preset:");
		const auto& names = presets->List();
		if (s_copySourceIdx >= static_cast<int>(names.size())) s_copySourceIdx = 0;
		const char* srcLabel = names.empty() ? "(none)" : names[s_copySourceIdx].c_str();
		ImGuiMCP::SetNextItemWidth(160.0f);
		if (ImGuiMCP::BeginCombo("##dupsrc", srcLabel, 0)) {
			for (int i = 0; i < static_cast<int>(names.size()); ++i) {
				if (ImGuiMCP::Selectable(names[i].c_str(), i == s_copySourceIdx, 0, ImVec2{ 0, 0 })) {
					s_copySourceIdx = i;
				}
			}
			ImGuiMCP::EndCombo();
		}
		ImGuiMCP::SameLine();
		ImGuiMCP::SetNextItemWidth(160.0f);
		ImGuiMCP::InputText("##dupname", s_copyNameBuf, sizeof(s_copyNameBuf), 0, nullptr, nullptr);
		ImGuiMCP::SameLine();
		if (ImGuiMCP::Button("Create copy")) {
			if (!names.empty() && s_copyNameBuf[0]) {
				const std::string source = names[s_copySourceIdx];
				if (presets->CopyPreset(source, s_copyNameBuf)) {
					presets->LoadPreset(s_copyNameBuf);
					s->activePreset = s_copyNameBuf;
					Settings::GetSingleton()->Save();
					s_copyFeedback = std::format("Copied '{}' to '{}' - now active, edit away", source, s_copyNameBuf);
					s_copyNameBuf[0] = '\0';
				} else {
					s_copyFeedback = "Copy failed (name taken or empty?)";
				}
			} else {
				s_copyFeedback = "Pick a source and type a name first";
			}
		}
		if (!s_copyFeedback.empty()) {
			ImGuiMCP::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", s_copyFeedback.c_str());
		}

		SectionHeader("Heights & times",
			"Reference heights (game units; 70 = ~1 meter, eye height = ~108). Anything "
			"between tiers blends smoothly - these are anchors, not limits.");
		ImGuiMCP::Text("Vault tiers (up & over)");
		bool ch = false;
		ch |= ImGuiMCP::SliderFloat("Low height##v", &p.vaultHeights[0], 20.0f, 200.0f, "%.0f");
		ch |= ImGuiMCP::SliderFloat("Mid height##v", &p.vaultHeights[1], 20.0f, 200.0f, "%.0f");
		ch |= ImGuiMCP::SliderFloat("High height##v", &p.vaultHeights[2], 20.0f, 200.0f, "%.0f");
		ch |= ImGuiMCP::SliderFloat("Low time##vd", &p.vaultDurations[0], 0.15f, 3.0f, "%.2fs");
		ch |= ImGuiMCP::SliderFloat("Mid time##vd", &p.vaultDurations[1], 0.15f, 3.0f, "%.2fs");
		ch |= ImGuiMCP::SliderFloat("High time##vd", &p.vaultDurations[2], 0.15f, 3.0f, "%.2fs");
		ImGuiMCP::Spacing();
		ImGuiMCP::Text("Mantle tiers (up on top)");
		ch |= ImGuiMCP::SliderFloat("Low height##m", &p.mantleHeights[0], 30.0f, 200.0f, "%.0f");
		ch |= ImGuiMCP::SliderFloat("Mid height##m", &p.mantleHeights[1], 30.0f, 200.0f, "%.0f");
		ch |= ImGuiMCP::SliderFloat("High height##m", &p.mantleHeights[2], 30.0f, 200.0f, "%.0f");
		ch |= ImGuiMCP::SliderFloat("Low time##md", &p.mantleDurations[0], 0.15f, 3.0f, "%.2fs");
		ch |= ImGuiMCP::SliderFloat("Mid time##md", &p.mantleDurations[1], 0.15f, 3.0f, "%.2fs");
		ch |= ImGuiMCP::SliderFloat("High time##md", &p.mantleDurations[2], 0.15f, 3.0f, "%.2fs");

		SectionHeader("Pace & momentum");
		ch |= ImGuiMCP::SliderFloat("Global speed", &p.speedMult, 0.25f, 4.0f, "%.2fx");
		ch |= ImGuiMCP::SliderFloat("Sprint time scale", &p.sprintDurationScale, 0.4f, 1.5f, "%.2fx");
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("%s",
				"Multiplies move time when entering at sprint. BELOW 1.0 makes sprint "
				"vaults FASTER than walk-up ones; set 1.0 for identical timing.");
		}
		ch |= ImGuiMCP::SliderFloat("Apex clearance", &p.apexClearance, 0.0f, 40.0f, "%.0f");
		if (ch) {
			p.Sanitize();
			presets->pacingDirty = true;
		}
		SliderTip("Vault speed matching", &s->vaultSpeedMatch, 0.0f, 1.0f, "%.2f",
			"Fast entries compress the vault so ground speed stays constant (Brink-style "
			"flow). 0 = OFF: every vault takes its full tier time, uniform speed. "
			"1 = full compression. This - not momentum - is what made sprint vaults "
			"much quicker than others.");
		SliderTip("Momentum kept on vault", &s->momentumKeep, 0.0f, 1.5f, "%.2fx",
			"How much of your entry speed carries through a vault landing. Sprint through "
			"vaults at 1.0.");
		SliderTip("Exit direction steering", &s->exitDirBlend, 0.0f, 1.0f, "%.2f",
			"0 = momentum continues along the approach; 1 = fully where you are looking on "
			"exit (Brink found some steering feels best).");
		SliderTip("Momentum drop cutoff", &s->momentumDropCutoff, 30.0f, 300.0f, "%.0f",
			"If the ground past a vault falls away more than this, momentum is cleared "
			"instead of launching you off the edge.");
		SliderTip("Control handback", &s->controlHandback, 0.0f, 0.6f, "%.2f",
			"Final fraction of the move where your keys come back to life (jump can buffer "
			"the next hop, sprint re-arms) - control returns before the move visually ends "
			"(Dying Light comfort rule).");

		SectionHeader("Move arcs",
			"Each graph is the move seen from the side: left-to-right is TIME, bottom-to-top "
			"is HEIGHT (bottom = where you started, top = the apex over the ledge). One arc "
			"per height tier; heights between tiers blend the two nearest arcs. The end of "
			"the arc is pinned to the real landing height in game, so the last stretch of "
			"the curve only shapes the final approach.");
		{
			auto& p2 = presets->Active();
			char label[96];

			// Copy one tier's arc onto another: dial in a single curve,
			// then stamp it across tiers as a starting point.
			{
				static int s_arcSrc = 0;
				static int s_arcDst = 1;
				static const char* kArcNames[6] = {
					"Vault Low", "Vault Mid", "Vault High",
					"Mantle Low", "Mantle Mid", "Mantle High"
				};
				Curve* arcs[6] = {
					&p2.vaultArc[0], &p2.vaultArc[1], &p2.vaultArc[2],
					&p2.mantleArc[0], &p2.mantleArc[1], &p2.mantleArc[2]
				};
				ImGuiMCP::Spacing();
				ImGuiMCP::Text("Copy curve:");
				ImGuiMCP::SameLine();
				ImGuiMCP::SetNextItemWidth(130.0f);
				if (ImGuiMCP::BeginCombo("##arccopysrc", kArcNames[s_arcSrc], 0)) {
					for (int i = 0; i < 6; ++i) {
						if (ImGuiMCP::Selectable(kArcNames[i], i == s_arcSrc, 0, ImVec2{ 0, 0 })) {
							s_arcSrc = i;
						}
					}
					ImGuiMCP::EndCombo();
				}
				ImGuiMCP::SameLine();
				ImGuiMCP::Text("onto");
				ImGuiMCP::SameLine();
				ImGuiMCP::SetNextItemWidth(130.0f);
				if (ImGuiMCP::BeginCombo("##arccopydst", kArcNames[s_arcDst], 0)) {
					for (int i = 0; i < 6; ++i) {
						if (ImGuiMCP::Selectable(kArcNames[i], i == s_arcDst, 0, ImVec2{ 0, 0 })) {
							s_arcDst = i;
						}
					}
					ImGuiMCP::EndCombo();
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Copy##arccopy")) {
					if (s_arcSrc != s_arcDst) {
						*arcs[s_arcDst] = *arcs[s_arcSrc];
						Presets::GetSingleton()->curvesDirty = true;
					}
				}
				if (ImGuiMCP::IsItemHovered()) {
					ImGuiMCP::SetTooltip("%s",
						"Overwrites the destination arc with the source arc (in memory - "
						"use Save... to keep it).");
				}
			}

			ImGuiMCP::Spacing();
			ImGuiMCP::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "Vault arcs (up and OVER - ends past the obstacle)");
			for (int i = 0; i < 3; ++i) {
				static const char* tierNames[3] = { "Low", "Mid", "High" };
				std::snprintf(label, sizeof(label), "Vault %s  (ledges near %.0f units, %.2fs)",
					tierNames[i], p2.vaultHeights[i], p2.vaultDurations[i]);
				DrawArcEditor(label, p2.vaultArc[i], p2.vaultDurations[i]);
			}
			PresetSliderTip("Vault speed easing", &p2.vaultEase, 0.0f, 1.0f, "%.2f",
				"How your speed along the ground behaves: 0 = constant speed the whole way "
				"(momentum feel), 1 = soft start and stop. Never reverses - the camera can "
				"never move backward during a move.");

			ImGuiMCP::Spacing();
			ImGuiMCP::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "Mantle arcs (up and ON TOP - ends on the ledge)");
			for (int i = 0; i < 3; ++i) {
				static const char* tierNames[3] = { "Low", "Mid", "High" };
				std::snprintf(label, sizeof(label), "Mantle %s  (ledges near %.0f units, %.2fs)",
					tierNames[i], p2.mantleHeights[i], p2.mantleDurations[i]);
				DrawArcEditor(label, p2.mantleArc[i], p2.mantleDurations[i]);
			}
			PresetSliderTip("Mantle speed easing", &p2.mantleEase, 0.0f, 1.0f, "%.2f",
				"0 = constant speed, 1 = soft start and stop. A slow finish reads as body "
				"weight on the pull-up.");
		}

		SectionHeader("Test", "Fires against whatever the detector currently sees.");
		if (ImGuiMCP::Button("Test vault now")) {
			ParkourManager::GetSingleton()->ForceActivate(MoveKind::Vault);
		}
		ImGuiMCP::SameLine();
		if (ImGuiMCP::Button("Test mantle now")) {
			ParkourManager::GetSingleton()->ForceActivate(MoveKind::Mantle);
		}

		ImGuiMCP::EndChild();
		SaveBar();
	}

	// ============================================================
	// Debug content (tab + popout)
	// ============================================================
	void DrawDebugContent()
	{
		auto* s = Settings::GetSingleton();
		auto* dbg = DebugDraw::GetSingleton();

		ImGuiMCP::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "[ Live state ]");
		ImGuiMCP::Separator();
		const std::string state = dbg->GetStateText();
		ImGuiMCP::TextUnformatted(state.empty() ? "(debug disabled)" : state.c_str(), nullptr);

		ImGuiMCP::Spacing();
		ImGuiMCP::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "[ Decision log (newest first) ]");
		ImGuiMCP::Separator();
		const auto events = dbg->GetEvents();
		if (events.empty()) {
			ImGuiMCP::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  (no events)");
		}
		for (const auto& e : events) {
			const float fade = std::clamp(1.0f - e.age / 12.0f, 0.25f, 1.0f);
			ImGuiMCP::TextColored(ImVec4(0.8f, 0.8f, 0.8f, fade), "  [%4.1fs] %s", e.age, e.text.c_str());
		}
		(void)s;
	}

	// ============================================================
	// Page: Debugging
	// ============================================================
	void __stdcall RenderDebug()
	{
		auto* s = Settings::GetSingleton();

		// Content scrolls inside a child region; the save bar below it
		// stays pinned on screen at every scroll position.
		ImGuiMCP::BeginChild("##dbgcontent", ImVec2{ 0.0f, -74.0f }, 0, 0);

		SectionHeader("Debug tools",
			"World overlays draw every detection ray with its result; the decision log "
			"explains every reject in plain words.");
		CheckboxTip("Enable debugging", &s->debugEnabled,
			"Master switch for overlays, state readout, and the decision log.");
		CheckboxTip("Draw detection rays", &s->drawRays,
			"Green = check passed, red = check failed, grey = no hit. Labels show distance "
			"and collision layer.");
		CheckboxTip("Draw planned path", &s->drawPath, "The blue arc a move will follow.");
		CheckboxTip("Only failed rays", &s->drawOnlyFailed,
			"Hide passing rays to spot the one check that vetoes a move.");
		CheckboxTip("Freeze detection", &s->freezeDetection,
			"Stop scanning so you can walk around the last overlay.");
		SliderTip("Mover time scale", &s->moverTimeScale, 0.05f, 1.0f, "%.2fx",
			"Slow-motion moves for frame-by-frame inspection.");
		CheckboxTip("In-flight watchdog", &s->watchdogEnabled,
			"Per-frame obstruction probe during a move. Disable to compare against pure "
			"endpoint validation (Brink shipped with collision fully off mid-move).");
		CheckboxTip("Warp scene each move frame", &s->warpSceneEachFrame,
			"OFF (default) updates the player's 3D as a normal MOVE each frame; ON forces "
			"the TELEPORT path 30-60x per move, which hammers the culling caches and is a "
			"suspected trigger for render-thread crashes. Turn ON only if the character "
			"mesh visibly lags behind the capsule during a move.");

		SectionHeader("Animation test (PlayIdle)",
			"Per tier: instead of the melee action, play a raw idle .hkx directly when the "
			"move starts - iterate animations without OAR configs. Path is relative to "
			"Data/Meshes.");
		{
			bool tch = false;
			tch |= ImGuiMCP::Checkbox("Vault Low##ti", &s->testIdleVault[0]);
			ImGuiMCP::SameLine();
			tch |= ImGuiMCP::Checkbox("Vault Mid##ti", &s->testIdleVault[1]);
			ImGuiMCP::SameLine();
			tch |= ImGuiMCP::Checkbox("Vault High##ti", &s->testIdleVault[2]);
			tch |= ImGuiMCP::Checkbox("Mantle Low##ti", &s->testIdleMantle[0]);
			ImGuiMCP::SameLine();
			tch |= ImGuiMCP::Checkbox("Mantle Mid##ti", &s->testIdleMantle[1]);
			ImGuiMCP::SameLine();
			tch |= ImGuiMCP::Checkbox("Mantle High##ti", &s->testIdleMantle[2]);

			static char pathBuf[260]{};
			static bool pathInit = false;
			if (!pathInit) {
				std::snprintf(pathBuf, sizeof(pathBuf), "%s", s->testIdlePath.c_str());
				pathInit = true;
			}
			ImGuiMCP::SetNextItemWidth(340.0f);
			if (ImGuiMCP::InputText("Idle path##ti", pathBuf, sizeof(pathBuf), 0, nullptr, nullptr)) {
				s->testIdlePath = pathBuf;
				tch = true;
			}
			DrawIdleFileStatus("slot1", s->testIdlePath);
			static char evtBuf[128]{};
			static bool evtInit = false;
			if (!evtInit) {
				std::snprintf(evtBuf, sizeof(evtBuf), "%s", s->testIdleEvent.c_str());
				evtInit = true;
			}
			ImGuiMCP::SetNextItemWidth(220.0f);
			if (ImGuiMCP::InputText("Anim event##ti", evtBuf, sizeof(evtBuf), 0, nullptr, nullptr)) {
				s->testIdleEvent = evtBuf;
				tch = true;
			}
			ImGuiMCP::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
				"One-shot via dyn_Activation (the weapon-inspect path) - plays in first person "
				"when the .hkx is authored for the first-person rig.");
			if (tch) s_settingsDirty = true;
		}

		SectionHeader("Animation test 2 (PlayIdle + skip equip)",
			"A second idle slot, same as above but it also SKIPS the fast-equip animation "
			"that normally plays when the idle ends (the SeamlessInspect technique). Defaults "
			"to Vault.hkx. On a tier that has both slots on, this one wins.");
		{
			bool tch2 = false;
			tch2 |= ImGuiMCP::Checkbox("Vault Low##ti2", &s->testIdle2Vault[0]);
			ImGuiMCP::SameLine();
			tch2 |= ImGuiMCP::Checkbox("Vault Mid##ti2", &s->testIdle2Vault[1]);
			ImGuiMCP::SameLine();
			tch2 |= ImGuiMCP::Checkbox("Vault High##ti2", &s->testIdle2Vault[2]);
			tch2 |= ImGuiMCP::Checkbox("Mantle Low##ti2", &s->testIdle2Mantle[0]);
			ImGuiMCP::SameLine();
			tch2 |= ImGuiMCP::Checkbox("Mantle Mid##ti2", &s->testIdle2Mantle[1]);
			ImGuiMCP::SameLine();
			tch2 |= ImGuiMCP::Checkbox("Mantle High##ti2", &s->testIdle2Mantle[2]);

			static char pathBuf2[260]{};
			static bool pathInit2 = false;
			if (!pathInit2) {
				std::snprintf(pathBuf2, sizeof(pathBuf2), "%s", s->testIdle2Path.c_str());
				pathInit2 = true;
			}
			ImGuiMCP::SetNextItemWidth(340.0f);
			if (ImGuiMCP::InputText("Idle path##ti2", pathBuf2, sizeof(pathBuf2), 0, nullptr, nullptr)) {
				s->testIdle2Path = pathBuf2;
				tch2 = true;
			}
			DrawIdleFileStatus("slot2", s->testIdle2Path);
			static char evtBuf2[128]{};
			static bool evtInit2 = false;
			if (!evtInit2) {
				std::snprintf(evtBuf2, sizeof(evtBuf2), "%s", s->testIdle2Event.c_str());
				evtInit2 = true;
			}
			ImGuiMCP::SetNextItemWidth(220.0f);
			if (ImGuiMCP::InputText("Anim event##ti2", evtBuf2, sizeof(evtBuf2), 0, nullptr, nullptr)) {
				s->testIdle2Event = evtBuf2;
				tch2 = true;
			}
			ImGuiMCP::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
				"Resets the actor to base state on the idle's IdleStop event, suppressing the "
				"fast-equip - watch the log for '[AnimHijack] IdleStop -> equip-skip fired'.");
			if (tch2) s_settingsDirty = true;
		}

		bool popoutOpen = s_debugPopout && s_debugPopout->IsOpen.load();
		if (ImGuiMCP::Checkbox("Popout window (plays without pausing)", &popoutOpen)) {
			if (s_debugPopout) s_debugPopout->IsOpen.store(popoutOpen);
		}

		ImGuiMCP::Spacing();
		DrawDebugContent();

		ImGuiMCP::EndChild();
		SaveBar();
	}

	void __stdcall RenderDebugPopout()
	{
		DrawDebugContent();
	}

	// ============================================================
	// HUD overlay: debug rays + candidate indicator + focus dot
	// ============================================================
	void __stdcall RenderHUD()
	{
		auto* s = Settings::GetSingleton();
		if (!s->enabled) return;

		DebugDraw::GetSingleton()->Render();

		if (F4SEMenuFramework::IsAnyBlockingWindowOpened()) return;

		auto* io = ImGuiMCP::GetIO();
		auto* drawList = ImGuiMCP::GetForegroundDrawList();
		if (!io || !drawList) return;

		// Focus dot while a move plays.
		auto* mover = Mover::GetSingleton();
		if (mover->IsActive() && s->focusDot) {
			const ImVec2 center{ io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f };
			const auto a = static_cast<int>(255.0f * s->focusDotAlpha);
			ImDrawListManager::AddCircleFilled(drawList, center, 3.0f,
				IM_COL32(255, 255, 255, a), 10);
		}
	}
}

namespace F4Parkour::Menu
{
	void Register()
	{
		if (s_registered) return;

		if (!F4SEMenuFramework::IsInstalled()) {
			logger::warn("[Menu] F4SE Menu Framework is not installed - menu unavailable");
			return;
		}

		F4SEMenuFramework::SetSection("F4Parkour");
		F4SEMenuFramework::AddSectionItem("General", RenderGeneral);
		F4SEMenuFramework::AddSectionItem("Movement & Curves", RenderMovement);
		F4SEMenuFramework::AddSectionItem("Debugging", RenderDebug);
		F4SEMenuFramework::AddHudElement(RenderHUD);
		s_debugPopout = F4SEMenuFramework::AddWindow(RenderDebugPopout, false);

		s_registered = true;
		logger::info("[Menu] Registered with F4SE Menu Framework");
	}
}
