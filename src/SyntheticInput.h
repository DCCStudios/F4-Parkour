#pragma once

#include "RE/B/ButtonEvent.h"
#include "RE/IDs_VTABLE.h"

#include <cstdint>
#include <memory>

namespace SyntheticInput
{
	// CommonLib declares ButtonEvent with __declspec(novtable) because game
	// instances are normally created by the engine. Consequently, a local
	// ButtonEvent constructed by plugin code has a null vtable. Engine input
	// handlers call virtual methods such as QUserEvent(), so every synthetic
	// event must receive the runtime-selected game vtable before dispatch.
	inline void InitializeButtonEvent(RE::ButtonEvent& a_event)
	{
		*reinterpret_cast<std::uintptr_t*>(std::addressof(a_event)) =
			RE::VTABLE::ButtonEvent[0].address();
	}
}
