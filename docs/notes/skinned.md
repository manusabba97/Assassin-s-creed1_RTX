# Phase 8 - skinned characters (design notes, 2026-09-30)

Status: static analysis only (decompilation + `frame_capture_01.jsonl` + dxvk-remix source). Nothing here has
been run in the mod yet. Items marked **UNPROVEN** each come with the live read that would prove them.
Read spec: `addr  [reg+off]:SIZE:type` (`*[reg+off]` = read the pointer, then read bytes at it).

Scratch: `Vibe-Reverse-Engineering\patches\AssassinsCreed\scripts\skinned_trace.py` (skinned-draw census),
`dumps\skinned\skinned_draws.json` + `vs_*.txt` (full VS disassembly of all 33 skinned VS),
`scripts\_sk_ae11a0.asm` (palette builder disassembly).

## 1. Engine path (AssassinsCreed_Dx9.exe v1.02)

### 1.1 `SkinnedMeshInstance_Submit` 0xAE1810
`__thiscall(self = DX9StaticMeshInstance<SkinnedPrimitive>*, ctx, a2, placement)`, `ret 0xC`
(disasm 0xAE1A37). Prologue `83 EC 10 53 8B 5C 24 18 8B 43 1C 57`. Only caller is the dispatcher at 0xABB3F2
(vtable 0x016D66BC slot 1). Its arguments come from RenderBatch_DrawItems 0xABB0E0: `ctx` = dispatcher arg1
(esi), `a2` = dispatcher `this`, `placement` = `[listEntry+0]` (list entries are 0x14 bytes; +0xB = stencil ref,
+0xA = fade byte, divided by 255 into ctx+0x64 at 0xABB211-0xABB223).

`ctx` (the "render item", same object as the RenderMatrixContext: SetWorld is called on it):
+0x04 DX9Device wrapper, +0x14 pass type, +0x1C pass mask (= `1 << arg4`, `|0x40` when arg5 == 2, 0xABB146-0xABB15C).

Instance layout (read in 0xAE1810):

| off | use |
|---|---|
| +0x04 | u16 pass mask; `if (!(ctx+0x1C & mask)) return` (0xAE181E) |
| +0x08 / +0x0C | draw list 0 `{entries*, count & 0x3FFF}` |
| +0x10 / +0x14 | draw list 1 (same format) |
| +0x18 | u16 `[passType]` shader-constant flags for FUN_00AC4F70 |
| +0x24 | stream-1 VB wrapper (IDirect3DVertexBuffer9* at +8), per-vertex AO, SetStreamSource(1, ..) (0xAE189A-0xAE18E3) |
| +0x28 | IDirect3DVertexDeclaration9* |
| +0x2C | mesh (FUN_00ADFCA0 binds stream 0 + indices from it) |
| +0x34 | bits 0-30 stream-1 stride; bit 31: pass types 4/5 also use list 1 |
| +0x38 | pointer to the bone-palette record array (0x104 bytes per record) |

List selection (0xAE182E-0xAE1855): `L = 1` if `passType == 2`, or `(self+0x34 bit31) && passType in {4,5}`;
else `L = 0`. **Colour passes (0/1) always use list 0.**

Draw entry, **0x14 bytes** (static entries are 0x10): +0x0 MaterialInstance*, +0x4+passType record byte
(passed to BeginDraw/EndDraw), +0xA pass-mask byte (`& (u8)ctx+0x1C`), +0xB bits 0-6 range index / bit 7 forced
winding, +0xC bit0/bit1 depth-write force, **+0x10 u8 palette record index**.

Per entry (0xAE1980-0xAE1A2A):
1. `MaterialInstance_BeginDraw(entry+0, ctx, record, entry+0xB>>7, entry+0xC&1, entry+0xC>>1&1)`; skip if 0.
2. `FUN_00AE11A0(ecx = entry, ctx, placement, self+0x38)`, which builds and uploads the bone palette (1.2).
3. `DrawIndexedPrimitive(type, 0, minVertex, numVertices, startIndex, primCount)` from the range
   `[[self+0x2C] + 4 + L*8] + (entry+0xB & 0x7F) * 0x14` (same PrimitiveRange struct as static meshes).
4. `MaterialInstance_EndDraw(entry+0, record, ctx+4 device, ctx)` at 0xAE1A1E. **The mod's existing EndDraw hook
   already sees every skinned draw.**

Before the loop: `RenderMatrixContext_SetWorld(ctx, 0x01A0F000)` (0xAE18FF) and g_WorldViewProj upload (param 0x74).
0x01A0F000 is BSS, written only by the static initializer 0x15DD400 with rows (1,0,0,0) (0,1,0,0) (0,0,1,0)
(0,0,0,1) ([0x1680F90] = 1.0f): **the skinned world matrix is identity**, so the bone palette maps model space
straight to world space. Trace check: g_World c8-c11 = identity on all 399 colour-pass skinned draws.

### 1.2 Bone palette: `FUN_00AE11A0` (ret 0xC; prologue `55 8B EC 83 E4 F0 B8 E4 10 00 00 E8`; only caller 0xAE19CD)
- Palette record `R = [self+0x38] + entry[+0x10] * 0x104`: `+0x00 u32 count` (<= 32),
  `+0x04 Mat4* src[32]`, `+0x84 u32 poseIndex[32]`.
- `P = [placement+0x9C]` (pose source), `s = float [placement+0xAC]` (scale), `S = diag(s,s,s,1)` built by
  FUN_005B47B0.
- For each slot i with `src[i] != 0`, A_i is built from P and `poseIndex[i]`, by mode `[P+0x58]`:
  - 0: element `[P+0x60] + idx*0x20`: +0x00 float3 translation, +0x10 quaternion (x,y,z,w) (FUN_004E2A70 ->
    FUN_00428540, a row-vector rotation matrix);
  - 2: element `[P+0x60] + idx*0x10`: +0x0 short3 translation * 1/2048 ([0x1688860] = 0x3A000000) plus the float4 at
    `[P+0]`, +0x8 packed quaternion (FUN_004E0320) (FUN_00AE0E90);
  - 1 (and any other value when s == 1): FUN_004F6C90(P, idx), i.e. getters FUN_004F61E0 (rotation) and
    FUN_004F60A0 (translation).
- Final matrix (Mat4_Multiply 0x653400: `out = b * a`, row-vector):
  - `|s-1| <= 0.0005` ([0x1683308]): `B_i = src_i * A_i`;
  - otherwise: `B_i = (src_i * S) * A_i`;
  - `src[i] == 0`: `B_i = S`; `P == 0`: every `B_i = S`.
  So the vertex transform is `p_world = p_model * src_i * S * A_i`. That `src_i` is the inverse bind matrix and
  A_i the animated bone world transform is **UNPROVEN** (naming only; the mod does not need it).
- Upload: `B_i` is transposed into the device-wrapper constant shadow (`reg r` lives at `wrapper + (r+1)*16`):
  `wrapper+0x790 + i*64 + k*16` = column k of B_i = register `c(120 + 4i + k)`. Then
  `SetVertexShaderConstantF(120, wrapper+0x790, count*4)` at 0xAE1727 (call returns to 0xAE172F);
  wrapper+0x2A70 = 120, wrapper+0x2A74 = count*4 (reset to 0 after the call).
- **Palette slot = BLENDINDICES byte.** Palettes are per draw entry (per range), not per mesh.

Trace (frame 0, colour pass 2, 399 skinned draws): each draw is directly preceded by its c120 upload, with first
engine return address 0xAE172F. Bones per draw 1..32 (median 6, max 32). Bone translations lie 2.7 to 221 m from
g_EyePosition (world space). Column 3 = (0,0,0,1) and det(3x3) = +1.000 (or 0.941 = 0.98^3 on scaled
characters: no mirroring).

| seq | VS | range | c120 upload | bone 0 T (world) |
|---|---|---|---|---|
| 17596 | 0x202C1840 | start 0, 184 v / 242 t | seq 17595, 48 regs (12 bones) | (-36.209, -32.519, 35.361), det 1.0000 |
| 21355 | 0x20E79940 | start 0, 166 v / 250 t | seq 21354, 8 regs (2 bones) | (-21.939, -52.210, 36.186), row length 0.98 (s = 0.98) |

The eye is at (-39.59, -31.89, 35.42) in both.

### 1.3 Vertex format (trace CreateVertexDeclaration, most recent create before the draw)
Decl 0x21EA5F00 (343 colour draws) / 0x21EA3C40 (56), stride 32 on stream 0:

| off | type | usage | decode (skinned VS bytecode) |
|---|---|---|---|
| 0 | SHORT4N | POSITION | `p = (x,y,z)/32767 * 16, w = 1` (`mad r0, v0.xyzx, (16,16,16,0), (0,0,0,1)`). **Not the static decode** (no w*3.81e-6 scale) |
| 8 | UBYTE4 | NORMAL | `(b-127) * 0.00787401572`, skinned with 3x3, `nrm` |
| 12 | UBYTE4 | TANGENT | same decode |
| 16 | UBYTE4 | BINORMAL | same decode, stored explicitly (static meshes use sign*cross(N,T)) |
| 20 | SHORT2N | TEXCOORD0 | `uv * 16` |
| 24 | UBYTE4 | BLENDINDICES | slot = byte (`mul r, 4, v2; mova a0`) |
| 28 | UBYTE4N | BLENDWEIGHT | 4 weights byte/255, all four used, **not renormalised** in the VS |
| s1:0 | D3DCOLOR | TEXCOORD4 | decl 0x21EA3C40 only: AO, `o = sat(max(v7.x*c81.x + c81.y, c81.z))` (g_AOParams) |

- Skinning in every bone VS (all 33, e.g. vs_0x202C1840): `r = sum_i w_i * (p . B_idx_i)` (dp4 on
  c120+4idx .. c123+4idx), then `* g_World (c8, identity) * g_WorldViewProj`. The VS world position output is
  `r.xyz / r.w` (`rcp r0.x, r1.w; mul r0, r1, r0.x`), and `r.w = sum w_i`. So the effective position is
  `sum w_i p B_i / sum w_i`: **normalising the weights by their sum reproduces the engine exactly** (positions and
  normals; the normal is re-normalised anyway).
- Vertex colour (VS 0x1CFF9840, 0x21EDB8A0, 0x20E79940 ...): `o3 = (N.w, T.w, B.w)/255, pos.w * 0.99999994`.
- 2 draws in pass 1 use decl 0x21EA42E0 (FLOAT3 pos, FLOAT3 normal, FLOAT2 uv) with a non-bone VS. They are not
  in the colour pass, so they are ignored.
- **VB is static**: stream 0 comes from `[mesh+0x14]+8`, stride `[mesh+0x1C] & 0x7FF` (= 32), IB `[mesh+0x18]+8`
  (FUN_00ADFCA0). Skinning happens on the GPU from the palette, and the trace has no per-frame VB writes: all 6237
  non-ring VBs are `Usage = WRITEONLY, Pool = MANAGED` (created via 0xAAA56E -> 0xAA9392); only 13 DYNAMIC buffers
  exist, from ring-buffer paths. The mod's existing `Lock(READONLY)` decode therefore applies.
  **UNPROVEN** (the trace has no VB handle for its create records):
  - that `[self+0x2C]` is a DX9StaticMesh: `0xAE1945 [ecx]:4:ptr` should read 0x016D6614; the only other
    DX9GraphicObject subclass is DX9DynamicSubMesh;
  - the VB's Usage/Pool: log GetDesc at first decode;
  - the weight-byte sums: log a histogram of `b28+b29+b30+b31` at first decode (if every sum is 255, no
    normalisation is needed).

## 2. Remix side (C:\Users\manu\Desktop\MOD\dxvk-remix)

API: `remixapi_MeshInfoSurfaceTriangles.skinning_hasvalue/skinning_value` = `remixapi_MeshInfoSkinning
{bonesPerVertex, blendWeights_values/count, blendIndices_values/count}` (remix_c.h:342-362; counts are totals,
`bonesPerVertex * vertexCount`). Per-instance `remixapi_InstanceInfoBoneTransformsEXT {boneTransforms_values,
count}` (sType 14) is chained on `remixapi_InstanceInfo.pNext` (remix_c.h:438-445), with at most 256 bones.

Runtime: supported.
- rtx_remix_api.cpp:1164-1193 builds the weight/index buffers (indices packed to bytes), and 1207-1210 sets
  `numBonesPerVertex` and the blend buffers on the RasterGeometry.
- rtx_remix_api.cpp:999-1011 reads BoneTransformsEXT into `skinningData` (`tomat4`, :301, same 3x4 convention
  as the instance transform) and hashes it.
- rtx_scene_manager.cpp:352-361: a change of `boneHash` gives `kUpdateBVH`; 1387-1392 calls `dispatchSkinning`
  when `numBones > 0 && numBonesPerVertex > 0`.
- rtx_draw_call_cache.cpp:70: instances of the same mesh with different bones in one frame get separate BLAS
  entries.
- Shader skinning.h:70-131: `lastWeight = 1 - sum(first n-1 weights)`; bones multiply `(p,1)` and `(n,0)`; the
  normal is re-normalised. Tangents are not skinned; HardcodedVertex has no tangent anyway.
- Minor: rtx_remix_api.cpp:1005 copies `numBonesPerVertex` from the prototype geometry before it is assigned
  (always 0). Only the capturer reads it (rtx_game_capturer.cpp:587). No render impact.

32-bit bridge:
- DrawInstance forwards BoneTransformsEXT: client remix_api.cpp:233-238, server main.cpp:3089-3095,
  serialisation util_remixapi.cpp:682-704 (sType + count + 48-byte transforms, symmetric). OK.
- **BUG, CreateMesh skinning serialisation** (util_remixapi.cpp:438-443):
  `blendWeightSizePerVtx = blendWeights_count * bonesPerVertex * 4` and `blendIndicesSizePerVtx` likewise.
  The callers multiply this again by `vertices_count` (sizeOf :455/:457, serialize :490/:494, deserialize
  :523/:527). With the header's total counts that sends `V * (4V) * 4 * 4` bytes: it reads past the client arrays
  and overflows the channel. The runtime (rtx_remix_api.cpp:1166) expects `count` elements in total.
  **Fix:** the byte size is `blendWeights_count * sizeof(float)` and `blendIndices_count * sizeof(uint32_t)`
  (drop both `vertices_count *` factors and the `* bonesPerVertex`) at those six call sites. That is the only
  bridge change needed; the `_dtor` at :538-541 is already correct.

## 3. CPU vs Remix skinning (colour pass, frame_capture_01 frame 0)
399 skinned draws = 89,396 vertices / 105,124 triangles; 366 unique (VB, range, pose) and 152 distinct ranges.

- **CPU skinning**: every frame, 399 CreateMesh + DestroyMesh. That is about 89.4k * 64 B + 105k * 12 B, roughly
  7 MB per frame, serialised vertex by vertex through the bridge (util_remixapi.cpp:477-479; client channel 96 MB,
  global_options.h). Each CreateMesh gets fresh geometry hashes (`hack_getNextGeomHash`,
  rtx_remix_api.cpp:1217-1220), so every surface needs a new BLAS build (KBuildBVH, rtx_scene_manager.cpp:345)
  and a new replacement instance per frame.
- **Remix skinning**: CreateMesh once per (mesh, range): about 5.7 MB one time for the 152 ranges. Per frame,
  about 366 DrawInstance + BoneTransformsEXT (at most 32 * 48 B = 1.5 KB each, typically about 300 B), and a
  BLAS update plus compute skinning inside Remix, which is the path Remix already uses for D3D9 skinned draws.

**Recommendation: Remix skinning**, after the bridge fix in section 2. The runtime path is in the source but has
not been exercised by this mod yet. First test: one character, `rtx.debugView` normals.

## 4. Implementation plan

1. **Hook FUN_00AE11A0** (`__thiscall(entry, ctx, placement, palettes = self+0x38)`, ret 0xC). Call the original,
   then:
   - skip unless `ctx+0x14 in {0,1}` (colour; list 0);
   - `self = palettes - 0x38`, `mesh = [self+0x2C]`, `range = entry[0xB] & 0x7F`, `matInst = [entry]`;
   - `R = [palettes] + entry[0x10] * 0x104`, `n = [R]` (1..32);
   - `wrapper = [ctx+4]`; for bone i and row k in 0..2: `remixapi_Transform[i].matrix[k][0..3] =
     float4 at wrapper + 0x790 + i*64 + k*16`. That register is column k of the row-vector B_i, which is exactly
     row k of the column-vector 3x4 (the same transpose the mod's `toRemixTransform` does).
   - Dedupe per frame on `(self, placement, range)`: 33 of 366 colour ranges are drawn twice with different VS.
     The second draw carries the same pose.
   - Store `{mesh, range, matInst, bones[n]}` in the frame list, and set a thread-local "current skinned entry"
     for the EndDraw hook.
   An alternative is to rebuild B_i from section 1.2 (src_i, S, A_i). It is not needed: the shadow holds the
   engine's final values.
2. **Mesh decode** once per (DX9StaticMesh, range) with list-0 ranges (`[mesh+4]`, all colour ranges are
   TRIANGLELIST):
   - read the vertex window `[minVertex, minVertex + numVertices)` with the existing lockReadOnly, stride 32;
   - decode per 1.3: pos `s16/32767*16`; N `(b-127)/127`; uv `s16n*16`; `color = 0xFFFFFFFF`;
   - weights `w_i = b_i / sum(b)` (exact, see 1.3); indices = the 4 bytes at +24; bonesPerVertex = 4;
   - cull/winding per range exactly as static (readRangeCulls: entry+0xB bit7, MaterialInstance+0xC,
     Material+0x10). Note the entry stride is 0x14 and the entry list is `self+0x08`;
   - Remix mesh hash = `(mesh, range, sidedness)`. Destroy it in the existing DX9StaticMesh destructor hook
     (only if 0xAE1945 `[ecx]:4:ptr` = 0x016D6614).
3. **Submit** in onPresent, next to the static geometry: for each recorded draw, DrawInstance(mesh(range),
   transform = identity (world = 0x01A0F000), pNext = BoneTransformsEXT{bones, n}).
   Optional, to give Remix a meaningful instance position: `T` = the bone-0 translation, the instance transform
   = T, bones' = `B_i * T^-1`. The result is algebraically the same.
4. **Materials**: EndDraw 0xAA3130 is already called per skinned draw (0xAE1A1E). Change `findRange` to use the
   thread-local skinned entry (0x14 stride) instead of the 0x10-stride static list, keyed by (mesh, range).
5. **Passes/LOD**: take only pass types 0/1 (ctx+0x14). Types 2/4/5 (shadow cascades / prepass / shadow) use
   list 1 and are skipped, so there are no duplicates. LOD is inherited: the mod mirrors exactly the instances the
   engine drew this frame.
   Limitation: characters outside the colour pass (off-screen) are not submitted. Phase 2 could take them from
   pass type 2 by rebuilding list-0 palettes from 1.2.
6. **Bridge**: apply the util_remixapi.cpp fix in section 2 and rebuild the bridge (both the 32-bit client and
   the 64-bit server).

## 5. Live reads still to take

- Pass types and list: `0xAE1831 [ebx+0x14]:4:u32`, `[ebx+0x1C]:4:u32`, `[edi+0x34]:4:u32`.
- Duplicate entries: `0xAE19C1 [esi+0xB]:1:u8`, `[esi+0x10]:1:u8`, `[edi+0x2C]:4:ptr`, `[esp+0x2C]:4:ptr`,
  counted per frame.
- Placement / pose source:
  - `0xAE11D9 [eax]:4:ptr` (placement vtable -> RTTI), `[eax+0x9C]:4:ptr`, `[eax+0xAC]:4:float32`,
    `[eax+0x40]:64:float32` (is +0x40 the character world matrix, as for static meshes?);
  - `0xAE11F2 [esi+0x58]:4:u32` (pose mode);
  - `*[esi+0x60]:32:float32` (first pose element).
- Record: `0xAE11CA [ebx]:4:u32` (count), `*[ebx+0x4]:64:float32` (src_0).
- Final palette as uploaded: `0xAE1724 [edi]:192:float32` (bones 0-2, transposed) and `[ecx]` = 120.
- Mesh class: `0xAE1945 [ecx]:4:ptr` (expect 0x016D6614).
