#include "PCH.h"
#include "AnimHijack.h"
#include "Settings.h"
#include "SyntheticInput.h"

#include <chrono>

#include <Windows.h>

// ============================================================
// Player anim-graph event hook (SeamlessInspect / FPCameraOverhaul
// pattern for this CommonLib fork): PlayerCharacter inherits
// BSTEventSink<BSAnimationGraphEvent> at +0x38; ProcessEvent is
// vtable slot 1 (+0x8). Patching that slot observes every graph
// event without an OG-only relocation. Multiple mods can hook this
// slot: each stores the pointer it found and forwards, so the chain
// works regardless of install order.
// ============================================================
namespace
{
	using FnProcessEvent = RE::BSEventNotifyControl (*)(
		void*, const RE::BSAnimationGraphEvent&,
		RE::BSTEventSource<RE::BSAnimationGraphEvent>*);

	FnProcessEvent g_originalProcessEvent = nullptr;

	RE::BSEventNotifyControl HookedProcessEvent(
		void* a_self,
		const RE::BSAnimationGraphEvent& a_event,
		RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
	{
		F4Parkour::AnimHijack::GetSingleton()->OnAnimEvent(a_event.tag);
		return g_originalProcessEvent
			? g_originalProcessEvent(a_self, a_event, a_source)
			: RE::BSEventNotifyControl::kContinue;
	}
}

namespace
{
	// Create (or reuse) a runtime keyword by editor ID — the proven
	// SuperSprint pattern (FPGunplayOverhaul InitSuperSprint).
	RE::BGSKeyword* GetOrCreateKeyword(const char* a_editorID)
	{
		auto* kw = RE::TESForm::GetFormByEditorID<RE::BGSKeyword>(a_editorID);
		if (kw) {
			logger::info("[AnimHijack] Found existing keyword '{}' (FormID 0x{:08X})", a_editorID, kw->GetFormID());
			return kw;
		}
		if (auto* factory = RE::ConcreteFormFactory<RE::BGSKeyword, RE::ENUM_FORM_ID::kKYWD>::GetFormFactory()) {
			kw = factory->Create();
			if (kw) {
				kw->SetFormEditorID(a_editorID);
				logger::info("[AnimHijack] Created runtime keyword '{}' (FormID 0x{:08X})", a_editorID, kw->GetFormID());
				return kw;
			}
		}
		logger::error("[AnimHijack] Failed to create runtime keyword '{}'", a_editorID);
		return nullptr;
	}

	// Keyword add/remove on the player NPC's BGSKeywordForm — the same
	// allocate-copy-swap approach as SuperSprint / CrouchSlide so OAR
	// sees identical data, extended with a capacity-tracked owned array:
	// keywords toggle every move here (not once per session), so
	// re-allocating per add would leak an array per vault. Once the form
	// points at OUR array we append/remove in place.
	struct OwnedArray
	{
		RE::BGSKeyword** arr{ nullptr };
		std::uint32_t    cap{ 0 };
	};
	OwnedArray s_owned{};

	void AddKeywordTo(RE::BGSKeywordForm* a_form, RE::BGSKeyword* a_kw)
	{
		if (!a_form || !a_kw) return;
		for (std::uint32_t i = 0; i < a_form->numKeywords; ++i) {
			if (a_form->keywords[i] == a_kw) return;
		}

		if (a_form->keywords == s_owned.arr && s_owned.arr && a_form->numKeywords < s_owned.cap) {
			s_owned.arr[a_form->numKeywords] = a_kw;
			a_form->numKeywords++;
			return;
		}

		const auto newCount = a_form->numKeywords + 1;
		const auto newCap = newCount + 8;  // room for the whole parkour keyword set
		auto** newArr = new RE::BGSKeyword*[newCap];
		for (std::uint32_t i = 0; i < a_form->numKeywords; ++i) {
			newArr[i] = a_form->keywords[i];
		}
		newArr[a_form->numKeywords] = a_kw;
		a_form->keywords = newArr;
		a_form->numKeywords = newCount;
		// Previous owned array (if any) is intentionally abandoned only
		// when the engine swapped the pointer out from under us; the
		// common per-move path reuses in place.
		s_owned.arr = newArr;
		s_owned.cap = newCap;
	}

	void RemoveKeywordFrom(RE::BGSKeywordForm* a_form, RE::BGSKeyword* a_kw)
	{
		if (!a_form || !a_kw) return;
		std::uint32_t idx = UINT32_MAX;
		for (std::uint32_t i = 0; i < a_form->numKeywords; ++i) {
			if (a_form->keywords[i] == a_kw) { idx = i; break; }
		}
		if (idx == UINT32_MAX) return;
		for (std::uint32_t i = idx; i < a_form->numKeywords - 1; ++i) {
			a_form->keywords[i] = a_form->keywords[i + 1];
		}
		a_form->numKeywords--;
	}

	RE::BGSKeywordForm* PlayerKeywordForm(RE::PlayerCharacter* a_player)
	{
		if (!a_player) return nullptr;
		auto* npc = a_player->GetNPC();
		return npc ? static_cast<RE::BGSKeywordForm*>(npc) : nullptr;
	}

	void SetGraphVars(RE::PlayerCharacter* a_player, F4Parkour::MoveKind a_kind, float a_height)
	{
		static const RE::BSFixedString kType{ "F4Parkour_Type" };
		static const RE::BSFixedString kHeight{ "F4Parkour_Height" };
		// -1 = no move, 0 = vault, 1 = mantle (documented in the OAR
		// example configs).
		const int type = a_kind == F4Parkour::MoveKind::Vault ? 0 :
			a_kind == F4Parkour::MoveKind::Mantle ? 1 : -1;
		a_player->SetGraphVariableInt(kType, type);
		a_player->SetGraphVariableFloat(kHeight, a_height);
	}
}

namespace F4Parkour
{
	void AnimHijack::Init()
	{
		kwParkour = GetOrCreateKeyword("AnimsParkourKeyword");
		kwVault   = GetOrCreateKeyword("AnimsParkourVaultKeyword");
		kwMantle  = GetOrCreateKeyword("AnimsParkourMantleKeyword");
		static constexpr const char* kVaultTierIDs[3] = {
			"AnimsParkourVaultLowKeyword", "AnimsParkourVaultMidKeyword", "AnimsParkourVaultHighKeyword"
		};
		static constexpr const char* kMantleTierIDs[3] = {
			"AnimsParkourMantleLowKeyword", "AnimsParkourMantleMidKeyword", "AnimsParkourMantleHighKeyword"
		};
		for (int i = 0; i < 3; ++i) {
			kwVaultTier[i] = GetOrCreateKeyword(kVaultTierIDs[i]);
			kwMantleTier[i] = GetOrCreateKeyword(kMantleTierIDs[i]);
		}

		// ActionMelee resolved by editor ID (runtime-agnostic; the FPGO
		// gun-bash work verified the default-object manager path is the
		// fragile one).
		actionMelee = RE::TESForm::GetFormByEditorID<RE::BGSAction>("ActionMelee");
		if (!actionMelee) {
			if (auto* dom = RE::BGSDefaultObjectManager::GetSingleton()) {
				actionMelee = dom->GetDefaultObject<RE::BGSAction>(RE::DEFAULT_OBJECT::kActionMelee);
			}
		}
		// Runtime idle form for the per-tier PlayIdle test toggles. The
		// anim file path is set fresh from Settings before every play so
		// the menu can retarget it without a restart.
		if (auto* idleFactory = RE::ConcreteFormFactory<RE::TESIdleForm, RE::ENUM_FORM_ID::kIDLE>::GetFormFactory()) {
			testIdle = idleFactory->Create();
			if (testIdle) {
				testIdle->SetFormEditorID("F4ParkourTestIdle");
			}
		}
		if (!testIdle) {
			logger::warn("[AnimHijack] Failed to create runtime idle form - test-idle toggles disabled");
		}

		InstallAnimEventHook();

		logger::info("[AnimHijack] Init: keywords={} action={} testIdle={}",
			kwParkour && kwVault && kwMantle, actionMelee != nullptr, testIdle != nullptr);
	}

	void AnimHijack::InstallAnimEventHook()
	{
		if (animEventHookInstalled) return;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			logger::warn("[AnimHijack] Player null - equip-skip anim hook not installed");
			return;
		}

		auto* sink = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(player) + 0x38);
		const std::uintptr_t vtable = *reinterpret_cast<std::uintptr_t*>(sink);
		const std::uintptr_t addr = vtable + 0x8;  // ProcessEvent = slot 1

		std::memcpy(&g_originalProcessEvent, reinterpret_cast<void*>(addr), sizeof(void*));

		const std::uintptr_t hookAddr = reinterpret_cast<std::uintptr_t>(&HookedProcessEvent);
		DWORD oldProtect = 0;
		if (!::VirtualProtect(reinterpret_cast<void*>(addr), sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			logger::error("[AnimHijack] VirtualProtect failed - equip-skip anim hook disabled");
			g_originalProcessEvent = nullptr;
			return;
		}
		std::memcpy(reinterpret_cast<void*>(addr), &hookAddr, sizeof(void*));
		::VirtualProtect(reinterpret_cast<void*>(addr), sizeof(void*), oldProtect, &oldProtect);

		animEventHookInstalled = true;
		logger::info("[AnimHijack] Hooked player anim-graph ProcessEvent for equip-skip (vtable=0x{:X})", vtable);
	}

	void AnimHijack::OnGameLoaded()
	{
		// The player TESNPC is a global form: runtime keywords added
		// mid-move persist across in-session loads. Strip them so a load
		// during a vault can never leave every future melee playing the
		// parkour replacement clip.
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player) {
			RemoveAll(player);
			SetGraphVars(player, MoveKind::None, 0.0f);
		}
		// The owned-array cache may or may not still back the reloaded
		// NPC's keyword form; drop the claim either way and let the next
		// add re-establish ownership.
		s_owned = OwnedArray{};
	}

	void AnimHijack::AddKeyword(RE::PlayerCharacter* a_player, RE::BGSKeyword* a_kw)
	{
		AddKeywordTo(PlayerKeywordForm(a_player), a_kw);
	}

	void AnimHijack::RemoveKeyword(RE::PlayerCharacter* a_player, RE::BGSKeyword* a_kw)
	{
		RemoveKeywordFrom(PlayerKeywordForm(a_player), a_kw);
	}

	void AnimHijack::RemoveAll(RE::PlayerCharacter* a_player)
	{
		auto* form = PlayerKeywordForm(a_player);
		RemoveKeywordFrom(form, kwParkour);
		RemoveKeywordFrom(form, kwVault);
		RemoveKeywordFrom(form, kwMantle);
		for (int i = 0; i < 3; ++i) {
			RemoveKeywordFrom(form, kwVaultTier[i]);
			RemoveKeywordFrom(form, kwMantleTier[i]);
		}
	}

	void AnimHijack::OnMoveStart(RE::PlayerCharacter* a_player, MoveKind a_kind, float a_height, int a_tier)
	{
		// 1. Pre-arm keywords + graph variables (must precede the clip —
		// OAR evaluates conditions at clip activation).
		a_tier = std::clamp(a_tier, 0, 2);
		AddKeyword(a_player, kwParkour);
		AddKeyword(a_player, a_kind == MoveKind::Vault ? kwVault : kwMantle);
		AddKeyword(a_player, a_kind == MoveKind::Vault ? kwVaultTier[a_tier] : kwMantleTier[a_tier]);
		SetGraphVars(a_player, a_kind, a_height);

		// 2. Notify the graph for anyone listening.
		static const RE::BSFixedString kEvtVault{ "F4Parkour_Vault" };
		static const RE::BSFixedString kEvtMantle{ "F4Parkour_Mantle" };
		a_player->NotifyAnimationGraphImpl(a_kind == MoveKind::Vault ? kEvtVault : kEvtMantle);

		// 3. Animation: either a per-tier PlayIdle test toggle (plays a raw
		// .hkx directly — iterate animations without OAR configs) or the
		// melee action the OAR replacement rides on. A refusal only costs
		// the animation — the movement is already running and never waits
		// on this.
		//
		// Two test-idle slots: slot 2 (Mantle.hkx) wins over slot 1
		// (Ledge.hkx) on a tier that has both on. (The slot-2 equip-skip
		// is retired — post-idle equip handling lives OAR-side now.)
		auto* settings = Settings::GetSingleton();
		const bool vault = (a_kind == MoveKind::Vault);

		// VAULT WHILE AIMING — keep the weapon aimed, play NO special idle.
		// SetupSpecialIdle irrecoverably drops the GRAPH's sighted state
		// (the WPNSightedState node), which breaks the ADS pose and See
		// Through Scopes' overlay; nothing injected afterward re-drives it
		// (NotifyAnimationGraph is a graph INPUT that Papyrus listeners
		// never see; iSyncSightedState did not take). So we do not disturb
		// sights at all: the parkour KEYWORD is already tagged above, so an
		// OAR config can animate the vault from the sighted state (track-
		// filtered to the off arm) with the sighted state — and STS —
		// untouched throughout.
		const int gs = static_cast<int>(a_player->gunState);
		if (vault && (gs == 6 || gs == 8)) {
			logger::info("[AnimHijack] Sighted vault - weapon stays aimed, no special idle (OAR keyword drives the anim)");
			return;
		}

		const bool slot2 = vault ? settings->testIdle2Vault[a_tier] : settings->testIdle2Mantle[a_tier];
		const bool slot1 = vault ? settings->testIdleVault[a_tier] : settings->testIdleMantle[a_tier];

		// Automatic weapon-away idles: with the weapon sheathed (hands
		// free), a vault plays Vault.hkx at ANY height, a low/mid mantle
		// plays Mantle.hkx, and a HIGH mantle plays Ledge.hkx (the
		// authored-curve climb — the mover drives the player along that
		// clip's exported root trajectory in lockstep) — all from the
		// same folder as the test idles. Any ticked debug test-idle slot
		// for this move/tier OVERRIDES these (the debug tools stay
		// authoritative).
		const bool weaponAway = a_player->weaponState == RE::WEAPON_STATE::kSheathed;
		const bool autoIdle = !slot2 && !slot1 && weaponAway;
		std::string autoPath;
		if (autoIdle) {
			const std::string& base = settings->testIdlePath;
			const auto cut = base.find_last_of("\\/");
			autoPath = (cut == std::string::npos)
				? std::string("Meshes\\Actors\\Character\\Animations\\F4Parkour\\")
				: base.substr(0, cut + 1);
			autoPath += vault ? "Vault.hkx" : (a_tier == 2 ? "Ledge.hkx" : "Mantle.hkx");
		}

		if (testIdle && (slot2 || slot1 || autoIdle)) {
			// File-based dynamic idles route through a dyn_* anim event on
			// the graph; an empty event plays nothing. SetupSpecialIdle is
			// the exact engine path weapon-inspect mods ride for their
			// FIRST-person animations, so FP playback works — the .hkx just
			// has to be authored for the first-person rig to look right
			// there. testConditions=false skips the idle-condition check
			// (one less refusal path for a test tool).
			const std::string& path = autoIdle ? autoPath :
				slot2 ? settings->testIdle2Path : settings->testIdlePath;
			const std::string& evt  = (autoIdle || !slot2) ? settings->testIdleEvent : settings->testIdle2Event;
			testIdle->animFileName = path.c_str();
			testIdle->animEventName = evt.c_str();
			testIdle->behaviorGraphName = settings->testIdleBehavior.c_str();
			testIdle->data.loopMin = 0;
			testIdle->data.loopMax = 0;
			testIdle->data.flags = 0;
			testIdle->data.replayDelay = 0;
			bool played = false;
			if (a_player->currentProcess) {
				played = a_player->currentProcess->SetupSpecialIdle(
					*a_player, RE::DEFAULT_OBJECT::kActionIdle, testIdle, false, a_player);
			}
			idleRetryPending = !played;
			if (!played) {
				// State snapshot so the log says WHY the engine refused.
				std::uint32_t hkState = 999;
				if (a_player->currentProcess && a_player->currentProcess->middleHigh &&
					a_player->currentProcess->middleHigh->charController) {
					hkState = static_cast<std::uint32_t>(
						a_player->currentProcess->middleHigh->charController->context.m_currentState);
				}
				logger::warn(
					"[AnimHijack] {} idle REFUSED '{}' (event '{}') - gunState={} meleeState={} hkState={} weaponDrawn={}",
					autoIdle ? "Auto" : (slot2 ? "Test slot 2" : "Test slot 1"), path, evt,
					static_cast<std::uint32_t>(a_player->gunState),
					static_cast<std::uint32_t>(a_player->meleeAttackState),
					hkState,
					static_cast<std::uint32_t>(a_player->weaponState));
			} else {
				logger::info("[AnimHijack] {} idle '{}' (event '{}') -> true",
					autoIdle ? "Auto" : (slot2 ? "Test slot 2" : "Test slot 1"),
					path, evt);
			}
		} else if (settings->playMeleeAnim && actionMelee) {
			const bool ok = a_player->PerformAction(actionMelee, nullptr);
			if (!ok) {
				logger::info("[AnimHijack] kActionMelee refused - move plays without animation");
			}
		}
	}

	void AnimHijack::RetryTestIdle(RE::PlayerCharacter* a_player)
	{
		if (!idleRetryPending || !testIdle || !a_player) return;
		idleRetryPending = false;

		bool played = false;
		if (a_player->currentProcess) {
			played = a_player->currentProcess->SetupSpecialIdle(
				*a_player, RE::DEFAULT_OBJECT::kActionIdle, testIdle, false, a_player);
		}
		logger::info("[AnimHijack] Test idle apex retry -> {}", played);
	}

	// ============================================================
	// Anim-graph event sink. The equip-skip that used to fire here on
	// "IdleStop" is RETIRED (2026-08-27): the post-idle fast-equip is
	// handled OAR-side now, and its UpdateAnimation(1000) fast-forward
	// integrated the huge delta into live player movement — the
	// post-vault teleport (position-pinning the call only reduced it).
	// ============================================================
	void AnimHijack::OnAnimEvent(const RE::BSFixedString& /*a_tag*/)
	{
		// No-op: the sighted-vault path no longer disturbs the sighted
		// state, so there is no stray sightedStateExit to counter. The
		// ProcessEvent hook stays installed for any future graph-event
		// needs.
	}

	void AnimHijack::OnMoveConverted(RE::PlayerCharacter* a_player, int a_mantleTier)
	{
		if (!a_player) return;
		a_mantleTier = std::clamp(a_mantleTier, 0, 2);
		RemoveKeyword(a_player, kwVault);
		for (int i = 0; i < 3; ++i) {
			RemoveKeyword(a_player, kwVaultTier[i]);
		}
		AddKeyword(a_player, kwMantle);
		AddKeyword(a_player, kwMantleTier[a_mantleTier]);
		static const RE::BSFixedString kEvtConvert{ "F4Parkour_Convert" };
		a_player->NotifyAnimationGraphImpl(kEvtConvert);
	}

	void AnimHijack::OnMoveEnd(RE::PlayerCharacter* a_player, MoveKind a_kind)
	{
		(void)a_kind;
		idleRetryPending = false;
		if (!a_player) return;
		RemoveAll(a_player);
		SetGraphVars(a_player, MoveKind::None, 0.0f);
		static const RE::BSFixedString kEvtEnd{ "F4Parkour_End" };
		a_player->NotifyAnimationGraphImpl(kEvtEnd);
		// No ADS restore needed: a sighted vault never dropped sights (we
		// skip the special idle for it), so the sighted state and See
		// Through Scopes were never disturbed.
	}

	void AnimHijack::RequestSneak(RE::PlayerCharacter* a_player)
	{
		// Synthetic Sneak press+release through the engine's own handler —
		// the only mechanism confirmed (CrouchSlide work) to actually play
		// the crouch. Never write the sneak bool directly.
		auto* pc = RE::PlayerControls::GetSingleton();
		if (!pc || !pc->sneakHandler) return;
		if (a_player->IsSneaking()) return;

		auto fill = [](RE::ButtonEvent& a_evt, float a_value, float a_held) {
			SyntheticInput::InitializeButtonEvent(a_evt);
			a_evt.device = RE::INPUT_DEVICE::kKeyboard;
			a_evt.deviceID = 0;
			a_evt.eventType = RE::INPUT_EVENT_TYPE::kButton;
			a_evt.next = nullptr;
			a_evt.timeCode = 0;
			a_evt.handled = RE::InputEvent::HANDLED_RESULT::kUnhandled;
			a_evt.strUserEvent = RE::BSFixedString("Sneak");
			a_evt.idCode = 0;
			a_evt.disabled = false;
			a_evt.value = a_value;
			a_evt.heldDownSecs = a_held;
		};

		// Dispatch through the handler's live vtable (slot 8) so other
		// mods' hooks on SneakHandler see a normal press.
		using FnHandleButton = void (*)(void*, const RE::ButtonEvent*);
		uintptr_t vtable = *reinterpret_cast<uintptr_t*>(pc->sneakHandler);
		auto handle = *reinterpret_cast<FnHandleButton*>(vtable + 8 * sizeof(void*));
		if (!handle) return;

		RE::ButtonEvent press;
		fill(press, 1.0f, 0.0f);
		handle(pc->sneakHandler, &press);
		RE::ButtonEvent release;
		fill(release, 0.0f, 0.01f);
		handle(pc->sneakHandler, &release);

		logger::info("[AnimHijack] Crouch-only mantle - synthetic sneak dispatched");
	}
}
