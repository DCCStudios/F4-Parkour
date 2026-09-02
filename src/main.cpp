#include "PCH.h"
#include "Settings.h"
#include "Curves.h"
#include "ParkourManager.h"
#include "Input.h"
#include "AnimHijack.h"
#include "Menu.h"
#include "OARConditions.h"

namespace Plugin
{
	static constexpr auto NAME    = "F4Parkour"sv;
	static constexpr auto VERSION = REL::Version{ 1, 0, 0 };
}

namespace
{
	void MessageCallback(F4SE::MessagingInterface::Message* msg)
	{
		if (!msg) return;

		switch (msg->type) {
		case F4SE::MessagingInterface::kPostLoad:
			// Every F4SE plugin has returned from Load; the menu framework
			// module is available regardless of load order.
			F4Parkour::Menu::Register();
			// OAR's DLL is loaded but has not parsed configs yet, so register
			// our custom conditions now - before any HasKeyword-style parse
			// race could matter. Replaces the DLL-created parkour keywords.
			OARConditions::RegisterConditions();
			break;

		case F4SE::MessagingInterface::kGameDataReady:
		{
			logger::info("[F4Parkour] kGameDataReady - initializing");
			auto* settings = F4Parkour::Settings::GetSingleton();
			settings->Load();
			F4Parkour::Presets::GetSingleton()->Init(settings->activePreset);
			F4Parkour::AnimHijack::GetSingleton()->Init();
			F4Parkour::Input::Install();
			F4Parkour::InstallFrameHook();
			break;
		}

		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			logger::info("[F4Parkour] Game loaded - resetting state");
			F4Parkour::ParkourManager::GetSingleton()->OnGameLoaded();
			F4Parkour::AnimHijack::GetSingleton()->OnGameLoaded();
			break;

		default:
			break;
		}
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* F4SE, F4SE::PluginInfo* info)
{
	info->infoVersion = F4SE::PluginInfo::kVersion;
	info->name        = Plugin::NAME.data();
	info->version     = 1;

	if (F4SE->IsEditor()) {
		return false;
	}

	const auto ver = F4SE->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* F4SE)
{
	F4SE::Init(F4SE, {
		.log = true,
		.logName = Plugin::NAME.data(),
		.trampoline = true,
		.trampolineSize = 128,
	});

	logger::info("{} v{}.{}.{} loading", Plugin::NAME,
		Plugin::VERSION[0], Plugin::VERSION[1], Plugin::VERSION[2]);
	logger::info("Runtime {} ({})",
		F4SE->RuntimeVersion().string(),
		REX::FModule::IsRuntimeOG() ? "OG" :
			REX::FModule::IsRuntimeNG() ? "NG" : "AE");

	auto* messaging = F4SE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(MessageCallback)) {
		logger::critical("[F4Parkour] Failed to register messaging listener");
		return false;
	}

	logger::info("[F4Parkour] Plugin loaded, waiting for kGameDataReady");
	return true;
}
