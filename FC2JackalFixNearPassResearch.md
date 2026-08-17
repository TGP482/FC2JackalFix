# Far Cry 2 Jackal Fix - near pass and FOV research

Session record. Target is `Dunia.dll`, 32-bit, image base `0x10000000`, Steam build. All `FUN_`
names are Ghidra defaults. Continues `FC2JackalFixFOVResearch.md`; the ladder, cutscene, glider and
ironsight work is recorded there.

The session started from "cutscene and intro drive geometry moves with the camera" and ended with
three fixes shipped, one long-standing rule removed, and the machete blade still open.

---

## 1. Status

| Item | State |
|---|---|
| Cutscene near pass scaling against a clamped world | **Fixed.** Decision published from the gameplay thread |
| Intro drive: driver, cab and scenery sliding under camera rotation | **Fixed.** No promotion in a vehicle, near pass follows the camera in a seat |
| Turrets and AI through walls, popping in, low rate animation, broken faces, frame time | **Fixed** by deleting the promotion rule that caused them |
| Machete blade at world FOV while swinging | **Open.** Owner not recoverable from either side, see section 6 |
| Machete blade held idle | Never broken. Do not re-test as though it were |

No diagnostics remain in the tree.

---

## 2. Engine facts established

### The first person layer is a schema property

- `CGraphicComponent+0x12C` is the first person layer mask. `FUN_1051a6e0` pushes it to the render
  node's `+0x90`, and the routing test is `(renderContext+0x24 & node+0x90)`. Measured context mask
  is 1.
- The field is the property `RenderInNearZViewPortID`, string at `0x10E7182C`, registered by
  `FUN_1051e330`, write side `0x1051AAE0`:

```
1051aae0  8B 44 24 04     MOV  EAX,[ESP+0x4]
1051aae4  89 81 2C 01..   MOV  [ECX+0x12c],EAX
1051aaea  E8 ? ? ? ?      CALL 0x1051a6e0
```

- Every write comes through the generic property thunk `FUN_105dd920`, which reads the value off a
  source object via its vtable `+0x11C` and calls the setter stored at property record `+0x10` with
  the field offset at record `+0x14`.
- Measured over one load: **9 components are given 1, 1254 are given 0**, all from that thunk. The
  string appears in **no** entity library XML (all 15 checked, including `20_OA_FakeWeapons.xml`,
  `07_enemy_archetypes.xml`, `30_player.xml`, `42_weapons.xml`), so the value is produced at
  runtime, not declared.
- `0x1051B230` is the enable/disable form: `+0x12C |= mask`, `+0x130 &= ~0x14`, `|= 0x20` when
  enabling; `+0x12C &= ~mask`, `+0x130 &= ~0x20`, `|= 0x10` when disabling. Called through primary
  vtable slot `+0x98`. `0x1051B1C0` (copy the mask from another component) sits at slot `+0xC0` and
  has zero call sites in retail.
- The held weapon's layer is toggled on weapon swap from `0x10156C20`, and it only ever touches one
  component, the first person weapon mesh.

### CGraphicComponent layout

| Offset | Meaning |
|---|---|
| `+0x08` | refcounted resolver node; `+0x0C` of it is the `CEntity`, `+0x08` of that the 64 bit id |
| `+0x12C` | first person layer mask, `RenderInNearZViewPortID` |
| `+0x130` | flags. Constructor `0x81` at `0x1051BBD4`, `\| 0x90` at `0x1051CCA0` |
| `+0x30` | array of pointers to object entries |
| `+0x34` | entry count |

Object entry, measured on the machete:

```
+00: 42ACFE7D 117FF4BB 00000002 42ACFE7D 10FD42D1 4D415246 00000045 ...
```

`42ACFE7D` is `hidNodeName` (`FRAME`), `117FF4BB` is `hidNodeNameLOD0` (`FRAME_LOD0`), then the
inline string. Both match `42_weapons.xml`. `objModel` (`58F2EB0E` for `machette_states123.xbg`) is
**not** kept: it resolves at load and appears nowhere in the component or the entry. `+0x2C` holds
the loaded mesh resource pointer, shared by every component drawing the same `.xbg`, which is the
usable equality test.

### The component update

`FUN_1051cc90` copies the component's flags into each render node it owns:

```
1051ccd9  8B 9E 30 01..   MOV  EBX,[ESI+0x130]   ; owner flags
1051ccea  E8 ? ? ? ?      CALL 0x10456d70        ; fetch the node, writable
1051ccef  89 58 14        MOV  [EAX+0x14],EBX    ; ESI component, EAX node
```

Pattern `6A 01 83 C7 1C 57 B9 ? ? ? ? E8 ? ? ? ? 89 58 14 8B 5C 24 0C`, offset 0x10. The tail
`89 58 14 8B 5C 24 0C` is unique; the head matches four sites.

It is a change path, not a per frame one, and the node it hands over has not been filled in yet:
bounds still read the constructor's `-100000`, position zero. Anything measured there that depends
on node geometry is meaningless.

### The weapon setup

`FUN_10131280`, one call per weapon set up, player and AI alike. `ESI` is **not** the component: it
is a holder whose `+0x04` is the property object the ironsight hook reads and whose `+0x00` is the
component.

```
101315f3  8B 46 04        MOV  EAX,[ESI+0x04]    ; property object
10131616  D9 80 E4 ...    FLD  [EAX+0xE4]        ; fIronsightFOV
1013161c  D9 5F 1C        FSTP [EDI+0x1c]        ; pawn's ironsight channel
```

Entity chain: `*(ESI)` -> `+0x08` resolver -> `+0x0C` entity -> `+0x08` id. Reading `ESI` itself as
the component returns `101269D01003D3A0`, a pointer pair rather than an id.

### Ordering trap

A weapon's graphic component updates **before** the setup that names its entity. Measured:
component update at `t=17634718`, setup at `t=17635078`, and **602 other component updates inside
that 406 ms gap**. Anything that pairs the two by remembering recent components needs a table of
hundreds, not tens, and needs to handle both orders.

---

## 3. Fix - cutscene near pass scaled against a clamped world

**Cause.** `NearPassFieldOfView` re-derived the cutscene test on the render thread from
`nNarrowContext` plus `NarrowStateIsStale()`. Both are written on the gameplay tick. Measured
in-scene: `ctx=2` with `stale=1` and `near=45.01` against `camera=75.01`, held for ten seconds while
the clamp was holding the world at 75.01.

**Fix.** The pawn camera hook, which is the only place that knows whether the clamp is being
applied, publishes `bNearPassFollowsCamera` on every path including its early returns. The render
side reads it and never computes it.

---

## 4. Fix - intro drive

Two separate faults, both visible as "everything slides with the camera".

**Promoted world geometry.** Covered in section 5. In a vehicle the near pass runs at
`ViewmodelFieldOfView` while the world runs at the vehicle's own FOV, so every promoted prop slid.

**The body against the cab.** The player's body and arms are first person geometry the engine routes
itself; the cab is world geometry. Different fields of view put the hands where the wheel is not, by
a gap that grows with distance from screen centre.

`MapIsInVehicle` was meant to cover this and cannot: markers are only placed while the map is open,
so it reads false through a drive nobody opens the map in. Replaced by the engine's own state,
`CPawnFOV+0x34`, the vehicle channel weight, sampled once a frame off the blend hook. Near pass
follows the camera while that weight is above zero, faded by the weight so mounting does not pop.

Cost: `ViewmodelFieldOfView` does not apply in a seat. That is what keeps the hands on the wheel.

---

## 5. Removed - the `0x500` promotion rule

The rule overrode the layer test at all four sites, promoting any node whose owner flags word
(`node+0x14`, a copy of `CGraphicComponent+0x130`) had `0x500` set. It shipped before this session
to make the machete blade follow the viewmodel FOV.

**It is wrong.** Measured in game:

- Around 65 nodes a frame were handed the near mask, at distances up to **278 units** from the eye.
- `flags=00000591` sits on the arms, on their neighbours, and on those distant props alike.
- The only bit that ever differs between a node the engine routes and one it does not is `0x20`,
  which `0x1051B230` writes to record that the mask is set. That is the mask again, not an owner.

**What it cost:** turrets and AI drawn without the world's depth so they showed through walls,
characters sliding under camera rotation whenever near and world FOV differed, animation and facial
LOD picked as though they were first person geometry, and a `VirtualQuery` per submission in the
instrumented builds.

Deleted along with `NodeIsFirstPerson` and the four site hooks. Routing is the engine's again.

---

## 6. Open - the machete blade mid-swing

Idle is correct and always was. Only the swing is wrong: the blade draws at the world field of view,
detached and oversized against the hand, at every `ViewmodelFieldOfView` value including 75.

What is known:

- The blade's node was caught at draw time: unmasked, radius 0.32, 0.48 units from the eye,
  submitted at sites 0 and 2, which matches the earlier "machete uses sites 0 and 2" measurement.
- Its owning component **never passes `FUN_1051cc90`**, so it is not driven by a plain
  `CGraphicComponent` update and a transform-handle map built there cannot name it.
- No second component draws the held weapon's mesh resource. The `HandToHand.Fake_Machete` theory
  from `20_OA_FakeWeapons.xml` produced zero matches at runtime.
- The layer is never taken back off the held weapon's component: a per frame check for a cleared
  `+0x12C` logged nothing.
- Granting the layer to the held weapon's entity works mechanically (entity resolved, setter called,
  mask holds) and changes nothing visible, because that component is not what draws the blade.

Next thing to try, in order of cost: find what allocates and drives that node - the render pool at
`0x10F96D88`, the writer of `node+0x70`, or whatever part system attaches a weapon mesh to the first
person arms. If the node genuinely has no owner reachable at any of those, the remaining option is a
bounded rule at sites 0 and 2 gated on the player holding a melee weapon (`selWeaponClass` 0 at
property object `+0x40`), for unmasked nodes within about a unit of the eye. Measured basis: with a
2.0 unit and 0.15 radius filter, exactly one node qualified in a whole run, the blade itself.

---

## 7. Dead ends

- **`node+0x14` as an owner signature.** Section 5. Same value on the arms and on props 275 units
  away.
- **`node+0x70`/`+0x74`.** Transform and bone instance handles share nothing between the blade and
  the nodes the engine routes.
- **The material record's `FIRST_PERSON` bit** (`record+0x4C` bit 12, written `0x1037EF3B`) says the
  right thing but resolves after all four layer tests (`0x103C6AD7`, `0x103C7D4C`, `0x103C4B83`).
- **`0x1051B1C0`, copy the mask from another component.** Vtable slot `+0xC0`, zero call sites.
- **`RenderInNearZViewPortID` in data.** Not in any entity library XML. The nine grants are runtime.
- **`objModel` at runtime.** Registered by `FUN_10519510` at record offset 8, but that is the
  serialization record; the live entry holds node name hashes and an inline string, and the hash is
  gone. Use the mesh resource pointer at entry `+0x2C`.
- **`CFakeWeapon` as the swing model.** Constructor `FUN_106D34A0` holds one float
  (`fFakeReliability`) and no owner, is built by the generic class factory `0x10656D40`, and no
  second component drawing the held mesh ever appeared.
- **Waiting for a component to come back through the update once the held weapon is known.** It
  never comes back; see the ordering trap in section 2.

---

## 8. Method notes

- **Instrument every site, and print misses.** A probe that only prints on a lookup hit made "the
  map missed" look identical to "nothing was drawn". Two builds lost to that.
- **Check where the data is live.** The component update hands over a node whose bounds are still
  `-100000`; a distance filter there matches nothing and says nothing.
- **`VirtualQuery` and a mutex per node submission costs frames.** Diagnostics at the layer test
  sites run tens of thousands of times a frame. Take pointers straight from the engine's registers,
  keep to one thread, and dump on a timer.
- **The caller's return address names the mechanism.** `[ESP]` at a setter entry turned "who sets
  this" into one line of log twice, and both times it was the answer.
- **Ask what the symptom is before assuming which mechanism it is.** The blade was assumed to be a
  near/world FOV mismatch until it turned out to be broken at 75/75 as well, where no mismatch
  exists.
