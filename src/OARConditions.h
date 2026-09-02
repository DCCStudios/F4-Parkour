#pragma once

// ============================================================
// Custom OAR conditions for F4Parkour.
//
// Replaces the old keyword + HasKeyword(editorID) gating. A DLL-created
// keyword is resolved by OAR ONCE at config-parse time and cached; if OAR
// parses before the keyword exists (init-order dependent) the condition is
// dead for the whole session. Registering named conditions through OAR's
// plugin API sidesteps that entirely: OAR binds them by name, there is no
// editorID to resolve, and no plugin/ESP is needed.
//
// Each condition is a player-only boolean backed by an atomic the Mover
// flips at move start/end. Read on the anim thread during OAR evaluation,
// written on the game thread -> relaxed atomic, no lock.
// ============================================================

#include "OAR/OpenAnimationReplacerAPI-Conditions.h"

#include <array>
#include <atomic>
#include <memory>

namespace OARConditions
{
	// One slot per parkour keyword the AnimHijack used to add. Order is
	// load-bearing: the tier slots are consecutive so SetMove can index them
	// as (kVaultLow + tier) / (kMantleLow + tier).
	enum State : std::size_t
	{
		kAnyMove = 0,
		kVault,
		kMantle,
		kVaultLow,
		kVaultMid,
		kVaultHigh,
		kMantleLow,
		kMantleMid,
		kMantleHigh,
		kCount
	};

	inline std::array<std::atomic<bool>, kCount> g_state{};
	inline bool g_oarAvailable{ false };

#define PARKOUR_COND(ClassName, STATE, NAME, DESC)                                            \
	class ClassName final : public OAR::ConditionBase                                         \
	{                                                                                          \
	public:                                                                                    \
		std::string GetName() const override { return NAME; }                                 \
		std::string GetDescription() const override { return DESC; }                          \
                                                                                              \
	protected:                                                                                 \
		bool EvaluateImpl(RE::TESObjectREFR* a_refr, RE::hkbClipGenerator*, const OAR::SubMod*) \
			const override                                                                    \
		{                                                                                      \
			return a_refr && a_refr == RE::PlayerCharacter::GetSingleton() &&                  \
				g_state[STATE].load(std::memory_order_relaxed);                               \
		}                                                                                      \
		void InitializeImpl(const nlohmann::json&) override {}                                 \
		void SerializeImpl(nlohmann::json&) const override {}                                  \
	};

	PARKOUR_COND(AnyMoveCond, kAnyMove, "Parkour_IsAnyMove",
		"True while any F4Parkour vault/mantle is active on the player.")
	PARKOUR_COND(VaultCond, kVault, "Parkour_IsVault", "True while a vault is active.")
	PARKOUR_COND(MantleCond, kMantle, "Parkour_IsMantle", "True while a mantle is active.")
	PARKOUR_COND(VaultLowCond, kVaultLow, "Parkour_IsVaultLow", "True while a low vault is active.")
	PARKOUR_COND(VaultMidCond, kVaultMid, "Parkour_IsVaultMid", "True while a mid vault is active.")
	PARKOUR_COND(VaultHighCond, kVaultHigh, "Parkour_IsVaultHigh", "True while a high vault is active.")
	PARKOUR_COND(MantleLowCond, kMantleLow, "Parkour_IsMantleLow", "True while a low mantle is active.")
	PARKOUR_COND(MantleMidCond, kMantleMid, "Parkour_IsMantleMid", "True while a mid mantle is active.")
	PARKOUR_COND(MantleHighCond, kMantleHigh, "Parkour_IsMantleHigh", "True while a high mantle is active.")

#undef PARKOUR_COND

	// Register at kPostLoad (OAR's DLL is loaded, but it has not parsed configs
	// yet). Factories must be plain function pointers, so non-capturing lambdas.
	inline void RegisterConditions()
	{
		auto* api = OAR::Conditions::GetAPI();
		g_oarAvailable = (api != nullptr);
		if (!api) {
			logger::info("[F4Parkour][OAR] conditions API unavailable at kPostLoad "
						 "(OpenAnimationReplacer missing?) - custom conditions inactive");
			return;
		}

		struct Reg
		{
			const char* name;
			OAR::Conditions::ConditionFactoryFn factory;
		};
		static const Reg kRegs[] = {
			{ "Parkour_IsAnyMove", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<AnyMoveCond>(); } },
			{ "Parkour_IsVault", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<VaultCond>(); } },
			{ "Parkour_IsMantle", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<MantleCond>(); } },
			{ "Parkour_IsVaultLow", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<VaultLowCond>(); } },
			{ "Parkour_IsVaultMid", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<VaultMidCond>(); } },
			{ "Parkour_IsVaultHigh", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<VaultHighCond>(); } },
			{ "Parkour_IsMantleLow", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<MantleLowCond>(); } },
			{ "Parkour_IsMantleMid", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<MantleMidCond>(); } },
			{ "Parkour_IsMantleHigh", []() -> std::unique_ptr<OAR::ICondition> { return std::make_unique<MantleHighCond>(); } },
		};

		int ok = 0;
		for (const auto& r : kRegs) {
			const auto result = api->RegisterCondition(r.name, r.factory);
			if (result == OAR::Conditions::APIResult::OK ||
				result == OAR::Conditions::APIResult::AlreadyRegistered) {
				++ok;
			} else {
				logger::warn("[F4Parkour][OAR] failed to register '{}' (result={})",
					r.name, static_cast<int>(result));
			}
		}
		logger::info("[F4Parkour][OAR] registered {}/{} custom conditions (API v{})",
			ok, static_cast<int>(std::size(kRegs)), api->GetAPIVersion());
	}

	inline void ClearAll()
	{
		for (auto& s : g_state) {
			s.store(false, std::memory_order_relaxed);
		}
	}

	// Mirror exactly the keywords AnimHijack armed for a move: AnyMove, the
	// kind, and the kind's tier slot. tier 0/1/2 (clamped).
	inline void SetMove(bool a_isVault, int a_tier)
	{
		const int tier = a_tier < 0 ? 0 : (a_tier > 2 ? 2 : a_tier);
		ClearAll();
		g_state[kAnyMove].store(true, std::memory_order_relaxed);
		if (a_isVault) {
			g_state[kVault].store(true, std::memory_order_relaxed);
			g_state[kVaultLow + tier].store(true, std::memory_order_relaxed);
		} else {
			g_state[kMantle].store(true, std::memory_order_relaxed);
			g_state[kMantleLow + tier].store(true, std::memory_order_relaxed);
		}
	}
}
