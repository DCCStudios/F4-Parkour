#include "PCH.h"
#include "Input.h"
#include "Detection.h"
#include "ParkourManager.h"
#include "Settings.h"
#include "SyntheticInput.h"

namespace
{
	using FnHandleButton = void (*)(void*, const RE::ButtonEvent*);

	FnHandleButton s_originalHandleButton = nullptr;
	bool           s_installed = false;

	std::atomic<bool>  s_suppressed{ false };      // move in progress: eat jump presses
	std::atomic<bool>  s_bufferedPress{ false };   // press waiting for a candidate
	std::atomic<float> s_bufferAge{ 999.0f };
	std::atomic<bool>  s_jumpHeld{ false };
	std::atomic<float> s_sinceGrounded{ 999.0f };
	std::atomic<float> s_sinceEngineJump{ 999.0f };
	std::atomic<bool>  s_dispatchingSynthetic{ false };

	void HookedHandleButton(void* a_self, const RE::ButtonEvent* a_event)
	{
		if (a_event && !s_dispatchingSynthetic.load(std::memory_order_relaxed)) {
			const auto* userEvent = a_event->QUserEvent().c_str();
			const bool isJump = userEvent && std::strcmp(userEvent, "Jump") == 0;
			if (isJump) {
				if (a_event->value > 0.0f) {
					s_jumpHeld.store(true, std::memory_order_relaxed);
				} else if (a_event->QReleased()) {
					s_jumpHeld.store(false, std::memory_order_relaxed);
				}

				// Only PRESSES are ever swallowed. Releases and holds always
				// reach the engine so its press/held state machine can never
				// be left dangling by a move that started mid-press.
				if (a_event->QJustPressed()) {
					if (s_suppressed.load(std::memory_order_relaxed)) {
						return;  // a move owns the jump key
					}
					// Contextual rule, pre-evaluated each detection tick by
					// the manager so this stays cheap and synchronous.
					if (F4Parkour::ParkourManager::GetSingleton()->JumpWouldParkour()) {
						F4Parkour::ParkourManager::GetSingleton()->RequestActivation();
						return;  // the engine never sees this jump
					}
					// No candidate right now: let the jump through and
					// buffer the press — detection may produce an air-grab
					// a few frames into the jump (Dying Light jump assist).
					s_sinceEngineJump.store(0.0f, std::memory_order_relaxed);
					s_bufferedPress.store(true, std::memory_order_relaxed);
					s_bufferAge.store(0.0f, std::memory_order_relaxed);
				}
			}
		}

		if (s_originalHandleButton) {
			s_originalHandleButton(a_self, a_event);
		}
	}
}

namespace F4Parkour::Input
{
	void Install()
	{
		if (s_installed) return;

		auto* pc = RE::PlayerControls::GetSingleton();
		if (!pc || !pc->jumpHandler) {
			logger::error("[Input] PlayerControls or JumpHandler is null - jump trigger disabled");
			return;
		}

		// BSInputEventUser single-inheritance: vtable pointer at object+0;
		// HandleEvent(ButtonEvent*) is slot 8 (the FPGO-proven layout).
		uintptr_t vtable = *reinterpret_cast<uintptr_t*>(pc->jumpHandler);
		constexpr uintptr_t kSlotOffset = 8 * sizeof(void*);
		uintptr_t addr = vtable + kSlotOffset;

		std::memcpy(&s_originalHandleButton, reinterpret_cast<void*>(addr), sizeof(void*));

		uintptr_t hookAddr = reinterpret_cast<uintptr_t>(&HookedHandleButton);
		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			logger::error("[Input] VirtualProtect failed - jump trigger disabled");
			s_originalHandleButton = nullptr;
			return;
		}
		std::memcpy(reinterpret_cast<void*>(addr), &hookAddr, sizeof(void*));
		VirtualProtect(reinterpret_cast<void*>(addr), sizeof(void*), oldProtect, &oldProtect);

		s_installed = true;
		logger::info("[Input] Hooked JumpHandler::HandleEvent(ButtonEvent*) - vtable=0x{:X}, slot=8, original=0x{:X}",
			vtable, reinterpret_cast<uintptr_t>(s_originalHandleButton));
	}

	void Update(float a_dt)
	{
		auto* settings = Settings::GetSingleton();

		if (s_bufferedPress.load(std::memory_order_relaxed)) {
			const float age = s_bufferAge.load(std::memory_order_relaxed) + a_dt;
			s_bufferAge.store(age, std::memory_order_relaxed);
			if (age > settings->jumpBufferWindow) {
				s_bufferedPress.store(false, std::memory_order_relaxed);
			}
		}

		s_sinceEngineJump.store(
			std::min(999.0f, s_sinceEngineJump.load(std::memory_order_relaxed) + a_dt),
			std::memory_order_relaxed);

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player && Detection::IsOnGround(player)) {
			s_sinceGrounded.store(0.0f, std::memory_order_relaxed);
		} else {
			s_sinceGrounded.store(
				s_sinceGrounded.load(std::memory_order_relaxed) + a_dt,
				std::memory_order_relaxed);
		}
	}

	bool ConsumeBufferedPress()
	{
		return s_bufferedPress.exchange(false, std::memory_order_relaxed);
	}

	bool JumpHeld()
	{
		return s_jumpHeld.load(std::memory_order_relaxed);
	}

	float TimeSinceGrounded()
	{
		return s_sinceGrounded.load(std::memory_order_relaxed);
	}

	float TimeSinceEngineJump()
	{
		return s_sinceEngineJump.load(std::memory_order_relaxed);
	}

	void SetSuppressed(bool a_suppressed)
	{
		s_suppressed.store(a_suppressed, std::memory_order_relaxed);
	}

	void ForwardJumpTap()
	{
		// Refund path: a press the hook swallowed turned out not to start
		// a move (path blocked between detection ticks, preconditions
		// collapsed). Give the engine the jump it was owed so the input
		// never just vanishes — the CrouchSlide synthetic-sneak pattern.
		auto* pc = RE::PlayerControls::GetSingleton();
		if (!pc || !pc->jumpHandler || !s_originalHandleButton) return;

		auto fill = [](RE::ButtonEvent& a_evt, float a_value, float a_held) {
			SyntheticInput::InitializeButtonEvent(a_evt);
			a_evt.device = RE::INPUT_DEVICE::kKeyboard;
			a_evt.deviceID = 0;
			a_evt.eventType = RE::INPUT_EVENT_TYPE::kButton;
			a_evt.next = nullptr;
			a_evt.timeCode = 0;
			a_evt.handled = RE::InputEvent::HANDLED_RESULT::kUnhandled;
			a_evt.strUserEvent = RE::BSFixedString("Jump");
			a_evt.idCode = 0;
			a_evt.disabled = false;
			a_evt.value = a_value;
			a_evt.heldDownSecs = a_held;
		};

		s_dispatchingSynthetic.store(true, std::memory_order_relaxed);
		RE::ButtonEvent press;
		fill(press, 1.0f, 0.0f);
		s_originalHandleButton(pc->jumpHandler, &press);
		RE::ButtonEvent release;
		fill(release, 0.0f, 0.01f);
		s_originalHandleButton(pc->jumpHandler, &release);
		s_dispatchingSynthetic.store(false, std::memory_order_relaxed);
		s_sinceEngineJump.store(0.0f, std::memory_order_relaxed);

		logger::info("[Input] Swallowed jump refunded as a synthetic tap");
	}
}
