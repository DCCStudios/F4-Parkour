# F4Parkour — Implementation Plan: Camera Collision · Hand IK · Air Parkour

Status: proposed. Grounded in the current code (Mover.cpp, Detection.cpp,
ParkourManager.cpp, AnimHijack.cpp, CameraPivot.cpp). Each feature is
independent and shippable on its own; suggested order is Air → Camera →
IK (rising risk, rising reward).

The three share one hard rule already baked into this codebase:
**movement never waits on, and never breaks because of, animation or
camera.** Every feature below degrades to today's behavior if its own
piece fails.

---

## Reference method: STALKER "Ledge Grabbing" (demonized) — read this first

A shipped first-person climbing mod for STALKER Anomaly
(`PluginTemplate/Ledge Grabbing/gamedata/scripts/`) solves all three of
our goals with ONE mechanism, and it changes the IK recommendation below.
Its method, distilled from the source:

- **Detection**: an array of forward rays from max→min climb height finds
  the ledge; a player-width side check and a stand-height check validate it
  (`demonized_ledge_grabbing.script` ~L506-580). This is what we already do.
- **Motion = procedural path blended with an authored camera curve.** Each
  frame it computes TWO camera transforms and blends them:
  1. a **procedural cubic Bezier** from the start camera position to the
     measured climb point — this is what adapts to ANY height/position;
  2. a **prebaked camera curve** (location XYZ + rotation euler XYZ, baked
     from a Blender animation as Bezier segments — see
     `demonized_ledge_grabbing_animation_data.script` and the exporter
     `..._blender_script.py`), rotated to the player's facing and added as
     an offset.
  It eases the prebaked curve IN over `animInFrame`, holds it through the
  "signature" middle, then eases back OUT to the pure procedural path after
  `animOutFrame` (L1295-1322). Procedural start → authored signature →
  procedural settle. **This is phase segmentation via a blend weight, and
  it is where variable height is absorbed — by the procedural half, not by
  scaling the animation.**
- **The body follows the camera**: `set_actor_position(cameraPos.y - camY)`
  each frame, where `camY` is the fixed camera-height-over-feet offset
  (L1304-1313). Feet track the view.
- **Camera looks at the geometry**: it lerps the view toward the grab point
  (`animInK`), then toward the climb point (`animRotationK`), plus the
  authored rotation curve as an additive flavor, easing back to neutral on
  exit (L1327-1381). This IS the "cutscene" framing and it keeps the view
  aimed at what you are climbing.
- **Hands are a VIEWMODEL animation, NOT world-space IK.**
  `play_hud_motion(2, "item_anm_ledge_grabbing", "anm_climb", ...)`
  (L1398). The hands read as grabbing the ledge purely because the CAMERA
  is positioned relative to the ledge so the first-person hands line up.
  There is no per-frame hand IK to world geometry anywhere in the mod.

### What this changes for us
1. **First person needs NO world-space hand IK.** We already play a
   first-person hand animation on weapon-away moves (the `Vault.hkx` /
   `Mantle.hkx` auto-idles just added) — that is the exact analogue of
   `anm_climb`. The "attached" feel comes from **camera-to-geometry
   alignment + a canned FP hand animation**, which sidesteps the entire
   FP-arms-projection problem (§3). This is dramatically simpler and is a
   proven shipping approach.
2. **Camera collision + cutscene framing + the mantle "feel" are one
   feature**: a camera director that drives the FP camera along a
   procedural path to the climb point, looks at the grab/climb point, and
   optionally blends an authored curve for signature. Our `CameraPivot`
   node is the FO4 seam for it.
3. **World-space hand IK becomes a THIRD-PERSON-only concern** — the view
   where you actually see the body and a viewmodel trick cannot help.
4. We can **reuse their Blender camera-curve export concept** to author a
   signature FP climb curve and bake it to data we ship (same idea as our
   existing Catmull-Rom curve JSON), then blend it with the procedural path.

FO4 caveat: STALKER's `set_cam_custom_position_direction` and
`play_hud_motion` are X-Ray engine calls with no direct FO4 equivalent.
The FO4 mapping is: drive the FP camera via the `CameraPivot` inserted
node (position + rotation) instead of `set_cam_custom_position_direction`,
and the FP hand animation via our existing `SetupSpecialIdle` auto-idle
instead of `play_hud_motion`. The *method* ports; the API calls do not.

---

## 1. Air vault / mantle (extend the partial support that already exists)

### What already works
- `Detection::Scan(player, a_fromAir=true)` runs and extends the mantle
  reach by `airGrabExtraReach`; `LedgeCandidate.fromAir` is set.
- `ParkourManager::Update` already scans in air: `airborne = !grounded &&
  IsInAir` and `if (airborne && allowInAir) Scan(..., true)`.
- The Mover already classifies `startedInAir` physically and resolves an
  air-started move with a natural micro-landing (no camera-pitch bug).
- A buffered jump press is consumed for `candidate.fromAir` candidates
  (Dying Light jump-assist), and `jumpWouldParkour` (the synchronous hook
  trigger) is set from any valid candidate + `ContextualIntent` + dry-run,
  air included.

### Gaps to close
1. **Air VAULT is disabled.** `Detection::Scan` skips `VaultScan` entirely
   when `a_fromAir` (`Detection.cpp:796`). Only mantle can fire in air.
2. **Air trigger is second-class / unreliable.** Grounded jumps convert
   synchronously in the input hook; air relies on the buffered press +
   `ConsumeBufferedPress`. A single clean mid-air press near a ledge does
   not reliably grab because the candidate may not exist on the exact tick
   the press lands.
3. **The `grounded` gate over-claims.** `grounded` now includes
   `GroundWithin(25)` and the coyote window, so for the first fraction of a
   jump the player reads grounded and the air scan never starts — you have
   to be well clear of the ground before air detection engages.

### Plan
- **Enable air vault** behind a setting `allowAirVault` (default true when
  `allowInAir`): in `Detection::Scan`, run `VaultScan` when `a_fromAir` too.
  Air vault is a mid-flight clear of a waist obstacle; keep the same
  reach extension as air mantle.
- **First-class air trigger.** Keep the synchronous hook path as the
  primary: because `jumpWouldParkour` is already set for air candidates,
  the fix is to guarantee an air candidate *persists* long enough to be
  pressed. Extend the existing candidate-persistence grace specifically
  while airborne (raise the air grace to ~0.5s) so a mid-air press finds a
  candidate. The buffered-press path stays as the assist for the
  press-slightly-early case.
- **Air-grab auto-assist (optional, default off).** When falling
  (`fallTime > airAutoGrabDelay`, ~0.3s) with a valid air candidate in the
  look cone, auto-activate — the Dying Light "reach and it grabs" feel.
  Gate behind `autoAirGrab` so manual players are unaffected. SkyParkour
  does exactly this for its Grab type (`fallTime > 0.5`).
- **Tune the gate.** Add a short "just jumped" carve-out: once
  `Input::TimeSinceEngineJump()` shows a real jump launched and the player
  is rising/falling, allow the air scan even if `GroundWithin(25)` is still
  briefly true — an ascending jump clears 25u fast, and this is the window
  where you want the grab to arm early.
- **Momentum on air exit.** An air vault should preserve downward/forward
  momentum on exit (you were already moving); an air mantle clears it (you
  pulled onto the top). The Mover's existing vault/mantle exit-velocity
  split already does this — verify it reads correctly for air starts.

### Files
`Detection.cpp` (VaultScan in air, reach), `ParkourManager.cpp`
(air candidate grace, auto-grab, gate carve-out), `Settings.*`
(`allowAirVault`, `autoAirGrab`, `airAutoGrabDelay`), `Mover.cpp`
(verify air-exit momentum).

### Risk
Low — this is hardening an existing path, not new machinery. Main risk is
false air-grabs while falling past a ledge you didn't mean to catch;
mitigated by the look-cone requirement and `autoAirGrab` defaulting off.

### Test gate
Jump toward a chest-high ledge and press jump at apex (air mantle); sprint
off a rise over a low obstacle and press jump mid-flight (air vault); fall
past a ledge without input and confirm no unwanted grab.

---

## 2. Camera collision (no clipping through geometry)

### Engine reality
- **Third person** already collides: `PlayerCamera`'s `ThirdPersonState`
  sphere-sweeps from the player to the desired camera position. We do not
  take TP camera control during a move, so TP mostly self-corrects — but
  fast move motion can momentarily out-run the sweep.
- **First person** does NOT collide: the view is the FP `"Camera"` bone
  (`Get3D(true)`), and normally the near-plane hides wall interpenetration.
  During a mantle the head rises through the ledge lip / into the wall,
  and the near plane is not enough — you see inside geometry. **This is
  the case to solve.**

### Plan — reuse the CameraPivot insertion, add a translation channel
`CameraPivot` already inserts `F4Parkour_CameraInserted1st` above the FP
`"Camera"` bone and writes its `local.rotate` (+ a rotate-in-place
translate). Extend it into a small **camera director** that owns the
inserted node's *full* local transform each frame, composing two channels:
- **rotate**: the existing high-tier leveling / look-at-ledge pitch.
- **translate**: a NEW collision push.

Per move frame, first person:
1. Anchor at a stable point that is provably outside geometry — the neck /
   upper chest, e.g. player capsule position + (eyeHeight − ~20u), or the
   `"Camera"` bone's parent bone world position.
2. Read the FP `"Camera"` bone's current world position (the view point).
3. Sphere-cast (or ray with a skin radius ~8u) anchor → camera. If it hits
   at distance `d < |camera − anchor|`, the view is about to enter solid:
   pull the camera in to the hit point minus the skin, by writing the
   inserted node's `local.translate` = the pull-back offset expressed in
   the node's parent space (same basis math the pitch pivot already uses:
   rows-are-axes, `M^T` for local→parent).
4. Smooth the pull (convergent lerp, not a snap) so the view eases rather
   than yanks; ease back out when clear.

Because both channels live on one node, merge the rotate and translate
writes in one function so they never stomp each other (today `SetPitchDeg`
overwrites `local.translate` — that must become additive with the
collision offset).

### Optional generalization
Make the collision pass run **always in first person**, not only during a
move (a general FP camera-collision feature), behind `cameraCollision`
(default on) with a separate `cameraCollisionInMoveOnly` if we want to
scope it tight first. Keep TP to the engine unless testing shows the fast
move motion clips there too, in which case add a TP desired-position clamp.

### Files
`CameraPivot.cpp/.h` (rename conceptually to a camera director; add the
translate/collision channel + the combined write), `Mover.cpp` (call it
per move frame with the current phase), `Settings.*`
(`cameraCollision`, skin radius, smoothing).

### Risk
Medium. Pulling the FP view back can feel like a zoom if over-applied;
keep the skin small and the motion eased. Anchor choice matters — if the
anchor is ever inside geometry the cast is meaningless, so pick the
chest/neck (the capsule guarantees it is clear). Never write `data.angle`
(the round-1 lesson); this is a node offset only.

### Test gate
Mantle a ledge with a wall close behind in first person — the view should
ride the surface instead of showing the wall interior; strafe the camera
into a corner mid-move and confirm it eases out, not pops.

---

## 3. Hand IK to the climbed surface

**Scope narrowed by the STALKER reference (above): world-space hand IK is
now a THIRD-PERSON-ONLY feature.** In first person, do NOT IK the hands —
use the STALKER approach (camera driven to the geometry + the existing
`Vault.hkx`/`Mantle.hkx` viewmodel hand animation), which avoids the
FP-arms-projection problem entirely and is a proven shipping method. Build
the camera director (§2, extended toward the STALKER path) first; it
delivers the FP "attached" feel with zero IK.

The rest of this section is the THIRD-PERSON solver — where you see the
actual body and no viewmodel trick applies. It is the highest-effort piece
and is optional / last (the Far Cry / Dying Light technique: body motion +
IK-pinned contacts).

### Step 0 — skeleton probe (prerequisite; do this first, in-game)
Add a debug-gated dump (behind `debugEnabled`) that, on a mantle, walks
both skeletons and logs each arm bone's presence, parent chain, and rest
transform:
- Third person: `Get3D(false)` → the world-space rig (targets map directly).
- First person: `Get3D(true)` → the view rig (`CameraPivot` already
  navigates it via `GetObjectByName`, so access is proven).
Expected chain per arm: upper-arm → forearm → hand (FO4 names like
`LArm_UpperArm` / `LArm_ForeArm1` / `LArm_Hand`, to be confirmed). **The
probe decides whether FP IK is viable at all** — FP arm rigs are notoriously
sparse; if the chain is degenerate, FP degrades to no-IK and IK becomes a
third-person / FP-body feature.

### The solver — analytic two-bone IK
Closed-form (law of cosines), no iteration, effectively free per frame.
Per arm, given shoulder (root), elbow (mid), hand (end), a world-space
target, and a pole vector (elbow-out direction):
1. Clamp target to reach (upper + forearm length) so the limb never
   over-stretches — Dying Light's "~40 cm of freedom" clamp; beyond reach,
   blend the target toward the natural hand position.
2. Solve elbow angle from the two bone lengths and the root→target
   distance; orient the upper bone at the target; roll to the pole vector;
   rotate the hand to the surface normal.
3. Write the corrected bone `local` transforms.

### Targets — from Detection, already measured
- `mantleLedge` is the lip point; the lip tangent = `approachDir` rotated
  90° in the horizontal plane.
- Left hand target = `mantleLedge − tangent * shoulderHalf`, right =
  `mantleLedge + tangent * shoulderHalf` (shoulder width ~ 20u). Raycast
  straight down a few units at each to snap onto the real lip surface and
  read its normal for hand orientation.
- (Later) feet target the wall face below the lip during the pull phase.

### Where it runs — after the graph solves
The frame hook `PlayerUpdateAnimationHook::Thunk` runs `Original()` first
(the behavior graph poses the skeleton), then our code — so writing bone
transforms here lands *after* the animation each frame and our writes win.
This is the same ordering `CameraPivot` relies on. Put the IK pass in the
Mover's per-frame update (grounded moves) / a new `HandIK` module.

### Weight blending — phase-gated
Ramp IK weight 0→1 over the reach phase, hold through pull, ramp 1→0 over
stand, so hands ease onto and off the ledge instead of popping. Drive the
weight from move progress / phase (pairs naturally with the phase
segmentation in the broader mantle rework, but works off raw progress
without it).

### First-person reconciliation — the projection problem
FP arms render through a *different FOV* than the world (FO4 has
`fDefault1stPersonFOV` vs the world FOV; FOVSliderF4SE in the load order
proves these are live). A hand placed at the true world ledge won't line
up on screen under the mismatched projection. Two options:
- **Match the viewmodel FOV to the world FOV for the move's duration**
  (lerp in over reach), so the *same* world-space solver works on the FP
  rig. Cleanest; the weapon is away for a two-handed climb so the FOV
  shift is nearly invisible. Preferred.
- **Screen-space IK** (the literal Dying Light method): project the ledge
  through the world camera → screen, inverse-project through the viewmodel
  camera to place the hand. No FOV change, but a visual cheat.
Build the solver **world-space first on the TP rig**; treat FP as "same
solver, FOV-matched." Building FP-first bakes in the cheat.

### Files
New `HandIK.cpp/.h` (probe + solver + targets), `Mover.cpp` (call per
frame with phase/weight), `Detection`/`ParkourTypes` (expose lip tangent /
per-hand targets if not already derivable), `Settings.*` (`handIK`,
per-view enable, reach clamp), FOV reconciliation via the 1st-person FOV
INI var.

### Risk
Highest. Ordering (write after graph — mitigated, the hook runs Original
first), skeleton mapping (one-time, gated by the Step-0 probe), FP vs TP
being distinct rigs, and rubbery motion if IK corrects large errors (keep
the underlying body motion close and clamp reach). The nice property: the
view where IK is hardest (FP) is also where its absence hurts least (you
see forearms in a small screen region for ~1s), so a clean fallback exists.

### Test gate
Third person first: mantle ledges of several heights and confirm both
hands weld to the lip edge as the body rises, with no over-stretch. Then
first person with FOV-match: confirm the hands still line up on screen.

---

## Suggested sequencing (revised after the STALKER reference)

1. **Air parkour** — low risk, mostly enabling/hardening existing code;
   immediate gameplay payoff.
2. **Camera director (FP), STALKER-style** — the big one, and it now
   covers THREE things at once: camera collision (the path ends at a valid
   climb point and rides toward it), the "cutscene"/attached feel (view
   looks at the grab/climb point), and — combined with the existing FP hand
   auto-idles — the first-person "grabbing" illusion **with no hand IK**.
   Reuses the `CameraPivot` node. Optionally add a Blender-baked signature
   curve later, blended with the procedural path.
3. **Third-person hand IK** — optional, last, gated on the Step-0 skeleton
   probe. Only needed for the TP view; FP is already handled by step 2.

The STALKER reference collapses what looked like three features into
"harden air" + "one camera director" (+ optional TP IK). All keep movement
authoritative and degrade cleanly, so they ship incrementally without
destabilizing the current mantle/vault.
