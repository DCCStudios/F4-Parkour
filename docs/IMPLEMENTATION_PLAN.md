# F4Parkour — Vault & Mantle Implementation Plan

**Scope:** Code-driven vaulting and mantling for Fallout 4 (F4SE plugin), first- and third-person,
with an F4SE Menu Framework config UI, JSON curve/timing presets, OAR-based animation hijack of the
melee action via a runtime keyword, and a dedicated debug page with in-world overlays.

**Definitions (as agreed):**
- **Vault** — climb up and *over* an obstacle, landing on the far side. Momentum is preserved.
- **Mantle** — climb up and *on top of* an obstacle or ledge. Momentum is cleared (small settle step).

**Decisions locked in with the user:**
- Runtime keyword for OAR conditions, using the proven FPGunplayOverhaul SuperSprint pattern
  (`ConcreteFormFactory<BGSKeyword>::Create()` + `SetFormEditorID`, reuse if an ESP already defines it).
- F4SE Menu Framework only — delete the MCM folder. JSON presets + bootstrap INI.
- Contextual jump: forward input held + looking at ledge → parkour (jump swallowed); otherwise normal jump.
- No gameplay costs (AP/stamina) in v1; keep the hook points so a cost slider can be added later.
- Auto-parkour while sprinting: implemented, **default OFF**.
- Behavior-graph editing is out of scope — everything is C++ + OAR replacements of the melee clip.
- **No ledge-hang system** (Dying Light's hang/shimmy/side-hop states are explicitly out of scope).
  Every move here is a single committed vault or mantle that completes or cancels — there is no
  suspended state. Dying Light lessons are borrowed only where they apply to instant moves (jump
  assist, forward-vector pathing, camera comfort, early control hand-back), not their grab
  mechanics. Consequence for detection: geometry a hang system would accept (ledges with no body
  room on top, e.g. `topDepth` below the mantle minimum with no vault landing) is simply rejected
  rather than becoming a hang.

---

## 1. What we already have (asset inventory)

| Piece | Where | State |
|---|---|---|
| Plugin skeleton, messaging, logging, INI hot-reload | `F4Parkour/src/main.cpp`, `Settings.*` | Working |
| Detection logic ported from SkyParkourNG (vault sweep, mantle wall/ledge rays, headroom) | `F4Parkour/src/Parkouring.cpp` | Compiles, **untested — raycast is a stub** |
| Physics raycast | `F4Parkour/src/ParkourUtility.cpp` | **STUB — the one real blocker** |
| Proven FO4 raycast (`bhkPickData` + `TESObjectCELL::Pick`, layer allow-list, actor-skip re-cast, fraction sanity) | `FPGunplayOverhaul/src/ContextualLean.cpp` (anon namespace, ~lines 20–270) | Battle-tested in game |
| Runtime keyword creation + player attach/detach | `FPGunplayOverhaul` SuperSprint (`Inertia.cpp InitSuperSprint`, ~line 6956), same pattern in ADSReload / CrouchSlide | Battle-tested |
| Synthetic input dispatch (vtable-correct `ButtonEvent`, receiver dispatch through PlayerCamera's `BSInputEventReceiver`) | `FPGunplayOverhaul/src/SyntheticInput.h` + ContextualLean dispatch code | Battle-tested |
| Input handler vtable patches (SprintHandler/SneakHandler slot-8 allocate-copy-swap) | `FPGunplayOverhaul` SuperSprint / CrouchSlide | Battle-tested — model for the JumpHandler hook |
| F4SEMF menu: tabs (`AddSectionItem`), non-pausing popouts (`AddWindow`), input callback, foreground drawlist HUD | `FPGunplayOverhaul/src/Menu.*`, `F4Parkour/src/HUD.cpp` | Working |
| OAR F4 (in workspace) — `HasKeyword`, `HasGraphVariable`, perspective conditions; redistributable Clips API header | `OpenAnimationReplacer/`, `FPGunplayOverhaul/src/OpenAnimationReplacerAPI-Clips.h` | Available |
| Anim event sink on the player's graph sources | `F4Parkour/src/AnimEventSink.*` | Working pattern |
| SkyParkourNG reference (obstruction-behind-ledge, railing/thin checks, door exclusion, kNoSim usage, water check) | `PluginTemplate/skyrim-SkyParkourNG-3/` | Reference only |

**Design references distilled:**
- *Brink (GDC, slides PDF + full transcript in `docs/Building Brinks Smart System.md`):* classify by
  ledge height relative to player height (vault ≈ 0.4–0.8×, mantle ≈ 0.8–1.4×, air-grab up to mantle +
  jump height); mutual exclusion by distance (prefer mantle when close, vault when approaching with
  room); sort candidates by closest ledge; intro/exit physics states with spline paths, duration from
  distance + velocity; vault exit keeps momentum — with the *player influencing exit direction*, which
  is what made movement feel fluid in playtests — and clears it only when the far-side drop is too high
  (exit-time trace); mantle's climb phase deliberately *slows near the top* ("as you pull yourself up
  it makes sense to slow it down"); animations are driven *by* the physics state, never the reverse;
  player must be looking at the ledge; blocked while iron-sighting/knocked down. Collision was
  **fully disabled during the move** — they relied on validated endpoints and didn't care about the
  path between ("the goal is to have as fluid and nice-looking a movement between those two points");
  and competitive players preferred *manual* jump-press triggering over the auto SMART button because
  it let them start vaults earlier at sprint — direct validation of our jump-trigger + buffering design.
- *Dying Light (GDC 2018, full transcript in `docs/Parkour in 20 Simple Steps Transcript.md`):*
  pure runtime geometry scanning beats placed hints (50k hand-placed ledges was unmanageable);
  batch/limit traces for performance; keep moves **short**; never lock the camera — free look during
  the move is the single biggest first-person comfort win, and control should be handed back *before*
  the move visually ends; preserve momentum aggressively (begin-state duration scales with approach
  speed specifically to avoid speed changes); climb along the **player's forward vector**, not the
  ledge normal, with an angle clamp so the end position stays within capsule radius; **jump assist**
  (input buffering toward a better moment, coyote time, near-miss forgiveness) is what makes
  first-person jumping feel fair; hold-vs-tap on the jump button disambiguates stacked candidates
  better than any look-angle heuristic; full auto free-running prototyped great but killed skill and
  satisfaction — validates auto-parkour defaulting OFF; motion-sickness triad: never move the camera
  backward during a forward move, optional focus dot, avoid camera bumps in animations.

---

## 2. Architecture

New/changed modules inside `F4Parkour/src`:

```
Raycast.h/.cpp        — bhkPickData wrapper ported from ContextualLean (allow-list, actor skip,
                        re-cast, per-hit layer readout for debug). One function everything uses.
Detection.h/.cpp      — per-tick scan: VaultCheck + MantleCheck + classification → LedgeCandidate
Decision.h/.cpp       — vault-vs-mantle rules, intent (input) integration, tier blending
Mover.h/.cpp          — phase state machine + curve-driven position update, path validation,
                        char-controller sim toggle, exit velocity
Curves.h/.cpp         — curve evaluation (Catmull-Rom over control points), tier blending,
                        JSON preset load/save
AnimHijack.h/.cpp     — runtime keywords, synthetic melee dispatch, graph events, anim event sink
Input.h/.cpp          — JumpHandler vtable patch (contextual jump), air trigger, auto-parkour
Menu.cpp              — F4SEMF pages (General / Movement & Curves / Debug)
DebugDraw.h/.cpp      — world-space overlay (rays, points, capsules) + state popout + event log
Settings.h/.cpp       — extended settings, JSON preset plumbing (reuse FPGO preset infrastructure)
```

`ParkourManager` (existing) stays the orchestrator: `Update(dt)` runs detection at
`detectionInterval` (default 0.05 s) when idle, or the Mover when a move is in progress.

Data files:

```
Data/F4SE/Plugins/F4Parkour.ini                      — bootstrap (enable, debug, detection interval)
Data/F4SE/Plugins/F4Parkour/Presets/*.json           — shipped + user curve/timing presets
Data/F4SE/Plugins/F4Parkour/current.json             — active settings (menu writes here)
```

---

## 3. Phase 0 — the raycast (blocker; everything gates on this)

Port `CastLOSRay` from `ContextualLean.cpp` into `Raycast.cpp` as a general
`Raycast(start, end, RayOptions) → RayHit`:

- `bhkPickData` + `SetCollisionLayer(castQuery.m_filterData.m_collisionFilterInfo, layer)` — use
  **kLOS (41)** initially since it's the verified query layer; the pick returns per-hit
  `COL_LAYER` we filter ourselves.
- Keep the **allow-list** philosophy (Static, AnimStatic, Terrain, Ground, Clutter/Large, Props,
  Trees, Transparent*, Trap, DebrisLarge) — unknown layers must *fail* detection, not pass it.
  A missed vault is benign; vaulting onto an invisible collision box is not.
- Keep the actor-skip re-cast loop (player's own `skeleton.nif` registers on LOS picks) and the
  fraction sanity rejection (`frac <= 0.001 || > 1.0`).
- Extend `RayHit` with: hit point, normal (from `hknpCollisionResult`), `COL_LAYER`, layer name
  (debug), `NiAVObject*`, and best-effort `TESObjectREFR*` + FormType (for the "don't climb doors"
  rule — SkyParkourNG's `GetHitObjectFormType_Safe`).
- **Exit gate for this phase:** debug overlay draws every ray with hit distance + layer name,
  verified in an interior and an exterior cell. No detection work until rays are trustworthy.

Perf envelope: detection tick ≈ 15–45 rays at 20 Hz, player-only. Dying Light ran 200+/frame;
this is nothing, but still: early-out ordering (cheapest reject first) and skip detection entirely
while a move is in progress, in menus, in Power Armor, or when disabled.

---

## 4. Detection pipeline

Runs every `detectionInterval` while idle. Produces at most one `LedgeCandidate`:

```cpp
struct LedgeCandidate {
    MoveKind   kind;          // VaultEligible, MantleEligible, Both (decision phase picks)
    NiPoint3   ledgePoint;    // top-front point of the obstacle
    NiPoint3   ledgeNormal;   // surface normal at the top
    float      height;        // ledge z - player z (unscaled)
    float      topDepth;      // measured obstacle thickness along approach dir
    float      backClearance; // free distance past the far edge (vault landing room)
    NiPoint3   landingPoint;  // vault: far-side ground; mantle: on-top point
    float      landingDrop;   // vault: how far below ledge the far ground is
    HeadroomResult headroom;  // Stand / CrouchOnly / None (above ledge top)
    bool       fromAir;       // detected while airborne
};
```

### 4.1 Common preconditions
- Camera state: first person, third person, or ironsights-camera only (existing check).
- Not in Power Armor, not in menu, not knocked down/staggered, not already parkouring.
- **Not sighted/ADS** (gunState 6/8): hard block in v1. This respects the MagnaScope lesson —
  never yank the sighted state; we simply don't compete with it.
- Not swimming in v1 (Skyrim's water-grab is a later feature); still apply SkyParkourNG's
  water rule: reject any `ledgePoint.z` below cell water height − 10.
- Reject hits on `FormType::Door` and on any moving/animated ref we can't trust (keep it simple:
  Door + anything whose layer isn't in the allow-list).

### 4.2 Vault scan (port exists; add the new rules)
1. Forward head-height ray, `2 × vaultLength`: must be clear (nothing tall in the approach).
2. Downward sweep: step forward `5 u × 20` iterations, cast down from head height. Track:
   - first hit inside `[minVaultHeight, maxVaultHeight]` → the **obstacle top** (record highest);
   - subsequent hit back below `minVaultHeight` → the **far-side landing**. Record `landingDrop`
     and the far-edge index → `topDepth = (farEdgeIdx − firstIdx) × step`.
3. **Behind-object rule (user req):** landing must exist within the sweep AND
   `backClearance ≥ playerCapsuleRadius × 2 + margin` (≈ 40 u default, slider). Verified by a
   forward ray at *crouch* height from just past the far edge, plus a down ray at the landing point
   (must hit walkable ground within `maxVaultDrop`, default 140 u — clears the "vault into a pit"
   and "land inside the thing behind" cases). If this fails → vault ineligible; candidate may
   still be mantle-eligible.
4. **Railing/thin check (SkyParkourNG):** up ray from `ledgePoint + 5`: if blocked below
   `0.55 × playerHeight`, it's railing-like → vault-only geometry; side rays (±30 u) detect
   horizontally tiny structures → reject entirely if both blocked.
5. Landing-drop cap: `landingDrop ≤ maxElevationChange` (default 80 u) or vault ineligible
   (Brink clears momentum on big drops; we just don't vault them in v1).

### 4.3 Mantle scan (port exists; add the new rules)
1. Up ray above player (headroom to even begin rising).
2. Forward stepped rays at clearance height → wall hit.
3. Down ray from above the wall face → ledge point; require `normal.z ≥ 0.5` (flatness),
   height within `[minMantleHeight, maxMantleHeight]`.
4. **Obstruction-behind-ledge (SkyParkourNG):** from `ledgePoint + 5 z`, pulled back 15 u toward
   the player, cast forward 30 u and along the reversed hit normal — any hit means a wall rises
   right behind the lip → reject (this is the "don't mantle into the bookshelf" rule).
5. **Top depth (user req — thin tops):** stepped down-rays past the ledge point along the approach
   dir measure `topDepth`. Mantle requires `topDepth ≥ minMantleDepth` (default 25 u ≈ a third of
   the capsule). Thinner than that and the player would slide straight off → candidate becomes
   **vault-only** (the "extreme cases" the user called out), or nothing if vault also failed.
6. **Headroom, stand OR crouch (user req / UneducatedShooter):** up rays at the on-top point
   (center + left/right ±15 u):
   - clearance ≥ standing capsule height → `Headroom::Stand`;
   - else ≥ crouched capsule height → `Headroom::CrouchOnly` → move completes into forced sneak
     (send the sneak state the same way CrouchSlide does, so UneducatedShooter's own crouch
     collision-height adjustment sees a normal sneak and stays in charge of the capsule — we
     coexist, per the compatibility memory, never fight it);
   - else `None` → reject.
   Read the capsule dimensions from the live `bhkCharacterController` shape where possible instead
   of hardcoding 120/72, so mods that alter the capsule are automatically respected; fall back to
   configurable constants.
7. Classification into tiers (below), Brink-style look requirement: the candidate must be within
   a view cone (dot(viewDir, toLedge) threshold, slider, default ~35°).

### 4.4 Height tiers (user-configurable, defaults below)

Tiers are *reference points*, not gates — anything between `min` and `max` works; timing and curve
shape blend linearly between the two nearest tiers (Brink buckets + smooth interpolation).

| Tier | Default height (units) | ≈ meters | Default duration |
|---|---|---|---|
| Vault Low | 45 | 0.64 m | 0.50 s |
| Vault Mid | 70 | 1.00 m | 0.65 s |
| Vault High | 100 | 1.43 m | 0.85 s |
| Mantle Low | 60 | 0.86 m | 0.60 s |
| Mantle Mid | 105 | 1.50 m | 0.85 s |
| Mantle High (eye) | 150 | 2.14 m | 1.15 s |

Ranges: vault `[40, 110]`, mantle `[40, 155]` from the ground (≈ eye height cap, per the user);
airborne detection extends mantle max by current jump apex — practically `[40, ~230]` measured from
the jump start. FO4 scale notes: player height 120 u, eye ≈ 108 u, 70 u ≈ 1 m.

### 4.5 Airborne detection (user req)
While `IsInAir`, a jump press re-runs a tightened mantle scan from the current (falling) position:
shorter forward reach, ledge must be between hand height and `airGrabMaxReach`; on success execute
a mantle whose curve starts from current vertical velocity (blend, don't teleport). Vault from air
is allowed only when the full vault rule set passes from the airborne position (rare, but it makes
bunny-vaulting a railing mid-jump work). This is the Brink "grab" case merged into mantle.

---

## 5. Vault vs mantle decision + mid-move intent (user req)

When a candidate is `Both`-eligible at activation:

1. **Geometry veto first:** no back clearance → mantle; top too thin → vault.
2. **Intent:** sprinting or forward held → vault; otherwise → mantle
   (Brink's distance heuristic folded into input intent, which reads better in first person).

**Mid-move conversion (the "released forward" rule):** vault and mantle share their first phases
(Approach + Rise to the ledge apex). The commit point is the apex — the moment the capsule would
cross the far lip. Until then, each frame checks the move vector: if forward input has been released
(or the user started pulling back) **and** the candidate was mantle-eligible, the move retargets to
the mantle top-out. After the commit point the vault always completes (no teleporting back).
Animation on conversion: swap the runtime keyword and fire `F4Parkour_Convert` graph event; the OAR
clip may finish visually as a vault-ish motion — acceptable for v1, listed under polish.

---

## 6. Execution — the Mover

Phase state machine, all positions computed in code (no root motion dependency):

```
Vault : Approach → Rise → Cross → Land → Exit
Mantle: Approach → Rise → TopOut → Exit
```

- **Path:** cubic Bezier through computed keypoints — start → apex (`ledgePoint + apexClearance`,
  default 8 u above the top) → landing/on-top point. The *shape* comes from geometry; the *feel*
  comes from the curves (below). Duration = tier-blended base × momentum scale × user global speed.
- **Direction (Dying Light's "straight line out of hell"):** the path runs along the **player's
  approach vector**, not the ledge normal — snapping to the normal produces a visible sideways
  yank on oblique approaches. Clamp approach obliqueness (reject candidates approached at more
  than `maxApproachAngle`, default ~45°, slider) so the landing never misses the geometry by more
  than the capsule radius. `targetAngle = startAngle` (already the case) stays.
- **Momentum (user req):** capture horizontal entry speed at activation. Entry speed scales
  duration down (clamped, e.g. sprint ≈ 0.8×) and Vault Exit restores the captured velocity via
  `SetLinearVelocityImpl` — a sprint flows through the vault (Brink: vault keeps momentum).
  **Exit direction is player-influenced** (Brink playtest finding): blend the restored velocity
  from the approach vector toward the current movement-input/look yaw (slider, default ~50%), so
  steering during the vault carries into the landing. **Exit-time drop trace** (Brink): a final
  down ray at the landing point; if the actual drop exceeds `momentumDropCutoff` (default 100 u),
  zero the restored momentum instead of launching the player off a roof. Mantle Exit zeroes
  velocity and applies a small settle step (Brink: mantle clears). If sprint is still held on
  vault exit, re-assert the sprint state so the animation doesn't hitch (moveMode bit 0x100
  check, FPGO pattern).
- **Sim toggle:** prefer CommonLibF4's `CHARACTER_FLAGS::kNoSim` on `bhkCharacterController::flags`
  (SkyParkourNG uses the enum; the current raw `+0x300 | bit17` poke is listed as *needs in-game
  verification* — verify once with the debug page, then keep whichever is real). Restore on
  finish/cancel, always, including on game-load reset.
- **Never-inside guarantee (user req):** Brink shipped with collision fully disabled during the
  move and *only* endpoint validation — so our endpoint checks are the load-bearing part, and the
  in-flight watchdog is extra safety they didn't even need. Two layers —
  1. *Activation validation:* sample the Bezier at ~8 points; at each, cast up (capsule head) and
     forward (capsule radius) probes. Any solid hit → don't start the move at all.
  2. *In-flight watchdog:* per-frame forward probe at capsule center; a new obstruction (door
     opened, physics object rolled in) → before apex: cancel back to start (existing
     `CancelParkour`); after apex: settle onto the ledge top as a mantle finish. Never leave the
     player embedded.
- **Input during move:** free look stays enabled (Dying Light lesson — do NOT lock the camera).
  Replace the current blanket `ControlMap::SetIgnoreKeyboardMouse` with targeted suppression:
  swallow jump/sprint/attack via the same handler patches while the move runs, keep look axes
  live. Movement input is read (for intent) but not applied.
- **Early control hand-back (Dying Light):** during the final portion of TopOut/Land (last
  ~20–30 % of the move, slider), movement input starts blending back in while the position
  finishes settling — control returns before the move visually ends, which reads as
  responsiveness rather than animation lock.
- **dt source:** keep the wall-clock dt workaround in the RunActorUpdates hook but clamp harder
  (already done) and freeze the mover while in menus (menu check already runs first).
- Third person: identical movement code; `Update3DPosition(true)` already warps the graph. The
  camera follows on its own. First person needs nothing extra for movement; optional camera-feel
  channels below.

---

## 7. Curves & JSON presets (user-facing feel system)

Keep the *user model* simple: per move type, per tier — a **duration** plus a small set of named
curves, each editable as 2–8 control points (Catmull-Rom, evaluated at runtime, cached per frame):

```jsonc
// Data/F4SE/Plugins/F4Parkour/Presets/Smooth.json
{
  "name": "Smooth",
  "description": "Long, floaty arcs. Forgiving timing.",
  "global": { "speedMult": 1.0, "sprintDurationScale": 0.85, "apexClearance": 8.0 },
  "heights": {                    // tier reference heights are part of the preset
    "vault":  [45, 70, 100],
    "mantle": [60, 105, 150]
  },
  "durations": {
    "vault":  [0.50, 0.65, 0.85],
    "mantle": [0.60, 0.85, 1.15]
  },
  "curves": {
    // x = normalized time, y = normalized progress. Same schema everywhere.
    "vault.progress":   [[0,0],[0.35,0.45],[0.7,0.85],[1,1]],   // easing along the path
    "vault.vertical":   [[0,0],[0.45,1.0],[0.75,1.0],[1,0.2]],  // lift profile vs apex
    "mantle.progress":  [[0,0],[0.4,0.35],[0.8,0.9],[1,1]],
    "mantle.vertical":  [[0,0],[0.6,0.95],[1,1]],
    "camera.dip":       [[0,0],[0.15,0.3],[0.5,-0.1],[1,0]]     // optional FP pitch offset (deg)
  }
}
```

- Ship four presets: **Smooth** (default), **Snappy** (CoD-like, short durations, hard ease-out),
  **Deliberate** (Hell Let Loose/realistic, slower highs, pronounced camera dip), **Arcade**
  (Mirror's Edge-ish, fast, strong momentum scale). All mantle progress curves follow Brink's
  climb-phase rule — decelerate near the top of the pull-up; it reads as weight rather than lag.
- Between-tier heights blend duration and curve *outputs* (evaluate both neighbors, lerp by
  height fraction) — cheap and continuous.
- Menu edits write `current.json`; presets are read-only templates copied on "Save As" (exact
  FPGO preset UX: dropdown, save/save-as/delete, unsaved-changes marker).
- Curve editor widget: draggable points on an ImGui drawlist graph with a live preview sparkline;
  fallback simple mode = two sliders (ease-in %, ease-out %) that generate the points. Every value
  has a tooltip in plain language (FPGO `SliderFloatWithTooltip` helpers).
- **Motion-sickness guardrail (Dying Light):** the `*.progress` curves are validated
  monotonic non-decreasing — the camera must never move backward during a forward move (Techland
  traced actual player nausea to exactly this). The editor clamps offending points and shows why.
  Optional **focus dot**: a faint center-screen dot rendered only while a move is active
  (foreground drawlist, one call) — Techland's cheapest and most effective comfort aid.
  Toggle, default ON at low alpha.

---

## 8. Input integration

- **JumpHandler vtable patch** (allocate-copy-swap slot 8, the proven SprintHandler/SneakHandler
  approach — runtime-agnostic, no REL::IDs): on `JustPressed`:
  1. Move in progress → swallow.
  2. Grounded + valid candidate + contextual test passes (forward input > 0.1 on
     `PlayerControls::data.moveInputVec.y`, look-cone satisfied) → `TryActivateParkour()`, swallow.
  3. Airborne + air toggle on → run airborne scan; success → activate, swallow.
  4. Otherwise → pass through (normal jump).
- **Jump assist (Dying Light's three steps, adapted):**
  1. *Input buffering:* a jump press with no valid candidate is buffered for `jumpBufferWindow`
     (default 0.2 s, slider); if detection produces a candidate inside the window (sprinting at a
     low wall and pressing a hair early), the move fires instead of eating the press. Extends
     effective engage range without any aim requirement.
  2. *Coyote time:* a press within `coyoteWindow` (default 0.15 s) after walking off an edge still
     counts as grounded for detection.
  3. *Near-miss forgiveness:* a jump that clears an obstacle without touching it can still
     re-trigger an air-mantle within a short range of the ledge (~15 u) — folds into the airborne
     scan, no separate code path.
- **Hold-to-mantle disambiguation (Dying Light, adapted):** optional input mode (default OFF):
  when two candidates stack vertically (window opening + roof lip), *tap* takes the
  lower/contextual pick, *hold* commits to the higher mantle. Note the caveat: in Dying Light
  "hold" meant *grab the ledge* (their hang system) — we have no hang, so here it only biases
  candidate selection upward. Since our pipeline returns a single candidate anyway, this is a
  low-priority option; implement last, cut if it doesn't earn its keep in testing.
- **Auto-parkour toggle (default OFF):** while sprinting, a vault-eligible candidate within
  `autoEngageDistance` (default ≈ 1.5× capsule width, slider) triggers automatically — Brink's
  SMART button feel, opt-in. Dying Light's full free-run prototype "worked amazingly" but erased
  player skill — this stays opt-in and vault-tier-only. A separate **auto step-up** toggle
  (default OFF) covers only knee-height obstacles (< Vault Low) which are invisible below the FOV
  when close — Dying Light auto-climbs these unconditionally because it feels natural.
- Keep the existing F4SEMF input callback only as an *optional dedicated key* fallback (hidden
  setting, default unbound); primary path is the jump hook.
- Existing `requireMoving` setting folds into the contextual rule.

---

## 9. Animation integration (melee hijack + OAR)

Master toggle `bPlayMeleeAnim` (default **ON** — it *is* the animation path; OFF = pure camera/
position movement, e.g. for users without the OAR anim pack).

Sequence at activation (ordering matters — OAR evaluates conditions at **clip activation**, so
keywords must be pre-armed, verified in the ADSReload work):

1. Create-on-init runtime keywords (SuperSprint pattern), editor IDs:
   `AnimsParkourKeyword` (any parkour), `AnimsParkourVaultKeyword`, `AnimsParkourMantleKeyword`.
   Also set graph variables `F4Parkour_Type` (int tier) and `F4Parkour_Height` (float) as free
   extra conditioning data (OAR F4 has `HasGraphVariable`).
2. Add keyword(s) to the player.
3. Dispatch a synthetic melee/bash button event through the input receiver (`SyntheticInput`
   pattern; alternatively F4SEMF's now-working `AddInputEvent` — pick one path, the memory says
   the framework API dispatches as of 3.3.x). The engine plays the melee/bash clip in the active
   perspective; OAR conditions (`HasKeyword AnimsParkourVaultKeyword` + perspective) replace it
   with the vault/mantle animation. Both perspectives get replacements — ship example OAR configs
   under `OAR_TestConfigs/` style folders as authoring templates.
4. Fire `F4Parkour_Vault` / `F4Parkour_Mantle` graph events (already in place) for anyone
   listening; the AnimEventSink's `F4Parkour_End` / `F4Parkour_Cancel` remain supported as an
   *optional* early-finish signal, but the Mover's timer is authoritative (animations follow
   physics, never drive it — Brink rule, and it keeps the no-anim mode identical).
5. On finish/cancel/convert: remove keywords, clear graph variables.

**Melee side effects to handle:** the hijacked swing is a real attack — suppress its damage window
while a parkour is active (skip/neutralize the player's melee hit during the move via the attack
data or a HitEvent guard) and re-check `meleeAttackState` interactions. If suppression proves
messy in practice, fallback: whitelist the anim only (accept rare whiffs) and document it.
Weapon-drawn state is left alone (gun stays out; the replacement anims are authored one-handed,
exactly like SkyParkour's weapon-out grabs).

---

## 10. Menu design (F4SE Menu Framework)

Register like FPGO (`F4SEMenuFramework::AddSectionItem` per page + `AddWindow` popouts).

**Page 1 — General**
- Master enable; play melee animation toggle; auto-parkour while sprinting (default off);
  auto step-up for knee-height obstacles (default off); allow while airborne; allow in third
  person; require forward input; look-cone angle; jump buffer / coyote windows; hold-to-mantle
  mode; focus dot; detection interval; dedicated-key fallback (capture UI like FPGO hotkey
  capture).

**Page 2 — Movement & Curves**
- Preset dropdown + save/save-as/delete + unsaved marker (FPGO UX).
- Tier height sliders (vault ×3, mantle ×3) with unit + meter readout.
- Tier duration sliders; sprint duration scale; global speed; momentum keep %; apex clearance.
- Curve editors (progress/vertical per move type, camera dip) with simple/advanced mode toggle.
- "Test vault/mantle now" buttons (executes against current aim target — huge iteration saver).

**Page 3 — Debugging** (dedicated, per user)
- Master debug toggle + non-pausing popout window (framework-managed, FPGO pattern).
- **World overlays** (ImGui background drawlist + `NiCamera::WorldPtToScreenPt3` projection —
  TrueHUD's DrawArrow role, drawn ourselves):
  - every detection ray, green/red by hit, labeled with distance + collision layer name
    (the 47-entry layer name table from ContextualLean);
  - ledge point marker, landing point marker, top-depth span, back-clearance span;
  - planned Bezier path with sample-probe results; capsule ghost at landing;
  - candidate state text (kind, height, tier blend weights, headroom Stand/Crouch/None).
- **State popout:** live Mover phase, t, dt, entry speed, momentum, char-controller state,
  kNoSim flag readout (for the offset verification), gunState, moveMode bits, input snapshot.
- **Event log:** ring buffer of recent decisions with reject reasons ("mantle rejected:
  topDepth 12 < 25", "vault rejected: back clearance blocked by Static 'Bookshelf01'") — newest
  first with age stamps (FPGO recent-events pattern). This is the tool that makes tuning real.
- Toggles: freeze detection, slow-motion mover (0.1×), draw only failed rays, log to file,
  disable never-inside watchdog (for comparing), force-vault / force-mantle override.

---

## 11. Compatibility notes

- **UneducatedShooter:** never fight its input hooks or crouch capsule logic — we enter sneak via
  normal state, it adjusts collision; our headroom math reads the live capsule. Coexist, never
  require uninstall (standing memory).
- **FPGunplayOverhaul:** CrouchSlide + vault interplay — block parkour activation while sliding
  (matches Brink's active-state exclusion list: no vault while vaulting/sliding/sighted/knocked
  down). Inertia's camera springs read our position warp as motion; if it double-dips, expose the
  parkour window via the anim events it already consumes.
- **MagnaScope / sighted state:** hard-blocked while ADS (never drop the sighted state).
- **Multi-runtime (OG/NG):** everything chosen here is runtime-agnostic (CommonLib members,
  vtable patches, bhkPickData). The one REL::ID — `RunActorUpdates(556439)` — must be confirmed
  for both runtimes or replaced with a vtable/member-fn hook like FPGO's update path.
- **Doors, movables:** excluded by FormType/layer (SkyParkourNG's hard-won rules).

---

## 12. Risks & unknowns (verify early, each has a debug-page readout)

1. `kNoSim` flag offset/enum on FO4's `bhkCharacterController` — verify via debug readout before
   trusting the mover. Fallback: hold velocity at zero + reposition every frame (works without
   the flag, slightly fights the sim).
2. Melee hijack side effects (damage window, `meleeAttackState` gating our own re-activation,
   third-person melee move-forward root motion adding to our warp). Mitigations in §9.
3. `SetIgnoreKeyboardMouse` is too blunt — replace with targeted handler suppression; test pause
   menu access mid-move.
4. Havok fixed-update vs our wall-clock dt: watch for visible stutter on high-refresh setups;
   if present, move the position write to a camera-update hook (FPCameraOverhaul territory).
5. OAR clip-activation timing vs synthetic input latency (keyword pre-arm handles the known case;
   verify with the Clips API debug readout — `resolvedPath` tells us exactly what played).

---

## 13. Milestones (each ends with an in-game test gate)

| # | Milestone | Test gate |
|---|---|---|
| 0 | Raycast port + debug ray overlay + layer names | Rays visibly correct in interior + exterior |
| 1 | Detection rewrite (all §4 rules) + event log + reject reasons | Walk a settlement: candidates appear/reject sensibly, zero false positives on doors/invisible walls |
| 2 | Mover with curves (hardcoded Smooth), kNoSim verified, never-inside probes | Vault + mantle a test course; cancel/convert paths; no embedding ever |
| 3 | Input: JumpHandler patch, contextual rule, jump assist (buffer + coyote), air trigger, targeted suppression | Jump feels unchanged when no ledge; parkour when intended; sprint-press-early still vaults; air-mantle after jump |
| 4 | Momentum: sprint-through vaults, entry-speed duration scaling, mantle settle | Sprint a course without losing flow |
| 5 | Animation: keywords + melee hijack + example OAR configs + toggle-off mode | Anim plays in FP and TP; toggle-off still feels fine |
| 6 | Menu: all three pages, JSON presets, four shipped presets, curve editor | Full tune-test loop without leaving the game |
| 7 | Polish: conversion visuals, crouch-only mantles, compat passes, perf check | Play session in Magnum Opus list with FPGO + US active |
