# Culling and LOD — engine analysis (2026-10-01)

Static analysis of AssassinsCreed_Dx9.exe v1.02 (Ghidra decompilation + capstone disassembly of the exe; no live
session was available). Implementation: `mod/src/anticull.{h,cpp}`. Every value used there is listed below with the
function it was read from. Occlusion culling (DX9ViewSurface+0x6D) is already disabled by camera.cpp and is not
repeated here.

## 1. Pipeline, from world to draw item

```
FUN_009E6600 (all views)  /  FUN_009E5390 (single view, from FUN_00A4C600)
  for each view V (ViewSurface list; V+0x178 = its ViewSurface, set by ViewSurface::AddView FUN_009E2000):
    X = [V+0x1E0]                                     visibility result object (ctor FUN_00A30050)
    FUN_00A2FFD0(X, V)                                per-view setup
      FUN_00A2F890: X+0x2B0 = V; planes V+0x180 -> X+0xC0 (6 x float4); X+0x2AC = LOD fov scale
                    (FUN_00FA0510 * 2.414213); far-LOD grid bits -> X+0x2B8 (FUN_005E43B0)
      FUN_00A2FCC0: builds the query Y = X+0x750 (planes Y+0, eye Y+0x1F0, clip flags Y+0x204 = 0x42,
                    callbacks Y+0x260 = 0xA2F770 push object -> X+0xA40, Y+0x264 = 0xA2F7A0 push light -> X+0xA4C,
                    enabled lists Y+0x29C = 3, pass bits Y+0x2A4..0x2B0 = 1 / ~1 / 2 / ~2),
                    FUN_00AC6E90: cull distances Y+0x2D8..0x2EC, FUN_00AC6FC0: cell bitsets Y+0x2B8/+0x2C4/+0x2D0
    gw = [[V+0x60]]+0x98 (graphic world)
    FUN_009F3C80(X)  particles / FX pre-steps (not render-object culling)
    FUN_009F1D00(X)  -> FUN_00AA03F0: sky/sun (FUN_00A00E00) pushed to the "always" lists X+0xA58..0xA98
                     -> FUN_00AA0940 -> FUN_00AC6B90(container = [gw+0xC], Y, 0)
                          list 0 container+0x5C (render objects): FUN_00AC6020 -> FUN_00AC5CE0 per object
                          list 1 container+0x68 (lights):          FUN_00AC6110 -> FUN_00AC5860 per light
                          list 2 container+0x50:                   FUN_00AC6200 (disabled, Y+0x29C bit 2 = 0)
    FUN_009F1450(X)  -> Lights_Update FUN_00AA1AD0
Render side (FUN_00AC0F30, per view):
  FUN_00A9F910(sets = renderer+0x1DC, X): for each visible object FUN_00A9F420 -> component vtable slot 21 (+0x54)
      "AddToRenderSets(object, sets, id, group)":
      - LODSelectorInstance (vtable 0x016C2344): FUN_00A90160  (LOD selection, section 3)
      - GraphicObjectInstanceData / MeshInstanceData / DynamicSubMeshInstance / TerrainInstance: FUN_009F12D0
        (no LOD: add [this+8] handle, pass mask [this+0xE], group [this+0xF])
      - ParticleSystemInstanceData (vtable 0x016BFC84): also FUN_009F12D0
    FUN_00A9F870: always-drawn lists X+0xA58+k*8 (12-byte {object, handle, passMask}) into group k
  FUN_00A9F540 AddToRenderLists(handle, object, id, passMask, group, fade, stencil): 20-byte item
      {object, handle, u16 id, u8 fade, u8 stencil} pushed into list (pass + group*6) for every set bit of
      passMask & object+0x20; shadow cascades from object+0x94 into lists 24/30/36/42. No capacity check.
  RenderBatch_DrawItems 0xABB0E0 draws the lists (item+0xA fade/255 -> ctx+0x64 LOD blend factor).
```

Render object (the Submit `placement`, registered by FUN_00AA2880, reset FUN_00AA2620): +0x00/+0x10 world AABB
min/max, +0x20 pass bits (bit0 camera, bit1 shadow), +0x24 bits 0-2 type (1 = light) / bits 3-10 light channels,
+0x40 world, +0x84 entity, +0x94 shadow cascade mask, +0xA0 flags (bits 0-3 component count, bits 4-5 cell
bitset index, bits 6-19 grid cell id = entity+0xAA, bit 20 small, bit 21 medium, bit 26 hidden, bit 28 shadow
caster, bit 29 cell state), +0xA4 bits 0-2 LOD class, +0xA8 distance eye -> AABB (written by FUN_00AC5CE0),
+0xB0 component pointers.

## 2. Culling stages (render objects, list 0, FUN_00AC5CE0)

| # | Stage | Where / data | Patched |
|---|-------|--------------|---------|
| a | hidden flag | +0xA0 bit 26 = entity+0x60 bit 5 (FUN_00AA2880) — gameplay "invisible" | no |
| b | far-LOD cell swap | bit (7-(id&7)) of byte [Y + (cat+0x3A)*0xC] + id>>3; cat = +0xA0 bits 4-5 | no (section 4) |
| c | small / medium distance | bit 20 && d > Y+0x2D8, bit 21 && d > Y+0x2DC (d = eye->AABB distance) | yes: FLT_MAX |
| d | light-channel mask | FUN_00AC5860: channelMask Y+0x2A0 (0 = off) vs byte(+0x24 >> 3) | no |
| e | camera frustum | FUN_00AC5860 -> FUN_009F4010(planes Y+0, AABB): 1 in, 3 intersect, 2 out | yes |
| f | occlusion queries | RenderBatch_DrawItems occMode (already off via DX9ViewSurface+0x6D) | (existing) |
| g | LOD slot empty | FUN_00A90160: no instance / handle for the selected LOD -> nothing added | via ForceMaxLod |

Lights (list 1) only go through (d) and (e).

Distance values: Y+0x2D8 = surface settings(+0x24)+0x7C / options+0x80, Y+0x2DC = settings+0x84 / options+0x84
(FUN_00AC6E90); options = FUN_009DA400(), where options+0x80/+0x84 are the copies of Assassin.ini
`SmallObjectsCullDistanceModifier` / `MediumObjectsCullDistanceModifier` (FUN_009D9600 writes +0x44/+0x48 and
copies +0x1C..+0x57 to +0x58). Y+0x2E0/+0x2E4 = same for shadow casting, Y+0x2E8 entity shadow distance,
Y+0x2EC max cascade. FUN_00AC7120 initialises all of them to 0x7F7FFFFF (FLT_MAX = "no limit").

Frustum flags (FUN_00AC5860): input clip 0x42 (0x2 test camera frustum, 0x40 test shadow volume). Result 2
(outside): object+0x20 &= visibleMask, clip = clip & ~2 | 4, then shadow-volume test FUN_009F4660 (0x10 in /
0x20 out). Result 1: |= visibleSet, clip = clip & ~0x42 | 0x11. Result 3: |= visibleSet. The caller accepts the
object if (result & Y+0x244[list]) != 0 (list 0: 0x53, list 1: 3) and calls the push callback.

No portal / sector / anti-portal step exists in this runtime path: the container lists are flat, and the
GraphicPortalComponent (0x016C2574) / GraphicAntiPortalComponent (0x016C25FC) vtables are referenced only by
their constructors/destructors (0xA93E30/0xA93F70, 0xA94010/0xA94120).

Not render culling (left alone): `Graphic::UpdateEntitiesCullingFlags` task FUN_00459A60 -> FUN_0044FD10 per
entity (entity+0x60 bit 1 = in frustum via FUN_009F47D0, +0x80 = scaled distance^2) -> FUN_0044E6A0 entity
LODLevel (entity+0x60 bits 12-14: 0 not visible, 1..5 by distance^2 < {FLT_MAX, 400, 100, 25, 4} at 0x18C62BC,
clamped by Assassin.ini MaxNPCLODLevel = options+0x7C for NPCs). This drives AI/animation update level, not which
mesh is drawn.

## 3. LOD selection (LODSelectorInstance::AddToRenderSets FUN_00A90160)

`__thiscall(instance, object, sets, id, group)`, ret 0x10. res = [instance+0x10] (scimitar::LODSelector):

- LODSelector: +0x0C five LOD descriptors of 0x10 bytes (+4 far distance, +8 blend width; defaults 20/40/80/160 m in
  ctor FUN_00A8F900), +0x5C bit0 (serialised), bit1 = no cross-fade; runtime table built by FUN_00A90C30 at
  res+0x60: +0x60 scale = 255/(lastFar*256/255), +0x64 256 x 2-bit LOD slot by quantised distance (entry i covers
  distance i/scale; descriptors in ascending far distance, so entry 0 = the most detailed slot), +0xA4 per class
  minimum slot (thresholds {FLT_MAX, FLT_MAX, 20, 10, 5, 2} at 0x16C2328), +0xA8 cross-fade target slot per slot,
  +0xAC + slot*8 {slope, offset} of the fade.
- lod = FUN_00A8F510(res+0x60, [X+0x2AC] * object+0xA8) (the only caller); if object+0xA4 & 7: lod = max(lod,
  class minimum); if !(instance+0x28) && !(res+0x5C & 2): blend = clamp(slope*d+offset, 0, 1) * 255.0 (double at
  0x1687DE0), fade target slot drawn in group 3 with fade = blend and a stencil id from sets+0x488.
- slot = [instance+0x14+lod*4]; if slot && [slot+8]: FUN_00A9F540([slot+8], object, id, [slot+0xE], 0,
  255-blend, stencil).

ForceMaxLod replaces the function: lod = FUN_00A8F510(res+0x60, 0.0) (the engine's own choice at distance 0), no
class clamp, no cross-fade, FUN_00A9F540(handle, object, id, passMask, 0, 0xFF, 0) — the same call the engine
makes on its no-blend branch. If that slot is empty the object draws nothing, exactly as the engine does at
distance 0 (LOD selectors whose first slot is empty are far-only proxies).

## 4. Far-LOD cells vs detailed cells

The masked terrain tiles draw cell k iff bit (first+k) of X+0x2B8 is set (kb.h). X+0x2B8 is written each frame by
FUN_005E43B0 (grid object at [[V+0x60]...+0x1C]): with DAT_01A25848 == 0 (default) it is the complement of the
grid's "processed/loaded cell" bitset grid+0x48 (cells are marked there when popped from the grid+0x18 load queue;
grid+0x64 per-cell fade-in byte), Morton-ordered cell ids. FUN_00AC6FC0 copies X+0x2B8 into Y+0x2C4, which culls
every render object of category 1 (objects with a grid cell, entity+0xAA != 0xFFFF) whose cell bit is set;
Y+0x2B8 (category 0, entity type 0xC) is cleared unless ViewSurface+0xDC == 0 (then all set), Y+0x2D0
(category 2, no cell) is always clear. ViewSurface+0xDD != 0 clears Y+0x2C4.

So the far-LOD tile is shown exactly where the detailed cell content is not (yet) loaded, and detailed objects of a
not-loaded cell are hidden. The detailed cells beyond the streaming radius do not exist in memory: they cannot be
"forced"; keeping them requires changing the grid loading distance (GridLoadingAdvisor /
RegionCellDataLoadingDistance, not analysed; memory-bound in a 32-bit process). The module leaves this swap alone
and the mod keeps mirroring the mask.

## 5. What anticull.cpp does

AntiCulling (all views):
1. `FUN_00AC6B90` detour (per view query): container totals -> grow X+0xA40 (visible objects, engine capacity
   0x2000) and X+0xA4C (lights, 0x80) with the engine reserve functions FUN_00A2F3F0 / FUN_00A2F490 when the whole
   container would not fit (the push callbacks do not check capacity); objects also gated by the render item id
   capacity 0x4000 (FUN_00ABCC90). If they fit: Y+0x2D8 / Y+0x2DC = FLT_MAX, and the frustum override is enabled for
   this query. Otherwise the view keeps engine culling (logged once).
2. `FUN_00AC5860` detour: after the engine's classification, an object/light the camera frustum rejected (result
   bit 2, test requested by clip bit 1, not rejected by the channel mask) gets object+0x20 |= visibleSet and result
   bit 1, i.e. the engine's in-frustum outcome for the camera pass. Its shadow verdict stays the engine's (the
   shadow-volume test already ran). Only for the two engine call sites (returns 0xAC5E04 objects, 0xAC61D4 lights).
3. `FUN_00A9F910` detour (render sets gather): need = sum of visible objects' component counts + always-drawn
   entries (upper bound of items per list); every engine-allocated render list (capacity != 0; FUN_00A9F740 sets
   0x10..0x1000) with less capacity is grown with the engine's FUN_00A9F690 before the gather. Growth doubles; lists
   the engine does not own (bit 31 clear) keep their old buffer (bounded leak).

ForceMaxLod: `FUN_00A90160` detour as in section 3.

## 6. Risks / to verify in game

- Performance: the whole loaded world goes through the engine's colour passes (D3D draws + Submit hooks) and
  small objects are no longer distance-culled. Expect several thousand draws per frame.
- Shadow passes are unchanged for really-outside objects; forced-LOD also applies to shadow lists (same LOD).
- Off-screen characters: their animation/AI update level comes from the entity LODLevel (entity in frustum ->
  level by distance, outside -> 0). Off-screen skinned meshes may keep a stale pose. Not patched (gameplay side).
- Lights: every registered light becomes visible -> Lights_Update relights with all of them and lights.cpp
  exports all of them.
- Buffers grown by the mod: logged as `anticull: ... grown a -> b`. A `kept engine culling` line means the
  container did not fit the id capacity 0x4000.
- Views: applied to every view that runs the visibility (the render object pass bit +0x20 is shared by views, so a
  per-view choice would let a later view clear it).
- Live checks to do: container totals (container+0x60 / +0x6C) vs capacities; Y+0x2D8/+0x2DC engine values in
  Masyaf; visible count X+0xA44 with the module on/off; screenshot looking at a wall / the sky with Remix
  reflections: buildings behind the camera must appear in reflections and cast shadows into view; distant
  buildings must show LOD0 detail (compare a rooftop at 150 m with ForceMaxLod on/off); no LOD popping when
  walking.
