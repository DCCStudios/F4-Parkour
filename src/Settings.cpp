#include "PCH.h"
#include "Settings.h"

namespace F4Parkour
{
	void Settings::Load()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(kINIPath) < 0) {
			logger::info("[Settings] {} not found - using defaults (it will be written on first save)", kINIPath);
			return;
		}

		auto getB = [&](const char* s, const char* k, bool d) { return ini.GetBoolValue(s, k, d); };
		auto getF = [&](const char* s, const char* k, float d) { return static_cast<float>(ini.GetDoubleValue(s, k, d)); };

		enabled           = getB("General", "bEnabled", enabled);
		playMeleeAnim     = getB("General", "bPlayMeleeAnim", playMeleeAnim);
		autoParkourSprint = getB("General", "bAutoParkourWhileSprinting", autoParkourSprint);
		autoStepUp        = getB("General", "bAutoStepUp", autoStepUp);
		allowInAir        = getB("General", "bAllowInAir", allowInAir);
		allowAirVault     = getB("General", "bAllowAirVault", allowAirVault);
		autoAirGrab       = getB("General", "bAutoAirGrab", autoAirGrab);
		airAutoGrabDelay  = getF("General", "fAirAutoGrabDelay", airAutoGrabDelay);
		highMantleCameraDirector = getB("General", "bHighMantleCameraDirector", highMantleCameraDirector);
		camCollisionSkin  = getF("General", "fCamCollisionSkin", camCollisionSkin);
		indicatorEnabled  = getB("General", "bIndicatorEnabled", indicatorEnabled);
		allowThirdPerson  = getB("General", "bAllowThirdPerson", allowThirdPerson);
		requireForward    = getB("General", "bRequireForwardInput", requireForward);
		lookConeDeg       = getF("General", "fLookConeDegrees", lookConeDeg);
		detectionInterval = getF("General", "fDetectionInterval", detectionInterval);
		activePreset      = ini.GetValue("General", "sActivePreset", activePreset.c_str());

		jumpBufferWindow  = getF("Input", "fJumpBufferWindow", jumpBufferWindow);
		coyoteWindow      = getF("Input", "fCoyoteWindow", coyoteWindow);
		holdToMantle      = getB("Input", "bHoldToMantle", holdToMantle);

		minVaultHeight      = getF("Detection", "fMinVaultHeight", minVaultHeight);
		maxVaultHeight      = getF("Detection", "fMaxVaultHeight", maxVaultHeight);
		minMantleHeight     = getF("Detection", "fMinMantleHeight", minMantleHeight);
		maxMantleHeight     = getF("Detection", "fMaxMantleHeight", maxMantleHeight);
		minMantleDepth      = getF("Detection", "fMinMantleDepth", minMantleDepth);
		minBackClearance    = getF("Detection", "fMinBackClearance", minBackClearance);
		maxVaultDrop        = getF("Detection", "fMaxVaultDrop", maxVaultDrop);
		maxApproachAngleDeg = getF("Detection", "fMaxApproachAngle", maxApproachAngleDeg);
		autoEngageDistance  = getF("Detection", "fAutoEngageDistance", autoEngageDistance);
		airGrabExtraReach   = getF("Detection", "fAirGrabExtraReach", airGrabExtraReach);

		momentumKeep       = getF("Movement", "fMomentumKeep", momentumKeep);
		vaultSpeedMatch    = getF("Movement", "fVaultSpeedMatch", vaultSpeedMatch);
		exitDirBlend       = getF("Movement", "fExitDirectionBlend", exitDirBlend);
		momentumDropCutoff = getF("Movement", "fMomentumDropCutoff", momentumDropCutoff);
		controlHandback    = getF("Movement", "fControlHandbackFraction", controlHandback);
		sneakOnCrouchOnly  = getB("Movement", "bSneakOnCrouchOnlyMantle", sneakOnCrouchOnly);

		focusDot      = getB("Comfort", "bFocusDot", focusDot);
		focusDotAlpha = getF("Comfort", "fFocusDotAlpha", focusDotAlpha);

		testIdleVault[0]  = getB("Testing", "bTestIdleVaultLow", testIdleVault[0]);
		testIdleVault[1]  = getB("Testing", "bTestIdleVaultMid", testIdleVault[1]);
		testIdleVault[2]  = getB("Testing", "bTestIdleVaultHigh", testIdleVault[2]);
		testIdleMantle[0] = getB("Testing", "bTestIdleMantleLow", testIdleMantle[0]);
		testIdleMantle[1] = getB("Testing", "bTestIdleMantleMid", testIdleMantle[1]);
		testIdleMantle[2] = getB("Testing", "bTestIdleMantleHigh", testIdleMantle[2]);
		testIdlePath      = ini.GetValue("Testing", "sTestIdlePath", testIdlePath.c_str());
		testIdleEvent     = ini.GetValue("Testing", "sTestIdleEvent", testIdleEvent.c_str());
		testIdleBehavior  = ini.GetValue("Testing", "sTestIdleBehavior", testIdleBehavior.c_str());

		testIdle2Vault[0]  = getB("Testing", "bTestIdle2VaultLow", testIdle2Vault[0]);
		testIdle2Vault[1]  = getB("Testing", "bTestIdle2VaultMid", testIdle2Vault[1]);
		testIdle2Vault[2]  = getB("Testing", "bTestIdle2VaultHigh", testIdle2Vault[2]);
		testIdle2Mantle[0] = getB("Testing", "bTestIdle2MantleLow", testIdle2Mantle[0]);
		testIdle2Mantle[1] = getB("Testing", "bTestIdle2MantleMid", testIdle2Mantle[1]);
		testIdle2Mantle[2] = getB("Testing", "bTestIdle2MantleHigh", testIdle2Mantle[2]);
		testIdle2Path      = ini.GetValue("Testing", "sTestIdle2Path", testIdle2Path.c_str());
		testIdle2Event     = ini.GetValue("Testing", "sTestIdle2Event", testIdle2Event.c_str());

		debugEnabled    = getB("Debug", "bDebugEnabled", debugEnabled);
		drawRays        = getB("Debug", "bDrawRays", drawRays);
		drawPath        = getB("Debug", "bDrawPath", drawPath);
		drawOnlyFailed  = getB("Debug", "bDrawOnlyFailed", drawOnlyFailed);
		freezeDetection = getB("Debug", "bFreezeDetection", freezeDetection);
		moverTimeScale  = getF("Debug", "fMoverTimeScale", moverTimeScale);
		watchdogEnabled = getB("Debug", "bWatchdogEnabled", watchdogEnabled);
		warpSceneEachFrame = getB("Debug", "bWarpSceneEachFrame", warpSceneEachFrame);

		// Clamps for values that feed math directly.
		detectionInterval   = std::clamp(detectionInterval, 0.016f, 0.5f);
		lookConeDeg         = std::clamp(lookConeDeg, 5.0f, 90.0f);
		jumpBufferWindow    = std::clamp(jumpBufferWindow, 0.0f, 0.6f);
		coyoteWindow        = std::clamp(coyoteWindow, 0.0f, 0.5f);
		momentumKeep        = std::clamp(momentumKeep, 0.0f, 1.5f);
		vaultSpeedMatch     = std::clamp(vaultSpeedMatch, 0.0f, 1.0f);
		exitDirBlend        = std::clamp(exitDirBlend, 0.0f, 1.0f);
		controlHandback     = std::clamp(controlHandback, 0.0f, 0.6f);
		maxApproachAngleDeg = std::clamp(maxApproachAngleDeg, 10.0f, 90.0f);
		moverTimeScale      = std::clamp(moverTimeScale, 0.05f, 1.0f);
		focusDotAlpha       = std::clamp(focusDotAlpha, 0.05f, 1.0f);
		minVaultHeight      = std::clamp(minVaultHeight, 20.0f, 200.0f);
		maxVaultHeight      = std::clamp(maxVaultHeight, minVaultHeight + 5.0f, 250.0f);
		minMantleHeight     = std::clamp(minMantleHeight, 20.0f, 200.0f);
		maxMantleHeight     = std::clamp(maxMantleHeight, minMantleHeight + 5.0f, 300.0f);

		logger::info("[Settings] Loaded (enabled={}, preset='{}')", enabled, activePreset);
	}

	void Settings::Save()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(kINIPath);  // keep unknown keys/comments where possible

		auto setB = [&](const char* s, const char* k, bool v) { ini.SetBoolValue(s, k, v); };
		auto setF = [&](const char* s, const char* k, float v) { ini.SetDoubleValue(s, k, v); };

		setB("General", "bEnabled", enabled);
		setB("General", "bPlayMeleeAnim", playMeleeAnim);
		setB("General", "bAutoParkourWhileSprinting", autoParkourSprint);
		setB("General", "bAutoStepUp", autoStepUp);
		setB("General", "bAllowInAir", allowInAir);
		setB("General", "bAllowAirVault", allowAirVault);
		setB("General", "bAutoAirGrab", autoAirGrab);
		setF("General", "fAirAutoGrabDelay", airAutoGrabDelay);
		setB("General", "bHighMantleCameraDirector", highMantleCameraDirector);
		setF("General", "fCamCollisionSkin", camCollisionSkin);
		setB("General", "bIndicatorEnabled", indicatorEnabled);
		setB("General", "bAllowThirdPerson", allowThirdPerson);
		setB("General", "bRequireForwardInput", requireForward);
		setF("General", "fLookConeDegrees", lookConeDeg);
		setF("General", "fDetectionInterval", detectionInterval);
		ini.SetValue("General", "sActivePreset", activePreset.c_str());

		setF("Input", "fJumpBufferWindow", jumpBufferWindow);
		setF("Input", "fCoyoteWindow", coyoteWindow);
		setB("Input", "bHoldToMantle", holdToMantle);

		setF("Detection", "fMinVaultHeight", minVaultHeight);
		setF("Detection", "fMaxVaultHeight", maxVaultHeight);
		setF("Detection", "fMinMantleHeight", minMantleHeight);
		setF("Detection", "fMaxMantleHeight", maxMantleHeight);
		setF("Detection", "fMinMantleDepth", minMantleDepth);
		setF("Detection", "fMinBackClearance", minBackClearance);
		setF("Detection", "fMaxVaultDrop", maxVaultDrop);
		setF("Detection", "fMaxApproachAngle", maxApproachAngleDeg);
		setF("Detection", "fAutoEngageDistance", autoEngageDistance);
		setF("Detection", "fAirGrabExtraReach", airGrabExtraReach);

		setF("Movement", "fMomentumKeep", momentumKeep);
		setF("Movement", "fVaultSpeedMatch", vaultSpeedMatch);
		setF("Movement", "fExitDirectionBlend", exitDirBlend);
		setF("Movement", "fMomentumDropCutoff", momentumDropCutoff);
		setF("Movement", "fControlHandbackFraction", controlHandback);
		setB("Movement", "bSneakOnCrouchOnlyMantle", sneakOnCrouchOnly);

		setB("Comfort", "bFocusDot", focusDot);
		setF("Comfort", "fFocusDotAlpha", focusDotAlpha);

		setB("Testing", "bTestIdleVaultLow", testIdleVault[0]);
		setB("Testing", "bTestIdleVaultMid", testIdleVault[1]);
		setB("Testing", "bTestIdleVaultHigh", testIdleVault[2]);
		setB("Testing", "bTestIdleMantleLow", testIdleMantle[0]);
		setB("Testing", "bTestIdleMantleMid", testIdleMantle[1]);
		setB("Testing", "bTestIdleMantleHigh", testIdleMantle[2]);
		ini.SetValue("Testing", "sTestIdlePath", testIdlePath.c_str());
		ini.SetValue("Testing", "sTestIdleEvent", testIdleEvent.c_str());
		ini.SetValue("Testing", "sTestIdleBehavior", testIdleBehavior.c_str());

		setB("Testing", "bTestIdle2VaultLow", testIdle2Vault[0]);
		setB("Testing", "bTestIdle2VaultMid", testIdle2Vault[1]);
		setB("Testing", "bTestIdle2VaultHigh", testIdle2Vault[2]);
		setB("Testing", "bTestIdle2MantleLow", testIdle2Mantle[0]);
		setB("Testing", "bTestIdle2MantleMid", testIdle2Mantle[1]);
		setB("Testing", "bTestIdle2MantleHigh", testIdle2Mantle[2]);
		ini.SetValue("Testing", "sTestIdle2Path", testIdle2Path.c_str());
		ini.SetValue("Testing", "sTestIdle2Event", testIdle2Event.c_str());

		setB("Debug", "bDebugEnabled", debugEnabled);
		setB("Debug", "bDrawRays", drawRays);
		setB("Debug", "bDrawPath", drawPath);
		setB("Debug", "bDrawOnlyFailed", drawOnlyFailed);
		setB("Debug", "bFreezeDetection", freezeDetection);
		setF("Debug", "fMoverTimeScale", moverTimeScale);
		setB("Debug", "bWatchdogEnabled", watchdogEnabled);
		setB("Debug", "bWarpSceneEachFrame", warpSceneEachFrame);

		if (ini.SaveFile(kINIPath) < 0) {
			logger::warn("[Settings] Failed to write {}", kINIPath);
		}
	}
}
