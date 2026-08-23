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

This is the highest-reward and highest-effort feature, and it is what
makes a mantle read as "attached" at any height from one animation (the
Far Cry / Dying Light technique: warped body + IK-pinned contacts).

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

## Suggested sequencing

1. **Air parkour** — low risk, mostly enabling/hardening existing code;
   immediate gameplay payoff.
2. **Camera collision (FP)** — medium; reuses the CameraPivot node, self-
   contained, improves every move.
3. **Hand IK** — gated on the Step-0 skeleton probe. Build TP world-space
   solver → add FP with FOV-match. This is the "attached" feel and pairs
   with the larger root-motion/phase-segmentation direction when that lands.

All three keep the movement authoritative and degrade cleanly, so they can
ship incrementally without destabilizing the shipping mantle/vault.
