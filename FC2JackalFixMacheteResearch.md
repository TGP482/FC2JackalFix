# Far Cry 2 Jackal Fix - machete blade and the first person layer

Session record. Target is `Dunia.dll`, 32-bit, image base `0x10000000`, Steam build. All `FUN_` names
are Ghidra defaults. Continues `FC2JackalFixNearPassResearch.md`, which left the machete blade open
and recorded the promotion rule as removed.

The session started from "the machete blade draws at the world field of view while swinging" and
ended with that fixed, two regressions it caused fixed, one smooth transition attempt reverted, and
the seat swim still open.

---

## 1. Status

| Item | State |
|---|---|
| Machete blade at world FOV mid swing | **Fixed.** Layer withheld across the swing, section 3 |
| Arms and mounted weapon over the vehicle after the fix | **Fixed.** Release pushes through `FUN_1051a6e0`, section 3 |
| Turret, boat, glider, car hides treated as swings | **Fixed.** Melee gate plus release triggers |
| Depth matrix rebuilt on frames nothing changed | **Fixed.** `bNearPassRewritten` gates the sync |
| Smooth near pass transition on mount, ladder, map | **Reverted.** Eased on exit only, section 6 |
| Viewmodel swim in a car and a glider | **Open.** Not this module, section 6 |

No diagnostics remain in the tree.

---

## 2. Engine facts established

### Node to owner

`FUN_10336b10` is the mask push, and every render node any component owns passes through it,
mask zero included:

```
10336b10  83 C1 1C        ADD  ECX,0x1c          ; ECX is the object entry's sub object
10336b13  6A 01 51        PUSH 1 / PUSH ECX
10336b16  B9 A0 68 F9 10  MOV  ECX,0x10f968a0    ; the node pool
10336b1b  E8 ? ? ? ?      CALL 0x10456d70        ; resolve, writable
10336b20  8B 4C 24 04     MOV  ECX,[ESP+0x4]     ; the mask
10336b24  89 88 90 ...    MOV  [EAX+0x90],ECX
```

At `0x10336B20` EAX is the node, `[ESP+4]` the mask and `[ESP]` the return address. Five call sites
reach it: `0x1051A741` (`CGraphicComponent`, through `FUN_1051a6e0`), `0x1035CCEC`, `0x103CE29B`,
`0x105B4662`, `0x105B5CE3`.

The component is only in hand one frame up:

```
1051a738  8B 96 2C 01..   MOV  EDX,[ESI+0x12c]   ; ESI is the CGraphicComponent
1051a73e  52              PUSH EDX
1051a73f  8B C8           MOV  ECX,EAX
1051a741  E8 ? ? ? ?      CALL 0x10336b10
```

Parking ESI across that one call and keying on the resolved node names any node's owner. The
previous session used `FUN_1051cc90` for this, which is a change path the blade's component never
reaches, and concluded the owner was unrecoverable.

Measured with that map: the blade is `component=6C25F930`, entity `B5310F2300054100`, one object
entry, node radius 0.318 at 0.45 to 0.66 units from the eye, submitted with `node+0x90` clear.

### The layer is taken away, not missing

```
[mask] off component=6C25F930 00000001 -> 00000000  stack 10156D03 10154DED ...
[mask] on  component=6C25F930 00000000 -> 00000001  stack 10156D03 10154459 ...
```

One off/on pair per swing. Both halves come through the vtable `+0x98` call at `0x10156D03` inside
`FUN_10156c20`, reached from one weapon state's vtable at `0x10E21E08`: `0x10154DD0` hides and
passes 0, `FUN_10154440` shows and passes 1. The call site and the frames above it are identical for
a swap, so neither separates the cases.

`0x1051B230` is `__thiscall(mask, bEnable)`. At the entry `[ESP]` is the return address, `[ESP+4]`
the mask, `[ESP+8]` the flag. Its tail calls `FUN_1051a6e0`, which is what pushes `+0x12C` out to
every node the component owns.

### Timing separates a swing from everything else

With a tick on every toggle:

```
57040937 hide 78E84C40 / 57042265 show 78E84C40   swing, 1328 ms
57042546 hide 78E84C40 / 57043875 show 78E84C40   swing, 1329 ms
57045093 hide 78DE2670 / 57049656 show 78DE2670   weapon swap, 4563 ms
57062218 hide 78DE2670 / 57062218 show 85DABA90   mount, other components shown at once
                         57074203 show 78DE2670   dismount, 11985 ms
```

A swing returns the same component in about 1.3 seconds with nothing else shown between. Everything
else shows something else immediately or leaves the weapon away for seconds.

### selWeaponClass

Property record registered at `0x100F5149`, field offset `0x40` written at `0x100F5169`. Class 0 is
every `HandToHand` archetype. `fIronsightFOV` registers at `0x100F5FD3` with offset `0xE4` in the
same function, so both live in the property object the weapon setup holds at `ESI+0x04`.

### Weapon data

`42_weapons.xml` has five `HandToHand.*` prototypes, all sharing `machette_states123.xbg` except
`Machete_HomeMade` and `Machete_Primitive`. `20_OA_FakeWeapons.xml` carries `HandToHand.Fake_Machete`
with the same mesh and a `CFakeWeapon` component. `RenderInNearZViewPortID` appears in no XML across
all 42 tmpla libraries and the patch overrides, confirming the nine grants are runtime.

---

## 3. Fix - the blade

**Cause.** The engine moves the melee weapon out of the first person layer for the length of a swing.
In stock both passes share a field of view, so the move is invisible. With `ViewmodelFieldOfView`
apart from `FieldOfView` it is a blade drawn at the world's field of view, detached and unscaled.

**Fix.** Hook `0x1051B230`. On a disable, if a melee weapon is in hand and no seat is taken, withhold
the clear by zeroing the mask argument, leaving the `+0x130` bookkeeping to the engine, and remember
the component with the bits withheld. Hand the clear over on the first of:

- another component shown through the same call, which is a swap or a mount
- the seat weight rising above zero
- `nSwingMaxMs` (1600) elapsed, checked from both per frame hooks

The release writes `+0x12C` and then calls `FUN_1051a6e0`, and refuses to write at all when the
component no longer reads or no longer carries the withheld bits.

**Two regressions it caused, both measured and both fixed.**

Withholding every hide held the layer through mounts. The arms and the mounted weapon drew over the
vehicle at `ViewmodelFieldOfView` while the world ran at the seat's own. The melee gate and the seat
release cover it.

Releasing by writing `+0x12C` alone left the component reading 0 while every node it owned still
carried 1, so first person geometry drew over the world for the whole ride. Only the turret escaped,
because showing its own components pushed through the engine. `FUN_1051a6e0` is resolved by pattern
and called on release.

---

## 4. Fix - depth matrix rebuilt for nothing

`SyncNearPassDepthMatrix` rebuilt `+0x260` from `+0x210` every frame. That is close to a no-op while
the two fields of view agree, not exactly one, so on frames where the projection was left alone the
derived matrix is a pure loss. `bNearPassRewritten` is published by the projection hook on every path
and the sync returns early when it is false.

This did not fix the seat swim, which is section 6.

---

## 5. Dead ends

- **Promoting nodes at the four layer tests.** Recorded in the previous session and confirmed here as
  unnecessary: the blade has an owner and the owner has the layer.
- **Matching the weapon by component pointer.** The setup is handed a holder whose `+0x00` is the
  weapon's own component; the layer call is handed the `CGraphicComponent`. Different objects.
- **Matching by entity id.** Different entities too, measured `B541842A80350200` at the setup against
  `B5310F2300054100` at the hide in one equip.
- **Pairing the setup with the next show.** The machete's setup arrives with no show near it and the
  next show is the end of a swing, far outside any sane window.
- **`selWeaponClass` alone as the gate.** Mounting while holding the machete runs no setup, so the
  class still reads melee. It is kept as a cheap filter, not as the decision.
- **Withholding every hide.** Section 3.
- **`CFakeWeapon` as the swing model.** Nothing in `Dunia.dll` references `Fake_Machete` by name and
  no second component ever drew the held mesh.

---

## 6. Open

**Viewmodel swim in a car and a glider.** Not this module. Measured in a seat with the trace in the
projection hook: `camera=122.40 near=122.40` in a car and `camera=107.51 near=107.51` in a glider,
`seat=1.00`, `mapVehicle=1` for the car so the hook returns without writing, and `bNearPassRewritten`
false so the depth sync is skipped. Every write this module makes is inactive there and the swim is
unchanged. Next: whether it reproduces with the plugin removed and at stock FOV values, then
`effectsorting`, `dx10fixes`, `internalres`.

**The narrow clamp fires around vehicles.** `camera=91.32` is 59.85 after the stretch, the cutscene
and ladder ceiling. It appears on vehicle entry and exit, once on foot, and held for four seconds in
a glider. World field of view dropping from 107.51 to 91.32 during a drive is a visible snap.

**A second viewport.** Lines with `aspect=1.000 camera=90.00` take `near=128.23`. Probably a mirror
or the map camera. Harmless if offscreen, wrong if it is the GPS screen.

**Smooth transitions, reverted.** One blended weight replacing the three switches (seat, map in
vehicle, cutscene flag) eased correctly on exit and snapped on entry, because the seat channel is the
engine's own blend and arrives fast, and the attempt took the larger of the two. Removing that
override was not retested before the work was reverted, so the approach is untried rather than
disproven.

---

## 7. Method notes

- **Two mid hooks on one address silently win over each other.** The probe and the fix both hooked
  `0x1051B230`; the probe's module is imported later, so the fix never ran and the log showed the
  clear landing exactly as before. One address, one hook.
- **A diagnostic that walks pointers needs the same page checks as shipping code.** The probe's entity
  walk crashed at `RoutingTestProbe+0x441` reading `0x378` on a component whose resolver was junk.
- **Check the log is from this build.** Two runs were read as failures when the file was stale from
  an older build; a field the current build no longer prints is the giveaway.
- **Print a tick on every line.** The swing versus mount question had been guessed at three ways and
  was answered in one run by timestamps.
- **State the release path, not just the state.** Clearing a field the engine also pushes elsewhere is
  half a change. `FUN_1051a6e0` is the other half.
