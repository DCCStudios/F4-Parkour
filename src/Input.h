#pragma once

namespace F4Parkour
{
	// ============================================================
	// Jump input integration: a slot-8 vtable patch on
	// JumpHandler::HandleEvent(ButtonEvent*) — the proven FPGO
	// SprintHandler/SneakHandler pattern. Contextual rule: a jump press
	// with a valid candidate (and forward intent) becomes a parkour
	// move and the engine never sees the jump; otherwise it passes
	// through untouched.
	//
	// Jump assist (Dying Light): presses with no candidate are buffered
	// for Settings::jumpBufferWindow and consumed the moment detection
	// produces one; a press within Settings::coyoteWindow of walking
	// off an edge still counts as grounded.
	// ============================================================
	namespace Input
	{
		// Install the JumpHandler vtable patch. Call once at
		// kGameDataReady (PlayerControls exists by then).
		void Install();

		// Per-frame bookkeeping (buffer aging, coyote tracking, hold
		// state). Called from ParkourManager::Update on the main thread.
		void Update(float a_dt);

		// True once if a buffered jump press is waiting (consumes it).
		bool ConsumeBufferedPress();

		// True while the jump key is physically held (hold-to-mantle).
		bool JumpHeld();

		// Seconds since the player was last grounded (coyote time).
		float TimeSinceGrounded();

		// Seconds since a jump press actually REACHED THE ENGINE (real
		// pass-through or synthetic refund). Distinguishes a genuinely
		// launched jump from a kInAir controller-state flicker on slopes
		// when classifying a move as an air start.
		float TimeSinceEngineJump();

		// Swallow jump PRESSES while a move is in progress (releases and
		// holds always pass through so the engine's press/held state
		// machine stays consistent).
		void SetSuppressed(bool a_suppressed);

		// Refund a swallowed press as a synthetic engine jump — used when
		// a hook-consumed press fails to start a move, so the input never
		// silently vanishes.
		void ForwardJumpTap();
	}
}
