# Far Cry 2 FOV research

Reverse engineering record for the field of view work in `fov.ixx`. Everything below was recovered
from `Dunia.dll` (Steam build, image base `0x10000000`, 64228 functions) in Ghidra, and from the
unpacked `patch.dat` / `common.dat` XML. Addresses are default base; rebase in the ASI.

The goal was to stop cutscenes, ladders and hang gliders from coming apart at a wide `FieldOfView`,
by clamping each of them independently of the global setting.

---

## 1. The camera FOV pipeline

The single most useful thing recovered. Everything else hangs off it.

### CCameraComponent

Constructor `0x105051A0`. Two vptrs, primary `0x10E70A70` and secondary `0x10E28B5C`, so every
method called through the second base receives `this = component + 4`. This trips up every offset
in the module unless it is tracked deliberately.

Schema registrar `FUN_10504F70`:

| Offset | Property | Notes |
|---|---|---|
| `+0x10` | `Active` | bool |
| `+0x14` | camera state handle | index into the pool below |
| `+0x20` | `FocusEntityID` | 64 bit |
| `+0x54` | `fCameraBlendTime` | |
| `+0x58` | widescreen blend weight | 0..1 |
| `+0x70` | `fFOV` | **radians** |
| `+0x74` | `fNearDistance` | |
| `+0x78` | `fFarDistance` | |

Constructor defaults confirm the units: `[0x1C] = 0x3FA78D36` = `1.3089969f` = 75 degrees in
radians, `[0x1D] = 0.25f` near, `[0x1E] = 1000.0f` far.

- `fFOV` setter `0x10504CC0`: `MOVSS XMM0,[ESP+4]` / `MULSS XMM0,[0x10E0EED8]` / `MOVSS [ECX+0x70]`.
  `0x10E0EED8` = `0.0174532924` = pi/180. Getter `0x10504CB0` multiplies back by `[0x10E17040]`.
- `OnActivate` `0x10505380`, **primary vtable slot 31** (`+0x7C`). Slot 32 (`+0x80`) is
  `OnDeactivate`; the generic implementation is `0x101D2B60` (`MOV byte ptr [ECX+0x10],0 ; RET`).
  Do not byte scan for that one, the pattern matches twice (`0x10159C2C`, `0x101D2B60`).
- Property changed hook `FUN_10504F00` seeds camera state `+0x28` and `+0x30` from `fFOV`.

### The live camera state

A `0x84` byte struct in a **copy on write pool** at `0x10F96CA8`, fetched by
`FUN_10451AE0(pool, &handle, writable)`. Allocated by `FUN_1045DBA0` over `FUN_10228F30(0x84, 0)`.

| Offset | Meaning |
|---|---|
| `+0x08 .. +0x10` | position |
| `+0x14 .. +0x1C` | position fraction |
| `+0x20` | near plane |
| `+0x24` | far plane |
| `+0x28` | **world FOV, radians** |
| `+0x2C` | snapshot of `+0x28` |
| `+0x30` | **first person model FOV, radians** |
| `+0x34` | snapshot of `+0x30` |

**The address is not stable.** When the dirty bit is set, `FUN_10451AE0` takes a different block off
a free list or allocates a new one and returns that instead; otherwise it returns the existing
pointer. Nothing may be keyed on this pointer across frames. This cost a full debugging cycle, see
section 7.

### The per-frame funnel

`CCameraComponent::Update` = `FUN_10504D50`, called with `this = component + 4` from six drivers:

| Driver | Camera |
|---|---|
| `FUN_10692050` | |
| `FUN_10692630` | |
| `FUN_10693C00` | **first person pawn camera** |
| `FUN_10694D90` | |
| `FUN_10695BC0` | |
| `FUN_10CD4DC0` | `CCameraBoneComponent`, which is what `Camera.Cinematic` is |

The widescreen Hor+ transform lives **inside** `FUN_10504D50`:

```
out = 2 * atan(tan(in * 0.5) * 0.75 * aspect)
```

Constants: `0.5` at `0x10E9E418`, `0.75` at `0x10EAF2F8`, `2.0` at `0x10E22D80`. Gated on
`[[0x11609560] + 0x40]`, the `WidescreenFOV` console variable (`FUN_10402210`).

This matters more than it first appears. Anything clamped **before** `FUN_10504D50` is clamped in
the same units as the ini `FieldOfView`; anything clamped after is clamping the widened result.
At 16:9 the two differ by a factor of `0.75 * 16/9` = 1.3333 in tangent space, so a base 75 renders
as 91.31 and a base 59.85 renders as 75.01. At 4:3 the transform is identity, because
`0.75 * 4/3` = 1 exactly.

### CCameraPawnComponent

`GetClassInfo` `0x10659250`, class size `0x140`. `Update` = `FUN_10693C00`.

| Offset | Meaning |
|---|---|
| `+0x104` | computed world FOV, feeds state `+0x28` |
| `+0x108` | zoom / aim FOV, `> 0` selects it over `+0x104`; computed by `FUN_10692E30` |
| `+0x10C` | debug FOV, written only by the `set_debug_fov` console command (`0x1070D090`) |
| `+0x110 .. +0x11C` | `NoiseFOVEnabled` / `TimeCount` / `Target` / `Current`, schema `FUN_10692FF0` |

The relevant tail of `FUN_10693C00`, and the site `fov.ixx` hooks:

```
10694312  F3 0F 10 87 08 01 00 00   MOVSS  XMM0,[EDI+0x108]
1069431A  0F 2F 05 9C 9A E0 10      COMISS XMM0,[0x10E09A9C]   ; 0.0f
10694321  76 11                     JBE    0x10694334
10694323  D9 87 08 01 00 00         FLD    [EDI+0x108]
10694329  D9 5E 28                  FSTP   [ESI+0x28]
1069432C  D9 87 08 01 00 00         FLD    [EDI+0x108]
10694332  EB 0C                     JMP    0x10694340
10694334  D9 87 04 01 00 00         FLD    [EDI+0x104]
1069433A  D9 5E 28                  FSTP   [ESI+0x28]
1069433D  D9 47 6C                  FLD    [EDI+0x6C]          ; fFOV
10694340  8B 4C 24 3C               MOV    ECX,[ESP+0x3C]      ; the pawn the camera follows
10694344  D9 5E 30                  FSTP   [ESI+0x30]
10694347  8A 9E 80 00 00 00         MOV    BL,[ESI+0x80]       ; <- hook here
1069434D  E8 ? ? ? ?                CALL   0x1007E1F0
10694352  D9 45 08                  FLD    [EBP+0x8]           ; dt
10694362  E8 ? ? ? ?                CALL   0x10504D50          ; widescreen transform
```

`ESI` is the camera state, `EDI` is `component + 4`, `[EBP+8]` is the frame delta. The hook is
anchored at `0x10694347` rather than `0x10694340` because one instruction earlier the x87 stack
still holds the value the `FSTP` is about to pop.

---

## 2. Cutscenes

`Camera.Cinematic` has no class of its own. It is a `CCameraBoneComponent` with the `Cinematic`
flag its schema adds at `+0x90` set.

- `GetClassInfo` `0x10659510`, ctor `FUN_10CD4930`, schema `FUN_10CD4960` (adds `Cinematic` `+0x90`,
  `Bone` `+0x94`), primary vtable `0x10F46F00`, secondary `0x10F46EEC`.
- `OnActivate` `0x10CD4AE0` branches on `Cinematic` and attaches a `CDynLoadComponent`
  (`0x10E23E5C`, GetClassInfo `0x10178990`), which streams the world around the shot. That is
  exactly what a cutscene camera needs and a plain bone camera does not.
- `OnDeactivate` `0x10CD4B40`, symmetric, also gated on `Cinematic`.
- `Update` `FUN_10CD4DC0` is the only path from the archetype's `fFOV` to the render camera:

```
10cd4f55  D9 47 6C        FLD  [EDI+0x6C]     ; fFOV, radians   <- hook here
10cd4f58  8B 54 24 40     MOV  EDX,[ESP+0x40]
10cd4f5c  D9 5E 28        FSTP [ESI+0x28]
10cd4f5f  D9 47 6C        FLD  [EDI+0x6C]
10cd4f63  D9 5E 30        FSTP [ESI+0x30]
```

Both stores read the same field, so rewriting it ahead of the first `FLD` covers both.

**The bone camera is only half the cutscene work.** In-game logging showed the scenes that were
still uncapped run entirely on `CCameraPawnComponent`: an active pawn camera carrying the full
`FieldOfView` next to an inactive bone camera already clamped. Those are first person dialogue
scenes; `CCameraBoneComponent::Update` is never reached for them. They are handled through the
beautifier instead, section 3.

The eight camera archetypes, all `fFOV` 75, live in
`patch_unpack/worlds/tmpla/generated/entitylibrary/04_cameras.xml`: `Camera.Bone`,
`Camera.Cinematic`, `Camera.Editor`, `Camera.First`, `Camera.Free`, `Camera.Ghost`,
`Camera.Spectator`, `Camera.Third`.

---

## 3. Ladders and first person scenes

**Far Cry 2 has no ladder FOV.** A full sweep of the module's 26 `ladder` strings and 54 `fov`
strings finds no overlap. `CPawnBeautifierLadder` carries only `angMinLookAngle` and
`angMaxLookAngle`; `CLadder`'s schema is `iOverrideLadderSteps`, `Users`, `sndClimbSound`. So the
clamp needs the player's *state*, and the beautifier is where that lives.

### CPawnBeautifierComponent

Schema `FUN_100E9920`:

| Offset | Property |
|---|---|
| `+0x08` | owner entity ref |
| `+0x14` | `Enable` |
| `+0x15` | `ApplyDisplacement` |
| `+0x18` | `Context`, a StringID |
| `+0x1C .. +0x24` | cached context tag triple |
| `+0x28` | `ContextBeautifier`, live instance pointer |
| `+0x2C` | `TypeBeautifier`, live instance pointer |

`Update` = `FUN_100E96D0`, entered with `ECX = component + 4`. It calls the reselect
`FUN_100E9380`, which recomputes the tag via `FUN_100E9230` and swaps `+0x28` / `+0x2C`.

**`FUN_100E9380`'s body is wrapped in `if (tag differs)`.** It runs only on transitions. This is the
single most important fact in this section and the cause of the stuck-clamp bug in section 7.
The whole function is also gated on `FUN_100D4710() != 0`, the beautifier repository.

### Identifying the context

Two live instances per pawn, and the class of each is the whole test:

- `ContextBeautifier` names the situation: `CPawnBeautifierLadder`, `CPawnBeautifierDominoPlayer`,
  `CPawnBeautifierCinematicFirst`, `CPawnBeautifierFirstNoControl`, `CPawnBeautifierThird`, ...
- `TypeBeautifier` names whose pawn it is: `CPawnBeautifierPlayer` vs `CPawnBeautifierAI`.

The type half is what separates the local player from the dozen AI pawns updating on the same
frame. It replaced an earlier attempt to match the camera's target pointer against the beautifier's
pawn, which does not work: they are different objects on different entities, and their entity ids
differ (camera `...3900`, pawn `...3A00` in every capture).

Observed in game during a Domino scene:

```
CAMERA CCameraPawnComponent  comp=4BB08680 active=1  fFOV=140.00  world=149.46  model=149.46
CAMERA CCameraBoneComponent  comp=1903DDE0 active=0  fFOV= 59.85  world= 75.01  model= 75.01
PAWN   comp=196F54C0 contextId=91FCE9E2  ctx=CPawnBeautifierDominoPlayer  type=CPawnBeautifierPlayer
PAWN   comp=19632D00 contextId=10F7D787  ctx=CPawnBeautifierThird         type=CPawnBeautifierAI
```

Context ids seen: `0x91FCE9E2` Domino player, `0x10F7D787` third/AI.

Beautifier data lives in `patch_unpack/worlds/tmpla/generated/entitylibrary/02_Beautifiers.xml`.

### Dunia's RTTI

Every class exposes `GetClassInfo` at **primary vtable slot 1**, returning a lazily built
descriptor:

```c
struct DuniaClassInfo {
    const char* name;        // +0x00, .rdata literal, never NULL
    uint32_t    depth;       // +0x04, chain length including self
    uint32_t    hashChain[]; // +0x08, base-most first, self last
};
```

Built by `FUN_1084AEE0(ci, name, parent)`; `selfHash = ci + 0x08 + 4 * (depth - 1)`. Verified on
`CCameraComponent` (`0x10E70A70` slot 1 -> `0x10050690` -> `"CCameraComponent"`),
`CCameraBoneComponent` (`0x10F46F00` -> `0x10659510`) and `CCameraPawnComponent`
(`0x10E92950` -> `0x10659250`).

`fov.ixx` uses this to name a beautifier instance, cached per vtable so the indirect call happens
once per class rather than once per frame, with every pointer range and page checked. The
alternative, capturing each vtable from a class-exclusive function, needs one exclusive entry point
per context and only `CPawnBeautifierLadder::OnActivate` (`FUN_100A2300`, vtable slot `+0x84`,
the single reference to that function in the module) has one.

### Component to entity

- `component + 0x08` = refcounted resolver node. Never null; `CComponent`'s ctor `FUN_100421F0`
  seeds it with a shared empty one.
- `node + 0x08` = refcount, `node + 0x0C` = `CEntity*`, **can be null** when unloaded.
- `entity + 0x08` = 64 bit entity id.
- `entity + 0x10` = Dunia string, the entity name.

All three offsets come from the Domino native `GetEntityName` worker `0x10590650`. In practice the
name came back **empty** for every camera and pawn entity at runtime, so class plus context is what
identifies these, not names.

---

## 4. Hang gliders

The glider camera FOV is the vehicle's own `fFOVAngle`. `CVehicle` schema `FUN_100DEA10`:

| Offset | Property |
|---|---|
| `+0x00` | `sName`, hash of the vehicle's own name string |
| `+0x114` | `fUnderWaterMaxDepth` |
| `+0x230` | `fFOVTransitionTime` |
| `+0x234` | `fFOVAngle`, **degrees** |
| `+0x238` | `archFOVCurveName`, Dunia string |
| `+0x254` | resolved FOV curve handle |

`0x7B2D589C` is the `paraglider` name hash, shared by every `Air.Paraglider*` prototype and no other
vehicle. `glider.ixx` already relies on the same pair.

Exactly two runtime readers of `+0x234`, both the same block inlined twice, both pushing the value
into the local player's `BaseFOV` override channel:

```
100dc109  8B CB           MOV   ECX,EBX
100dc10b  E8 ? ? ? ?      CALL  0x1007E1F0        ; = *(pawn+0x10) + 0xD8
100dc110  D9 86 30 02..   FLD   [ESI+0x230]       ; fFOVTransitionTime
100dc116  D9 58 2C        FSTP  [EAX+0x2C]
100dc119  8B 8E 54 02..   MOV   ECX,[ESI+0x254]   ; resolved curve
100dc11f  89 48 3C        MOV   [EAX+0x3C],ECX
100dc122  F3 0F 10 86 ..  MOVSS XMM0,[ESI+0x234]  ; <- hook here, still degrees
100dc12a  F3 0F 59 05 ..  MULSS XMM0,[0x10E0EED8] ; deg -> rad
100dc132  F3 0F 11 40 38  MOVSS [EAX+0x38],XMM0
100dc137  C6 40 29 01     MOV   byte [EAX+0x29],1
```

- `0x100DC122` in `FUN_100DC0A0`, `OnCharacterEnterSeat`, the mount.
- `0x100D721C` in `FUN_100D71C0`, the seat change path from the get-in and swap-seat handlers.

`ESI` is the `CVehicle` at both, so `*(uint32_t*)(ESI + 0) == 0x7B2D589C` gates to paragliders.

### CPawnFOV

Class name `0x10E20B9C`, ctor `FUN_10140610`, schema `0x10140710`. Embedded at **pawn data +0xD8**
by `FUN_10085870`. Two identical override channels:

| Channel | Base | Property name |
|---|---|---|
| Zoom | `+0x08` | `ZoomFOV` |
| Base | `+0x24` | `BaseFOV` |

Per channel, relative to its base:

| Offset | Field |
|---|---|
| `+0x05` | `FovOverrideEnabled` |
| `+0x08` | `FovOverrideTransitionTime` |
| `+0x0C` | `FovOverrideTransitionPercent` |
| `+0x10` | `FovOverrideMagnitude` |
| `+0x14` | `FovOverride`, **radians** |
| `+0x18` | curve handle |

Writing `fFOVAngle` rather than the channel leaves the transition and its curve intact, since the
value written is the blend *target*.

**Correction to an earlier finding.** A first pass concluded the `FovOverride*` schema strings at
`0x10E20BC8 .. 0x10E20C28` had zero cross references and were dead in retail. That was wrong. They
are the `CPawnFOV` schema, registered by `FUN_101408D0` / `FUN_10140710`, and the vehicle FOV path
above writes through them every time the player mounts.

Two helpers worth recording:

- `FUN_1007E1F0(pawn)` = `*(void**)(pawn + 0x10) + 0xD8`, the pawn's `CPawnFOV`.
- `FUN_1007E1D0(pawn)` = `*(uint8_t*)(*(void**)(pawn + 0x10) + 0x499)`, the player/AI discriminator
  the beautifier tag uses.

---

## 5. Iron sights and magnified optics

Weapon FOV comes out of the FCB archives, so it is intercepted during property deserialization
rather than at a call site. Dunia identifies properties by CRC-32, which means one can be caught
without knowing its owning class or offset.

| Property | Hash | Units |
|---|---|---|
| `fFOVAngle` | `0x49745480` | degrees |
| `fIronsightFOV` | `0xFB4ADD00` | radians |
| `fFOV` | `0xBEF721BA` | radians on gadgets, **degrees** on camera archetypes |

The deserialization pattern is
`8B 54 24 04 8D 44 24 04 50 83 C2 04 52 E8 ? ? ? ? 85 C0 74 0D 8B 00 8B 4C 24 08 89 01 B0 01 C2 08 00`:
the entry hook caches the descriptor hash from `[ESP+4]`, and the hook at `+0x18` replaces the
parsed float in `EAX` before it is written out.

### The weapon data

34 `fIronsightFOV` entries in `21_WeaponProperties.xml`. Seven sit below the 40 degree magnified
optic cutoff:

| Weapon | radians | degrees | renders at 16:9 |
|---|---|---|---|
| `Primary.AS50.Multi` | 0.2000 | 11.46 | 15.24 |
| `Primary.Dragunov.Multi` | 0.2800 | 16.04 | 21.28 |
| `Special.M1903.Multi` | 0.2850 | 16.33 | 21.66 |
| `Primary.MGL140.Multi` | 0.2900 | 16.62 | 22.04 |
| `Special.Dart_Rifle.Multi` | 0.3000 | 17.19 | 22.79 |
| `Special.Carl_Gustaf.Multi` | 0.5000 | 28.65 | 37.60 |
| `Primary.M16.Multi` | 0.6000 | 34.38 | 44.83 |

The other 27 span 42.97 to 75 degrees over fifteen distinct values.

The map scope is the `fFOV` property on `Equipped.Binoculars` and `Equipped.Monocular` in
`09_gadgets.xml`, both `0.2` rad. It shares its CRC with the camera archetypes' `fFOV`, which is in
degrees and always 75, so a value below 2 rad (114 degrees) separates the two safely.

### The 46.70 / 59.85 pair

The ini documents the stock ironsight as 46.70 non-widescreen and 59.85 widescreen. Those two are
**exactly one Hor+ stretch apart**, `tan(29.925) / tan(23.35)` = 1.3333 = `0.75 * 16/9`. That is
why walking `IronsightFieldOfView` from 46.70 to 59.85 in tangent space walks every magnified optic
from its own non-widescreen value to its own widescreen value:

| Setting | 46.70 | 50.00 | 55.00 | 59.85 |
|---|---|---|---|---|
| AS50 | 11.46 | 12.37 | 13.80 | 15.24 |
| Dragunov | 16.04 | 17.31 | 19.28 | 21.28 |
| Carl Gustaf | 28.65 | 30.84 | 34.23 | 37.61 |
| M16 | 34.38 | 36.95 | 40.92 | 44.83 |
| Binoculars / map scope | 11.46 | 12.37 | 13.80 | 15.24 |

Tangent space rather than degrees, so an optic keeps its magnification relative to the sights
instead of gaining a fixed number of degrees. The AS50 moves 3.8 degrees across the band while the
M16 moves 10.5.

---

## 6. Verified byte patterns

Every one confirmed to match **exactly once** in the module. Offsets are for `pattern.get_first(N)`.

| Site | Address | N | Pattern |
|---|---|---|---|
| Bone camera FOV store | `0x10CD4F55` | 0 | `D9 47 6C 8B 54 24 40 D9 5E 28 D9 47 6C 52 D9 5E 30` |
| Pawn camera FOV, post store | `0x10694347` | 10 | `D9 47 6C 8B 4C 24 3C D9 5E 30 8A 9E 80 00 00 00` |
| Beautifier update entry | `0x100E96D0` | 0 | `55 56 8B E9 57 8B 7D 04 8B 77 0C 83 47 08 01 8B CE E8 ? ? ? ? 83 3D ? ? ? ? 00 75 07` |
| Beautifier reselect tail | `0x100E94B2` | 9 | `EB 07 C7 46 28 00 00 00 00 F3 0F 7E 44 24 0C` |
| Ladder beautifier OnActivate | `0x100A2300` | 0 | `55 56 57 8B F9 8B 47 10 8B 70 08 8B 6E 0C 83 46 08 01 8B CD E8 ? ? ? ? 83 3D ? ? ? ? 00` |
| Glider FOV, enter seat | `0x100DC122` | 0x19 | `8B CB E8 ? ? ? ? D9 86 30 02 00 00 D9 58 2C 8B 8E 54 02 00 00 89 48 3C F3 0F 10 86 34 02 00 00 F3 0F 59 05 ? ? ? ? F3 0F 11 40 38 C6 40 29 01` |
| Glider FOV, seat change | `0x100D721C` | 0x1D | `8B 4C 24 08 74 ? E8 ? ? ? ? D9 86 30 02 00 00 D9 58 2C 8B 8E 54 02 00 00 89 48 3C F3 0F 10 86 34 02 00 00 F3 0F 59 05 ? ? ? ? F3 0F 11 40 38 C6 40 29 01` |
| Camera update funnel epilogue | `0x10504E83` | 12 | `D9 47 70 D9 5E 20 D9 47 74 D9 5E 24 D9 46 28 D9 5E 2C D9 46 30 D9 5E 34` |
| Any camera OnActivate | `0x10505380` | 0 | `56 8B F1 D9 46 54 6A 01 8D 46 14 D9 5E 58 50 B9 ? ? ? ? C6 46 10 01` |
| Bone camera OnActivate | `0x10CD4AE0` | 0 | `56 8B F1 E8 ? ? ? ? 80 BE 90 00 00 00 00 74 4C 8B 76 08` |
| Bone camera OnDeactivate | `0x10CD4B40` | 0 | `57 8B F9 80 BF 90 00 00 00 00 C6 47 10 00 74 78 53 56 8B 77 08 8B 5E 0C` |
| `SetCinematicUIMode` impl | `0x10724BF0` | 0 | `83 EC 24 53 56 57 E8 ? ? ? ? 83 40 08 01 89 44 24 10 8D 44 24 10` |

The two glider patterns each carry their caller's own instructions ahead of the shared block,
because the block on its own matches both sites.

---

## 7. Bugs found, and their causes

Recorded because each one cost a build and none of them is obvious from the code.

**`std::min` against `Windows.h`.** `common.hxx` includes `Windows.h` without `NOMINMAX`, so `min`
is a macro and `std::min(` expands to `std::((a) < (b) ? ...)`. 41 compile errors, all cascading
from four lines. `std::clamp` is unaffected, which is why the existing code never hit it.

**FOV strobing.** The blend owner was keyed on the camera state pointer. That pointer is
copy on write, so the owner test passed only on the frames the pool happened to reuse the buffer
and bailed on the rest. The clamp applied on some frames and not others. Keyed on the component
instead, which is a real object.

**Clamp stuck after quickload.** The context was written from `FUN_100E9380`, whose body only runs
on a context change. Quickloading out of a scene destroys the pawn holding the context with no
transition to clear it, so the clamp stayed on for the rest of the session and `FieldOfView` did
nothing. Fixed by reading from `FUN_100E96D0` every frame and expiring the state after 250 ms, so
anything that stops the player's beautifier ticking releases it.

**Weapon flung wide on the first frames of aiming.** The viewmodel used a fixed widening ratio faded
out over the top 15 percent of tangent space. At `FieldOfView` 140 that window is the first 7
percent of the aim sweep:

```
  0% aim   camera 149.46  ->  viewmodel  91.31
  2% aim   camera 147.97  ->  viewmodel 121.64
  5% aim   camera 145.74  ->  viewmodel 139.19
  7% aim   camera 144.25  ->  viewmodel 144.25
100% aim   camera  75.01  ->  viewmodel  75.01
```

The same window also left the weapon completely unscaled in vehicles, whose camera sits below it
whenever `VehicleFieldOfView` is under `FieldOfView`.

**Viewmodel could not tell a clamp from a zoom.** Replacing the ratio with an absolute cap
(`min(camera, ViewmodelFieldOfView)`) fixed aiming but broke ladders, because both leave the camera
narrower than `ViewmodelFieldOfView`. Three candidate rules, at `FieldOfView` 91.31 and
`ViewmodelFieldOfView` 75, on a ladder with the world at 75.01:

| Rule | Ladder viewmodel | Problem |
|---|---|---|
| Follow the camera | 75.01 | shows the world's `FieldOfView`, not the setting |
| Ignore the clamp | 91.31 | unchanged from gameplay, clamp looks like it is not applying |
| Scale by the clamp | 59.85 | correct |

Scaling by the clamp factor keeps the weapon and world at the same tangent ratio on a ladder as in
gameplay, 0.750 either way.

**Ladder pulled in then opened back out.** Mounting a ladder takes control away from the player, so
the pawn sits in a scene context for the mount animation and only then becomes
`CPawnBeautifierLadder`. On separate ceilings that read as 59.85 for the mount and 75 for the climb.

**A hook that silently never installed.** `cutscenelog.ixx` anchored a pattern on the beautifier
reselect convergence. `fov.ixx` hooks the same convergence and, being imported first, had already
written its jump over the bytes the pattern was scanning for. The only symptom was `beautifier=0`
in the log's init line. Worth remembering whenever two modules touch one function.

---

## 8. Dead ends

Recorded so they are not tried again.

- **`CGOStateLadderTransition`** (`0x10F47D98`) declares an `OnLadder` bool (`0x10F47E00`) plus
  `ladderID`. Serialization only; its accessors `0x10CE5C00` / `0x10CE5C50` are generic reflection
  thunks and there are zero code xrefs to the class name or property names.
- **`CLadder`'s `Users` array** (`+0x2C`) and the `leave_ladder_top` / `leave_ladder_bottom` string
  id globals (`0x10FE4E20`, `0x10FE4E24`) are reachable only from their own static initialisers at
  `0x10DAF307` / `0x10DAF327`. No consumer xrefs Ghidra can resolve.
- **`InsideTerminalFOV` / `EnterFOV` / `LeaveFOV` / `fFOVPercentage`** at `0x10E1819C` look like a
  climb or use-object camera transition. They are not: `FUN_100BA6F0` registers them as a terminal
  usage schema, `InitalTestDone +0x14`, `InsideTerminalFOV +0x15`, `fFOVPercentage +0x18`,
  `fRange +0x1C`. Unrelated to camera FOV.
- **There is no named cutscene API.** The module contains four `cinematic` strings total:
  `CPawnBeautifierAICinematic`, `CPawnBeautifierCinematicFirst`, `SetCinematicUIMode`, and the
  `CCameraBoneComponent` schema property `Cinematic`. No sequence loader, no cutscene name format
  string. The owning entity's name is the closest thing to an identifier, and it reads empty.
- **`CDominoSequenceManager`** (`0x10E76758`) exposes only `GetInstance`, `CreateListener`,
  `DeleteListener` through `FUN_1058BCB0`. Not a cutscene player.
- **`CGOStateEventCamera`** (`0x10F47B18`) does carry a `cameraName` string, read by its
  deserializer at `0x10CE0C90`, but it belongs to the GO state machine (bedroll, heal, rescue) and
  is consumed at load time.
- **The `hidName` to prototype map** built by `FUN_105492E0` is not a plain global; it lives at
  `this + 0x10` of the prototype manager passed in `ECX`, and the owning global was not found.
  Moot, since `entity + 0x10` gives the same name directly.
- **`hidHasAliasName`** (`0x10E6B518`) has no reachable runtime accessor on `CEntity`.
- **Clamping the post-transform funnel** (`0x10504E8F`, inside `FUN_10504D50`) is the obvious single
  choke point for all camera types, and it is wrong: the widescreen transform has already been
  applied there, so it caps the widened result rather than the quantity the ini feeds.

---

## 9. What shipped

`fov.ixx`, in addition to the pre-existing `FieldOfView`, `ViewmodelFieldOfView`,
`IronsightFieldOfView`, `VehicleFieldOfView` and widescreen-blend fixes:

| Context | Ceiling | Mechanism |
|---|---|---|
| Cutscene, bone camera | 59.85 | rewrite `fFOV` in `CCameraBoneComponent::Update` |
| Cutscene, first person | 59.85 | beautifier context, blended on the pawn camera |
| Ladder | 59.85 | same path, same ceiling |
| Hang glider | 75 | rewrite `fFOVAngle` on both seat sites, gated on the paraglider hash |

All ceilings are pre-Hor+, floor 45, and each follows `FieldOfView` up to its own ceiling. The
first person clamp blends over 0.35 s with a smoothstep, driven by the frame delta at `[EBP+8]`,
with deltas above 100 ms discarded as loads or hitches.

Glider note: at the stock `FieldOfView` 75 the paraglider now runs 75 rather than its data value of
90, which incidentally fixes the arm-seam bug the modding guide patches by hand to 81.

---

## 10. Open items

- The `fov.ixx` currently in `source/` is two revisions behind the last state reached in the
  session. It has neither the viewmodel clamp-versus-zoom fix (`fNarrowTangentScale`) nor the
  magnified optic and map scope scaling from section 5.
- `NarrowContext` still distinguishes ladder from cutscene although both take the same ceiling.
  That is deliberate; it is the one line to change if they need to diverge.
- The pawn camera clamp resets its blend when the active camera changes. If two cameras are briefly
  active together during a `fCameraBlendTime` crossfade the ramp will restart rather than age out.
  Not observed, but the failure mode is known.
- `phonecalllog.ixx` is still in `source/` and imported by nothing.
- `_to_delete/` folders exist under both connected folders and can be removed.
