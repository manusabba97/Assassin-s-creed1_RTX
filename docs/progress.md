# AC1 RTX — progress

Engine data only: every value below comes from the binary, the vertex shaders, a dx9 trace, a live read, or the
dxvk-remix source. Full evidence (addresses, decompilations) is in
`Vibe-Reverse-Engineering/patches/AssassinsCreed/kb.h`.

## Solved

### Toolchain and bridge (phases 0–1)
- Remix Plus fork built from `dxvk-remix` (runtime `_output/d3d9.dll`, bridge `bridge/_output`).
- 32-bit bridge client extended: `SetupCamera`, `CreateTexture`/`DestroyTexture`, `UpdateLightDefinition`,
  `CreateMesh` pNext loop fix. Presenter fix for a stale swap-chain image count.
- `tools/deploy_remix.ps1`, `tools/launch_to_level.ps1` (RETURN every 0.5 s; 80 s reaches Masyaf),
  `tools/screenshot.ps1`.

### Camera (phases 4–5)
- `Renderer_SetupView` 0xA1F100; view = `ctx+0xC0`, projection = `ctx+0x80`, main view = widest surface.
- Passed to `remixapi_SetupCamera` unchanged. Verified with the magenta marker (centred, correct size, occluded).

### Static geometry (phase 6) — solved 2026-09-30
- `DX9StaticMeshInstance<StaticPrimitive>::Submit` 0xAE03C0 and `<MaskedStaticPrimitive>::Submit` 0xAE0830
  (same instance layout: decl `+0x28`, `DX9StaticMesh` `+0x2C`, draw entries `+0x08/+0x0C`, world = placement `+0x40`).
- Vertex decode from the VS bytecode: pos = short4.xyz·|w·3.81481368e-6|, normal = (ubyte4.xyz−127)/127,
  uv = short2n·16. Buffers read through the bridge's client-side shadow copies (`Lock(D3DLOCK_READONLY)`).
- Masked meshes are the level ground/cliff tiles (256 m grid at z = 0). `DX9TerrainInstance` is not used in Masyaf.
- Per-range state read from the engine material (`FUN_00A4A580`):
  - cull: `material+0x10` bit 25 → NONE, bit 19 → CCW, else CW; `matInst+0xC` bit 0 / entry `+0xB` bit 7 force CW/CCW;
  - bit 26 → never drawn; depth write off (`bit 14`, or `bit 15` for blended; entry `+0xC` bits 0/1 force) → skipped.
- Facing in Remix: the engine camera is right-handed, Remix treats it as mirrored and flips facing
  (`rtx_instance_manager.cpp:59-81`). Rule: CW ranges → reversed indices, CCW → unchanged, NONE → `doubleSided`
  (separate Remix mesh per engine mesh, since `doubleSided` is per instance). Verified with
  `rtx.debugView.debugViewIdx = 3` (Is Front Hit) and culling off: all front hits.
- Sky: the background pass `FUN_00ABB440` (viewport depth 1.0, no depth write) draws the sky dome, a ~5 m
  hemisphere that follows the camera. Everything submitted during that pass is excluded from Remix geometry.

### Terrain far-LOD overlap — solved 2026-09-30
- Symptom: low-detail geometry/textures mixed with the detailed ones, like LOD0 and LOD1 together.
- Cause (measured): masked tiles are the far LOD of the terrain. The engine draws only the cells set in a per-frame
  bitmask (`FUN_00ADE5B0`), and that mask is distance-based: near cells (24–182 m, also in front of the camera) are
  off where detailed static meshes replace them, far cells (151–506 m) are on. The mod sent whole tiles.
  Static meshes showed no per-pass LOD swap (24 of 747 worlds with several meshes, all parts drawn in the colour pass).
- Fix: each masked mesh is split per engine cell (cell table from `FUN_00ADE5B0`, one Remix mesh per cell and
  sidedness); every frame only the cells whose bit is set are drawn.

### Engine occlusion culling off
- Under Remix the engine's occlusion queries return 0 samples, which culled almost everything from the colour pass
  (46 material draws per frame). The mod sets the engine's own switch `DX9ViewSurface+0x6D = 0` (≈630 per frame).

### Albedo textures and alpha test (phase 7, part 1) — solved 2026-09-30
- Read at draw time: `MaterialInstance_EndDraw` 0xAA3130 runs after each mesh draw; the device wrapper caches hold
  that draw's pixel shader (`+0x2998`) and textures (`+0x29D8 + stage*4`). The albedo stage is the PS CTAB sampler
  named `Diffuse*` / `diffusemap*` / `BaseTexture*`. Only colour passes (item `+0x14` = 0/1) are read.
- Alpha test from material `+0x10` (bit 24 enable, bits 6–13 ref, bit 5 LESS) → Remix `alphaTestType` 6/1.
- Pixels come from the bridge client's level shadow copies (`LockRect` read-only); DXT1→BC1-RGBA, DXT3→BC2 (added to
  the fork), DXT5→BC3, A8R8G8B8/X8R8G8B8/A8/L8/A8L8 → BGRA8 with the D3D9 swizzles. Texture hash = `0x…` path.
- One Remix material per (mesh, range), re-created with the same hash when its texture becomes known.

### Normal maps and texture lifetime — 2026-09-30
- Engine decode (PS bytecode): `N = (2r−1)T + (2g−1)B + (2b−1)N`, first `Normal*` CTAB sampler. Measured on static
  meshes: engine T follows dP/du (99.4 %), engine B is opposite to dP/dv (99.3 %), so green is flipped for Remix's
  UV-derived frame; then encoded as unsigned octahedral (packing.slangh) in BC5.
- Some surfaces are drawn twice per frame (normal-mapped PS + diffuse-only PS, same albedo): the richer state is kept,
  otherwise the material was re-created twice per frame.
- Textures are released when every remaining reference is the mod's (bridge client Release returns the interface
  refcount, base.h:425-445): Remix texture destroyed, pin dropped.

### Skinned characters (phase 8) — solved 2026-09-30
- Hook `FUN_00AE11A0` (bone palette builder, called per skinned draw entry before its draw). Colour passes only
  (render item `+0x14` = 0/1, draw list 0), deduped per frame on (instance, placement, range).
- Bones: the engine's final matrices from the device wrapper shadow `+0x790 + i*64 + k*16` (= c120+), already
  the rows of `remixapi_Transform`; world matrix is identity (0x01A0F000).
- Vertex decode (skinned VS bytecode): SHORT4N pos ×16, UBYTE4 N @8, SHORT2N uv @20 ×16, UBYTE4 indices @24,
  UBYTE4N weights @28 divided by their sum (the VS divides by the summed weight; many sums are below 255).
- One Remix mesh per (mesh, range, MaterialInstance) with 4 bones/vertex (see below); per frame `DrawInstance` +
  `InstanceInfoBoneTransformsEXT`. Materials come from the same `MaterialInstance_EndDraw` hook via the current
  skinned entry.
- Bridge fix: CreateMesh skinning serialization multiplied the total weight/index counts by bonesPerVertex and by
  the vertex count again (`util_remixapi.cpp` sizes); now `count * sizeof(element)` as the runtime expects.
- Details: `docs/notes/skinned.md`.

### Skinned materials per MaterialInstance — solved 2026-09-30
- Symptom: characters with wrong/flickering textures; ~174k Remix materials re-created in 5 minutes.
- Cause (measured live, livetools trace of the Submit functions, `scripts/mesh_matinst_census.py`): characters that
  share a skinned mesh draw the same range with different MaterialInstances (57 of 171 skinned (mesh, range) pairs,
  45 meshes in Masyaf). The mod kept one material per (mesh, range), so it alternated between texture sets every
  draw. Static meshes: 0 such cases (392 pairs).
- Fix: Remix has no per-instance material override (the material is bound to the mesh surface), so skinned ranges get
  one Remix mesh + material per (mesh, range, MaterialInstance); the decoded geometry is kept and reused for each new
  MaterialInstance. Static materials stay per (mesh, range). Result: ~1.6k materials per session.

### Skinned inner faces (hood, robes) — solved 2026-09-30
- Symptom: inside of the protagonist's hood (and other skinned meshes) see-through, as with the early world facing bug.
- Cause (dx9 trace frame 0, `scripts/skinned_cull.py`): the colour pass draws some skinned ranges twice through two
  draw entries: D3DCULL_CW with the outer material and D3DCULL_CCW with an inner one (different PS, VS and s0/s1
  textures; e.g. seq 17584/17596, same 184 vertices / 242 triangles). Pass 2: NONE 116, CW 250, CCW 33 draws. The
  mod deduplicated skinned draws per (instance, placement, range) and dropped the CCW entry.
- Fix: one draw per entry (dedupe per (instance, placement, range, MaterialInstance)); geometry decoded per
  (range, engine cull) with the same facing rule as the world (CW reversed, CCW unchanged, NONE double-sided); Remix
  mesh per (mesh, range, cull, MaterialInstance). Mod counts after the fix: NONE 26 %, CW 66 %, CCW 9 % of skinned
  draws (engine trace 29 / 63 / 8 %).

### Decals and blended surfaces — decals confirmed 2026-09-30
- Ranges drawn without depth write were skipped. Engine data: blend mode m = material flags & 7 -> FUN_00A21350
  SRCBLEND [0x1947AD8 + m*4] / DESTBLEND [0x1947AC0 + m*4] (1 ONE/ONE, 2 ONE/INVSRCCOLOR, 3 DESTCOLOR/ZERO,
  4 SRCALPHA/INVSRCALPHA, 5 DESTCOLOR/ONE, 6-7 ZERO/ONE). Draw entry +0xA = render-list mask; list 1 is the decal pass
  FUN_00ABEA60 (ZWRITE 0, DEPTHBIAS [[ctx+0x40]+0x4C]); lists 2/3 are the blended passes.
- Static vertex colour (+16, D3DCOLOR) is now decoded: the blended PS multiply albedo by its rgb and opacity by its
  alpha (alpha constants = 1 in every Masyaf draw).
- Remix: modes 4 -> BlendType kAlpha, 3 -> kMultiplicative (the types Remix assigns to the same D3D factors); texture
  stage ops Texture x VertexColor0 per instance (InstanceInfoBlendEXT); list-1 entries get DECAL_STATIC. Mode 5 (no
  Remix equivalent) is logged and not sent.

### Cloth (robes, awnings) — solved 2026-09-30
- Drawn by scimitar::DX9DynamicSubMeshInstance (vtable 0x016C2DF4, Submit 0xAA38A0; 28 draws in frame_capture_01),
  not by the static/masked/skinned Submits. CPU-vertex path (instance +0x30 == 0): the draw FUN_00AA4780
  (thiscall on R = [instance+0x1C]) copies the simulation's CPU vertex array [R+0x3C] ([R+0x40] bytes) into a ring VB
  (FUN_00A0CAD0) and draws [R+0x30] vertices, stride [R+0x2C] = 32: FLOAT3 pos, FLOAT3 normal, FLOAT2 uv; IB wrapper
  [R+0x10+[R+0x20]*4]; world = item+0x100 (0xAA3ECC). 12-byte draw entries (+0 MaterialInstance, +0xB bit0 winding,
  bit1/2 depth write on/off).
- Mod: after each colour-pass cloth draw the CPU vertices and the IB are read, finished at MaterialInstance_EndDraw
  with the entry's cull (same rules as the world); one Remix mesh per draw per frame (previous frame's destroyed).

### Layered terrain (cobblestone / gravel blends) — solved 2026-10-01
- PS 7a87be5e family: albedo = lerp(lerp(tex(s3)*c128, tex(s4)*c129, COLOR1.x), tex(s5)*c130, COLOR1.y) * COLOR0.rgb,
  normal = lerp(lerp(s0, s1, COLOR1.x), s2, COLOR1.y); COLOR1 = (NORMAL.w, TANGENT.w) bytes / 255.
- Shader identity = FNV-1a 64 of the little-endian bytecode (the trace prints DWORD tokens: first hash was wrong).
  Masyaf uses 14 variants with the same formula (the mod dumps PS outside the census to `tests/shaders`;
  `scripts/specular/disasm_dump.py` + `verify_layered.py` compare the symbolic albedo/normal formulas); all share the
  samplers, 36A729ED reads the tints from c131..c133 (its CTAB) instead of c128..c130 (`game.h kLayeredTerrainPs`).
- Remix: base range opaque; layers 1/2 as coplanar DECAL_STATIC overlays with the layer's albedo/normal, tint via
  tFactor, opacity = vertex weight (alpha SelectArg2 VertexColor0) - composes to the PS's nested lerp.

### Material churn from specular — fixed 2026-10-01
- Skinned surfaces are also drawn by a rim-light PS outside the census (447118CA, pow(|dot(V,N)|, RimPower)), alternating
  every frame with the census PS: the specular fields flipped and the material was re-created twice per frame (64k
  materials per session). PS outside the census now keep the specular read from the census PS (3.4k materials).

### NPC skin (grey arms) — solved 2026-10-01
- Skin PS b1b370c7 / 74673c36 / FC750BAF name their samplers only `OperatorN_k`, so the name lookup found neither albedo
  nor normal (material created with the 0.5 grey constant). Their disassembly: normal map s0; diffuse =
  lerp(tex(s1), c128, tex(s2).x) * saturate(tex(s3) + c129) (c128 = per-NPC tint); s4 N.L wrap ramp, s5 specular.
- The mod composes that diffuse on the CPU per (base, mask, modulate, c128, c129) at every mip of s1 (`game.h kSkinPs`,
  `remix.cpp ensureSkinTexture`); rim-light draws of the same surface keep the skin data.

### Glass (blend mode 5, Animus room) — solved 2026-10-01
- Mode 5 = DESTCOLOR/ONE (dst * (1 + src)): the background is never attenuated, the surface only adds light. All 7
  PS (game.h kGlassPs) share one graph: lit tex(s1)*DiffuseColor*COLOR0 + Phong * spec map * cubemap, times
  lerp(Fresnel_17, 1, (1 - N.V)^5) where Fresnel_17 (c133, measured 5/15/20) is a specular intensity, not a reflectance.
- Remix: thin-walled translucent, transmittance 1, normal map s0, IOR = Remix's own translucent default 1.3 (the game
  has no IOR). The blended static ranges are now per-range parts so their PS state is known at draw.

### Blended-surface tints — 2026-10-01
- PS families from the dumps (game.h kBlendTintPs): rgb = tex * DiffColor * COLOR0 (alpha tex.a * Alphablend * COLOR0.a),
  one variant with max(1, DiffuseUp), two without vertex colour (tex * c128, alpha tex.a). Sent as tFactor
  (isTextureFactorBlend) / Modulate(Texture, TFactor); alpha factor exact when the part's vertex alpha is all 255.

### HUD — solved 2026-10-01
- The engine draws its whole 2D layer (HUD, menus, subtitles) from the overlay dispatcher FUN_009ED5F0, called only
  from EndFrame's FUN_00A1F5A0 after every 3D view (frame_capture_01: all 181 HUD draws return through 0x9ED656, no
  3D draw after the first). All use a vertex shader, so Remix ignored them (vertex-shader check before UI detection).
- Mod (hud.cpp): hooks the dispatcher; there it submits the frame (camera, world, lights) and calls the new API
  `InjectRTXAtNextDraw`. Remix fork: option `rtx.enableApiInjectionMarker` (game rtx.conf = True) makes the next
  D3D9 draw the injection point; that draw and the rest of the frame are rasterized over the ray-traced image.
  Bridge forwards the call in the ordered command queue. Notes: docs/notes/hud.md.

### Game colour grading (LUT) over Remix — approved 2026-10-01
- AC1's look is a 16x16x16 A8R8G8B8 3D LUT: PS 0x14C9BA80 oC0 = tex3D(s1, tex(s0) * g_PreLutScale + g_PreLutOffset),
  run by FUN_00AAF540 (called from 0xAC02E1): copy scene -> A, LUT A -> B, then FUN_00A1F4A0 copies B to the back buffer.
- Mod hooks FUN_00AAF540 (return 0xAC02E6) and calls InjectRTXAtNextDraw(skipDraw = true): the scene copy is dropped,
  Remix writes into A, the game's LUT and final copy run on it, then the HUD. Key I toggles (off = inject at the HUD).
- Bridge fix: Direct3DVolume9 LockBox returned RowPitch/SlicePitch in pixels/rows instead of bytes, so the game wrote the
  LUT with overlapping rows (scene came out blue); now bytes, wire format unchanged.

### Water — approved 2026-10-01
- PS AAC5D7B1 (game.h kWaterPs): rgb = cubemap reflection * Color_9 * CubeMapIntensity, alpha = Opacity_20 * COLOR0.a *
  max(LODBlendFactor, LODBlendToggle); normal = two normal maps with uv = Panner01/02 (c128..c132) x (world.x, world.y,
  1), world from its VS 3F07D8C7 (texcoord1 = pos x g_World). The mesh has no texcoord of its own.
- Remix: thin-walled translucent (like the glass), transmittance Color_9, normal map s0; every frame the water parts
  are re-meshed with uv = Panner01 x world position (the game scrolls Panner01 per frame; kept out of the material
  state). Only one normal map (Remix has one); NormalMap02's scroll is not reproduced.

### Animus menu — approved 2026-10-01
- Menu frames: a 2048-wide view (with its own overlay pass) renders the band into a 2048x512 render target, then the
  3840 scene view draws the lab and, last of its transparent draws, the band mesh (7E9C1D10 range 3, list 2) textured
  with that target. Sequence: `V2048 O V3840 ... RT2 P P O`.
- Injection is gated on the main view (widest view of the previous frame, `camera::mainViewSeen`), so the 2048 view's
  overlay no longer injects early.
- MaterialInstance_BeginDraw hook (0xA4B1B0) marks the injection before the band draw; the RT-textured surface is not
  submitted through the API, so it is rasterized over the path-traced lab (black rim kept).
- Fork API `RasterizeOffscreenDraws(enable)`: draws into non-back-buffer-sized targets are rasterized even with a
  programmable VS (they were ignored, leaving the band's target empty). Enabled for secondary views in SetupView.

### Clutter / instanced vegetation — solved 2026-10-02
- Drawn by DX9DynamicSubMeshInstance::Submit (0xAA38A0) on its instanced path (instance +0x30 = instance count N,
  +0x48 = N 4x4 matrices), not by the static Submits. Types 5/6: g_World = identity, batches of 32 instances whose
  matrices are uploaded raw to c120 `g_ClutterWorldMatrices` (FUN_00AA93C0, no transpose); the VB holds 32 copies of
  the mesh with the copy index in TEXCOORD3 UBYTE4 .x (VS 0x215A24C0, decl stride 40: pos@0 normal@12 color@24
  copy@28 uv@32). Other types draw once per instance with g_World = matrix i (transposed like every world).
- Mod (dynamic_mesh.cpp): on the first draw of an entry, keeps copy 0 of the mesh, hashes it, and records the N
  transforms; remix.cpp keeps one Remix mesh per (geometry hash, MaterialInstance, sidedness) and draws it once per
  transform each frame. Masyaf: one grass mesh (32 vertices, 12 triangles), up to ~1.8k instances per frame.
- The 128-instance terrain-clutter batches (type 5 with a terrain, 16-byte records) are skipped with a log line; not
  present in Masyaf.

### PBR material editor (key M) — confirmed 2026-10-03
- Runtime fork `rtx_fork_material_editor.cpp`, files shared with the mod in `<pbr>\maps\<hash>\<variant>\`:
  `<hash>_edit.json` (live edit: per-map on/off, brightness, contrast, blur, invert; albedo saturation and tint;
  normal strength; parallax on/off, Sporgenza = displaceOut, Rientro = displaceIn, in cm), `<hash>_bake.txt`
  (permanent height / normal inversion) and `<hash>_save.json` ("Salva nelle texture": the adjustments are written
  into the PNG and the .ac1t, then reset). The mod polls them every 30 frames and rebuilds the maps on a worker thread
  (pbr_edit.cpp).
- Picking: hovering a PBR material with nothing selected fills its surface (post FX highlight `fillHighlighted`);
  click selects (outline only), N deselects.
- Vista: Normale / Bianco (`rtx.useWhiteMaterialMode`) / Normal (debug view 16) / Altezza (debug view 812).
- "Mostra texture non convertite": magenta fill on every surface that can take PBR maps but has none
  (live_textures.txt "U" lines from the mod).

### Materials realism pass, fog, decals — solved 2026-10-03
- PBR: skinned and dynamic meshes (characters, simulated cloth, clutter) keep the engine materials. tools/pbr_pack.py
  tune(): VLM material name -> class (roughness range, relief per texture repeat, POM default, metallic 0.7 for metal);
  POM depth in texture units capped at 4 % of a repeat, off on alpha-tested and non-tiling (uv span <= 1) surfaces.
  Alpha albedos: colour bled under transparent texels + alpha-weighted mips (the workflow returned black there:
  dark fringes / "flakes"). Metres per uv also measured on skinned and dynamic meshes.
- Baked shadow decals (black wherever visible) and faint grime overlays (dark, unsaturated, alpha <= 35 %) are hidden
  (alphaTestType Never): path tracing already shadows/occludes. Procedural smoke PS 3BC141C8 hidden.
- Skin PS: 9 more variants verified by disassembly (same diffuse formula); untextured PS EEF09AA5 uses c128 as albedo
  constant; materials created without albedo are logged with their PS.
- Game lights (not the sun) RadianceScale 0.3. Horizon fog: fork composite applyHorizonFog (rtx.horizonFog.*),
  exponential with height falloff, coloured by the aerial perspective volume's far slice; aerial perspective scene
  shadow off (blotches on distant walls). DLSS SR / FG DLLs 310.8 (NVIDIA-signed).

## Open
Reported 2026-10-01, to be handled later:
- Some fake lights are still exported (the light-channel filter does not catch all of them).
- Numos settings should change per level; they currently stay the same everywhere.
- Particles are not implemented.
- Overall performance is insufficient.

Other:
- Off-screen characters (not in the colour pass) are not submitted.
- Terrain clutter (128-instance batches) for Kingdom levels.
- Pixel shaders outside the specular census keep roughness 0.7.
- Fog (off in Masyaf), foliage alpha.
- `DX9TerrainInstance` (0xAB4B70) for Kingdom levels.

## Next
- PBR materials and parallax occlusion mapping.
