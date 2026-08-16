# F4Parkour

Code-driven vaulting and mantling for Fallout 4 (F4SE plugin).

- **Vault** — climb up and over an obstacle, landing on the far side. Momentum is preserved
  (sprint through a vault and keep running).
- **Mantle** — climb up and on top of a ledge. Momentum is cleared with a small settle step.

Both are triggered contextually by the **jump key**: hold forward, look at the obstacle, jump.
Releasing forward before the apex converts a vault into a mantle (your intent is "get on top").
Jumping again mid-air grabs and mantles ledges (air grab). Everything up to roughly eye height
(~155 units) is climbable; heights between the configurable tiers blend smoothly.

Design references: Brink's SMART system (GDC: *Vault, Slide, Mantle*) and Dying Light's Natural
Movement (GDC: *Parkour in 20 Simple Steps*) — transcripts and the implementation plan live in
`docs/`.

## Requirements

- Fallout 4 (OG / NG / AE — one DLL, multi-runtime CommonLib)
- [F4SE](https://f4se.silverlock.org/)
- [F4SE Menu Framework](https://www.nexusmods.com/fallout4/mods/) for the in-game config menu
- [Open Animation Replacer F4] *(optional)* for replacement animations — without it (or with
  "Play melee animation" off) the moves are pure camera/position movement

## How the animation hijack works

There is no behavior-graph edit anywhere. When a move starts, F4Parkour:

1. adds runtime keywords to the player — `AnimsParkourKeyword`, the kind keyword
   (`AnimsParkourVaultKeyword` / `AnimsParkourMantleKeyword`), and a height-tier keyword so
   each tier can get its own animation: `AnimsParkourVaultLowKeyword` / `...VaultMidKeyword` /
   `...VaultHighKeyword` and `AnimsParkourMantleLowKeyword` / `...MantleMidKeyword` /
   `...MantleHighKeyword` (nearest preset tier height). Graph variables set alongside:
   `F4Parkour_Type` (-1 none / 0 vault / 1 mantle) and `F4Parkour_Height` (game units);
2. fires the vanilla **melee action** (`ActionMelee` via `PerformAction`);
3. OAR conditions (`HasKeyword`) swap the melee clip for a vault/mantle animation.

The keywords are armed *before* the action fires because OAR evaluates conditions at clip
activation. Movement never depends on the animation: if the action is refused (graph busy), the
move plays without it. Example OAR configs live in `OAR_TestConfigs/F4Parkour_Anims/`.

## Menu

F4SE Menu Framework section **F4Parkour**:

- **General** — enable, melee-anim toggle, contextual-trigger tuning (look cone, jump buffer,
  coyote time, hold-to-mantle), auto-vault/auto-step-up (default off), focus dot.
- **Movement & Curves** — feel presets (Smooth / Snappy / Deliberate / Arcade shipped; saved as
  JSON under `Data/F4SE/Plugins/F4Parkour/Presets/`), tier heights and times, momentum options,
  and the curve editors. Progress curves are kept monotonic — the camera never moves backward
  during a forward move (a real nausea trigger, per Techland).
- **Debugging** — world-space ray overlay (green pass / red fail / grey miss, labeled with
  distance + collision layer), planned-path arc, live state readout, a plain-language decision
  log ("mantle rejected: top depth 12 < 25"), freeze detection, slow-motion mover, and a
  non-pausing popout window.

## Building

```
xmake f -p windows -a x64 -m releasedbg
xmake
```

Uses the multi-runtime CommonLibF4 fork checked out in
`../FPGunplayOverhaul/lib/commonlibf4` (keep the two repos side by side). Output lands in
`Compile/F4SE/Plugins/`. The legacy CMake build was archived to `docs/legacy-cmake/`.

## Compatibility notes

- Blocked while sighted/ADS, in Power Armor, dead, or in menus.
- Crouch-only mantles (headroom fits a crouched capsule but not standing) finish by entering
  sneak through the normal input path, so mods that manage the crouch collision capsule
  (UneducatedShooter) keep working untouched.
- The jump hook is a vtable patch on `JumpHandler::HandleEvent` — jump behaves identically
  whenever no candidate ledge is in front of you.
