# Phase 9 — lights, ambient, sky (design notes)

Status: static analysis only (binary, decompilation, dx9 trace `frame_capture_01.jsonl`, Remix source).
Nothing here has been confirmed live yet. Every item marked **[live]** gives the read that proves it
(livetools trace address + read spec; `*[reg+off]:N:type` = deref the pointer at reg+off, then read N bytes).
Scratch scripts: `Vibe-Reverse-Engineering/patches/AssassinsCreed/scripts/lights/` (`replay_consts.py`
replays the trace's constant state per draw -> `dumps/lights_f0.json`; `draw_detail.py <seq>` dumps one draw).

## 1. Shader side (colour pass = pass 2, RT 0x36523A20, 1428 of 1531 draws carry lighting constants)

CTAB struct layouts (static VS e.g. `dumps/lights_vs_5a1346ea.txt`, normal-mapped PS 0x1CFFC7C0):

| param | regs | layout (float4 each) | shader use |
|---|---|---|---|
| g_DirectLights[3] | c40..c45 | {m_Direction.xyz, m_Color.rgb} | `dp3_sat(N, c40+2i) * c41+2i` -> m_Direction points TO the light, world space |
| g_ShadowedDirect | c58..c60 | {m_Direction, m_Color, m_SpecularDirection} | VS: `if b0: dp3_sat(N,c58)*c59` -> o7 (sun term, shadowed in PS) |
| g_HasSun | b0 | bool | gates the c58 term |
| g_NumLights | c31 | (numDirect, numOmni, numSpot, 0) | `slt c31.x`, `if_lt i, c31.y`, `if_lt i, c31.z` |
| g_OmniLights[4] | c32..c39 | {PositionFar = pos.xyz, Far²}, {ColorFade = rgb, 1/(Far²-Near²)} | `att = sat((w0 - d²) * w1)`, `dp3_sat(N, L) * rgb * att` |
| g_SpotLights[3] | c46..c57 | {PositionFar}, {ColorFade}, {m_Direction}, {m_ConeAngles} | dist att as omni; `cone = sat((cA.x - dot(Lhat, m_Direction)) * cA.y)` |
| g_AmbientCube[6] | c24..c29 | +X, -X, +Y, -Y, +Z, -Z (rgb) | HL2 cube: n.z *= c118, normalize, sum n²·cube[sign] |
| g_AmbientNormalTopBendFactor | c118 | scalar | 1.5 in frame 0 |
| g_FogParams / g_FogColor | c17 / c16 | (start, 1/range, max, -) / rgb | `fog = sat((dist - x)*y)*z` |

World space throughout (VS computes world pos/normal with g_World c8; Lhat = normalize(lightPos - P)).

### Frame-0 values (colour pass)
- Sun: dir-to-light (0.39400538, -0.68243724, 0.61566138) (= elevation 38.0°, azimuth atan2(x,y) 150.0°),
  colour (1.5799999, 1.3866665, 1.1333332). In c40 (unshadowed copy) and c58 (shadowed, b0 = 1).
  A second sun-like record appears on 28+ draws: PS c41/c59 colour (0.85, 0.84, 0.8233), PS c60 specular dir
  (0.44147378, -0.76465493, 0.4694716) (uploaded by FUN_00ABAC50 at 0xABAE4D/0xABAED1) — origin unproven **[live-A]**.
- VS c59 = sun × 0.2 (0.316, 0.277, 0.227) and ambient × 2.5 on 123 draws: written by material command lists
  (MaterialInstance_BeginDraw 0xA4B1B0 -> 0xA49DD5 -> 0x7A2F09), i.e. per-material overrides, not the light system.
- Ambient cubes (from FUN_00ABAA40): S1 = +X (.346,.381,.402) -X (.367,.335,.296) +Y (.36,.36,.36)
  -Y (.36,.335,.325) +Z (.579,.628,.628) -Z (.339,.272,.24); S3 = +X (.268,.246,.217) -X (.309,.290,.268)
  +Y (.312,.386,.393) -Y (.316,.357,.393) +Z (.467,.540,.551) -Z (.316,.265,.224). Which is env+0x00 vs +0x70: **[live-B]**.
- One omni in use (1 draw with c31 = (0,1,0,0)): pos (-48.794918, -29.424192, 37.08533), Far² 169 (13 m),
  fade 1/169 (near 0), rgb (0.58352941, 0.48627451, 0.38274508). No spot lights, no extra directionals (c31.x = 0, .z = 0).
- The 224-register PS bulk upload also holds omni c34..c39 and spot c46..c53 values that no draw enables
  (c31 counts 0): stale shadow-register contents, ignore.
- Fog off (c17 = 0 in VS and PS: FUN_00ABAA40 uploads zeros when pass ctx +0x60 == 0). FogColor 0.0676.
- The engine never sets D3DSAMP_SRGBTEXTURE (type 11) nor D3DRS_SRGBWRITEENABLE (194) in the trace: lighting is
  computed on gamma-encoded albedo; Remix lights in linear space, so light colours cannot be copied 1:1 (see 3.3).

## 2. Engine side

### 2.1 Per-draw upload: FUN_00ABAC50 (installed as pass ctx +0x1C0 pmf by FUN_00AC0F30 at 0xAC1006,
called via FUN_00AA2E50 from each Submit: 0xAE04D6 static, 0xAE0948 masked, 0xAE1929 skinned)
`__thiscall(ECX = pass ctx, param_1 = draw state, param_2 = DX9GraphicObjectInstance)`, ret 8.
- Light set S = inst+0x90 (ctx+0x1CC == 0) or ctx+0x1C0 override:
  u16 `S+0`: bits0-4 first direct record idx, bits5-9 first spot idx, bits10-14 sun idx (float4 units);
  u8 `S+2`: bits0-2 omni count, bits3-4 direct count, bits5-6 spot count, bit7 has sun;
  u8 `S+3`: bit0 upload PS, bit1 upload VS; u32 `S+8` (= inst+0x98) -> record array (float4s).
  Omni records at index 0 (2 float4 each), direct 2, spot 4, sun 3.
- c31 = (direct, omni, spot) counts, b0 = has-sun; sun also appended to c40 when ctx+0x59 == 0 and direct < 2.
- Environment E = *(inst+0x80)+0x40 (or ctx+0x70); FUN_00ABAA40(state, E, flag), flag = (*(inst+0x84)+0x60 >> 16)&1:
  ambient cube E+0x00 (flag 1) or E+0x70 (flag 0), 6 float4; bend factor at +0x60 of the chosen block;
  FogParams E+0x100, FogColor E+0x110; E+0x124 env texture link (sampler param 0xD7), E+0x140 param 0xD8.
  Class of *(inst+0x80) unproven (candidate WorldAmbiance vtable 0x16BE1E4) **[live-B]**.

### 2.2 Record builders (FUN_00AD4C50, from FUN_00AD5570 per lit object)
Light object `L` = DX9GraphicObjectInstance of type 1 (`L+0x24 & 7 == 1`, FUN_00AA18F0): AABB L+0x00/+0x10,
world matrix rows L+0x40 (X), L+0x50 (Y), L+0x60 (Z), L+0x70 (position); Light resource `R = [L+0xA4]`.
Light resource (serializer FUN_00A05D80): R+0x10 intensity (float), R+0x100 colour rgb (3 floats),
R+0x110 u16 flags (bit 9 = dirty, FUN_00AA14B0). Type = R vtable slot 5 (`R->vt[5]()`):

| class | vtable | slot-5 id | extra fields |
|---|---|---|---|
| OmniLight | 0x16BE5F4 | 0x344780D6 | +0x120 near, +0x124 far (ctor default 0 / 10.0, FUN_00A06670) |
| SpotLight | 0x16BE5B4 | 0x80320FB8 | +0x120 inner half-angle, +0x124 outer half-angle (rad), +0x128 link, +0x12C near, +0x130 far |
| DirectionalLight | 0x16BE634 | 0x7E15FD50 | — |
| SunLight | 0x16BE6B4 | 0x5EDC3E04 | +0x120 byte "custom specular dir", +0x130 float4 specular dir |
| DirectionalSphereLight | 0x16BE674 | 0xC159B05D | ignored by FUN_00AD5570 |

- Omni FUN_00AD49C0: PositionFar = (L+0x70, far²); ColorFade = (R.rgb · R+0x10 · fade/255, 1/(far² − near²)).
- Spot FUN_00AD4AC0: as omni with near/far = +0x12C/+0x130; m_Direction = −(L+0x50); ConeAngles =
  (cos(R+0x124), 1/(cos(R+0x124) − cos(R+0x120)), 0, 0). So the spot points along **+Y** of L's matrix and
  cone = linear ramp from cos(outer) to cos(inner). FUN_00FA09A0 = cos: CRT x87/SSE2 dispatch; the stale spot
  record (0.24192188, −1.5731663) gives cos(inner) = 0.87758 = cos(0.5 rad) (derived; confirm **[live-C]**).
- Direct/Sun FUN_00AD4010: m_Direction = −(L+0x50) (light travels along +Y), colour = R.rgb · R+0x10 · fade/255.
  Sun 3rd float4 = −(L+0x50) or R+0x130 when byte R+0x120 != 0.
- `fade` = byte +0x11 of the selection entry (FUN_00AD54E0 keeps at most 4 omni, 2 direct, 2 spot, 1 sun per
  object); it is a per-object priority fade, so for Remix use 255 (full colour · intensity).

### 2.3 Per-frame list of visible lights
FUN_00AA1AD0(X) (from FUN_009F1450 <- 0x9E53C7 / 0x9E6690, X = [view+0x1E0]) -> FUN_00AA19B0(X+0xA40, X+0xA4C):
X+0xA40 {ptr, count} = objects to (re)light; **X+0xA4C = ptr to array of L, X+0xA50 = count** = visible light
objects of this view (FUN_00AA14B0 walks them). Light sets are cached per object (inst+0x24 bit 0x800) and rebuilt
only when a light is dirty, so hooking FUN_00AD5570 does not see every light every frame; the X+0xA4C list does.

**[live-D]** (prove the list, its per-frame call rate and contents): trace 0xAA1AD0 entry, read `[esp+4]:4:ptr`
(= X; count hits per frame). Then (second trace, X known) `[X+0xA4C]:4:ptr`, `[X+0xA50]:4:u32`; for each L:
`[L+0xA4]:4:ptr`, `*[L+0xA4]:4:ptr` (vtable, compare table above), `[L+0x24]:4:u32` (&7 == 1),
`[L+0x70]:12:float3`, `[L+0x50]:12:float3`, R: `[R+0x10]:4:float`, `[R+0x100]:12:float3`, `[R+0x120]:8:float2`,
`[R+0x12C]:8:float2`. Expected in Masyaf start: a SunLight with −Y = (0.394, −0.682, 0.616) and
R.rgb·R+0x10 = (1.58, 1.387, 1.133); an OmniLight at (−48.79, −29.42, 37.09), far 13.
**[live-A]** trace 0xABAC50 entry: `[esp+8]:4:ptr` (= inst; `*[esp+8]:4:ptr` = its vtable); then
`[inst+0x90]:4:u32` (w0 | counts<<16 | flags<<24), `*[inst+0x98]` records; sun record =
`[rec + ((w0>>10)&31)*16]:48:float[12]` for draws whose PS c41 is 0.85.
**[live-B]** trace 0xABAA40 entry: `[esp+8]:4:ptr` (E), `[esp+0xC]:1:u8`, `[E+0x00]:96:float[24]`,
`[E+0x70]:96:float[24]`, `[E+0x100]:32:float[8]`; and `*[inst+0x80]:4:ptr` vtable of the owner.
**[live-C]** read `[R+0x120]:8:float2` of a SpotLight and compare cos() with the record's ConeAngles.

## 3. Remix side

### 3.1 API and bridge (all verified in source)
- `remixapi_LightInfo` {sType, pNext, hash, radiance, isDynamic, ignoreViewModel} (remix_c.h:709-716).
  EXT: SphereEXT {position, radius, shaping_hasvalue, shaping_value{direction, coneAngleDegrees, coneSoftness,
  focusExponent}, volumetricRadianceScale} (603-619); DistantEXT {direction (= travel direction),
  angularDiameterDegrees, volumetricRadianceScale} (668-675); DomeEXT {transform, colorTexture path} (677-682).
- Bridge serializes LightInfo + Sphere/Rect/Disk/Cylinder/Distant/Dome/USD for CreateLight and
  UpdateLightDefinition (client `bridge/src/client/remix_api.cpp:259-363`, server `bridge/src/server/main.cpp:175-261,
  3121-3159, 3260-3270`, field lists `bridge/src/util/util_remixapi.cpp:739-905`). DestroyLight/DrawLightInstance
  forwarded (remix_api.cpp:420-444). **Gaps:** CreateLightBatched not exposed (interface table remix_api.cpp:505-533);
  AutoInstancePersistentLights is a client stub (remix_api.cpp:525-530) — not needed, the runtime calls it itself on
  native Present (`src/d3d9/d3d9_swapchain.cpp:493`); CreateLight failures on the server are only logged (main.cpp:3131).
- Runtime: handle = `info.hash`, must be non-zero (rtx_remix_api.cpp:1549-1552). Every created light is registered
  persistent and auto-instanced each frame (1612-1620, rtx_fork_light.cpp:89-97): **CreateLight once, DestroyLight when
  gone; DrawLightInstance is not required.** Updates via UpdateLightDefinition are queued and applied at Present
  (rtx_remix_api.cpp:2528-2573, 2480-2486). Non-dynamic lights stop accepting updates after
  `numFramesToPutLightsToSleep` updates (rtx_fork_light.cpp:131-146, counter grows on every update) -> set
  `isDynamic = true` for any light that is ever updated (sun, moving/flickering lights).
- DomeEXT is only active with a valid texture (rtx_light_manager.cpp:492-501); the texture must be a `.dds` file path
  (AssetDataManager::findAsset, rtx_asset_data_manager.cpp:578-591), sampled lat-long (light_helper.slangh:40-50).
  Only one dome is active (rtx_fork_light.cpp:82-96).

### 3.2 Mapping
- Sun / DirectionalLight -> DistantEXT: direction = +Y of L (= L+0x50, = −c40), radiance = R.rgb·R+0x10.
  Remix distant sample radiance = radiance/sin²(halfAngle) over a cone of solid angle ≈ π sin² (distant_light.slangh:61-91),
  so a surface receives E ≈ π·radiance and Lambert gives albedo·radiance·N·L — the same form as the engine term
  `albedo · c41 · dp3_sat(N, c40)`. angularDiameterDegrees: engine has none; use Remix's sunSize default 0.545.
- OmniLight -> SphereEXT at L+0x70, no shaping. Engine falloff `1 − d²/far²` (near = 0) has no Remix equivalent (Remix is
  unbounded 1/d²). Radiance choice is a design parameter, not engine data: e.g. match irradiance at d = far/2:
  radiance = rgb·0.75·(far/2)² / (π r²) for sphere radius r. Mark tuned values as such.
- SpotLight -> SphereEXT + shaping: direction = +Y of L, coneAngleDegrees = deg(R+0x124),
  coneSoftness = cos(R+0x120) − cos(R+0x124) (Remix: smoothstep(cosCone, cosCone+softness, cosθ),
  light_shaping.slangh:92-105; engine: linear ramp between the same two cosines), focusExponent = 0.
- Ambient cube: no Remix light type. Options: (a) DomeEXT with a small lat-long .dds written by the mod from the six
  cube colours (E block) — engine-faithful ambient, visible as the sky background; (b) Numos sky (below).
- Colour space: engine multiplies gamma-space albedo (no sRGB states in the trace); Remix linearizes albedo. Any global
  scale between engine colours and Remix radiance is a calibration, to be set once from a screenshot comparison.

### 3.3 Budget
Engine lights ≤ 4 omni + 2 direct + 2 spot + 1 sun per object; visible-list size per frame is **[live-D]**.
Remix handles hundreds of analytic lights (RTXDI); no need to cull below the engine's visible list.

## 4. Sky
- Engine: sky pass FUN_00ABB440 draws (frame 0, seq 31269) one DX9StaticMesh, 512 tris, VS = WVP only, PS unlit:
  `lerp(tex(s0, uv+c128.z), tex(s1, uv+c131.z), 0.4) * 0.9176` (scrolling UV offsets 0.320 / 0.192) — an LDR cloud
  dome, no sun/sky colour constants. LayeredSky code (0xA0072B/0xA00A69..) adds 4 textured quads (seq 31279-31287,
  PS = tex × vertex colour). The embedded cloud-plane shaders (CTAB strings 0x16C5248 g_SunDirection, g_SunIntensity,
  g_CloudPlane*) are not drawn in frame 0.
- Remix: API instances with the SKY category are hidden and not rasterized into the sky probe
  (rtx_instance_manager.cpp:1022-1026), so the dome cannot be reused through the API.
- Recommended: `rtx.skyMode = Numos` (rtx_options.h:1304) driven by the engine sun: each frame (on change)
  `SetConfigVariable("rtx.atmosphere.sunElevation", asin(t.z))` and `("rtx.atmosphere.sunRotation", atan2(t.x, t.y))`
  in degrees, t = −(L+0x50) of the SunLight (rtx_atmosphere.cpp:582-592 with rtx.zUp = True -> world = (x, z, y) of the
  Y-up frame, 2848-2851). Frame 0 gives 38.0° / 150.0°. Numos creates its own sun distant light
  (rtx_atmosphere.cpp:2869-2888), so **do not also create a DistantEXT sun in Numos mode**; its intensity comes from
  rtx.atmosphere.sunIlluminance × sunIntensity (596), not from the engine colour.
- Fallback (engine-faithful lighting): skyMode SkyboxRasterization + DistantEXT sun + DomeEXT from the ambient cube.

## 5. Implementation plan
1. Hook 0xAA1AD0 (entry, [esp+4] = X). After it returns, walk X+0xA4C/X+0xA50 (after [live-D] confirms).
   Per L: skip unless `L+0x24 & 7 == 1`; R = [L+0xA4]; type by `[R]` vtable (table 2.2).
2. Keep a map L -> {handle, last params}. New L: CreateLight(hash = (uint64)L | type<<32 or FNV of L, non-zero).
   Changed params: UpdateLightDefinition with isDynamic = true. L absent for N frames or level unload: DestroyLight.
3. SunLight: Numos mode -> push sunElevation/sunRotation only when changed (> 0.01°); fallback mode -> DistantEXT.
4. Ambient (fallback mode): hook 0xABAA40 or read at the existing static-submit hook: E = *(inst+0x80)+0x40,
   choose block by the material flag; when the block changes, write a 64×32 lat-long .dds (+Z at the pole matching
   rtx.zUp, verify transform in DomeEXT) and UpdateLightDefinition(DomeEXT).
5. Fog: E+0x100/+0x110 when pass ctx +0x60 != 0 -> later phase (Remix volumetrics), not needed for lights.
6. Verification: rtx.debugView light views + the engine values from [live-D]; one screenshot comparison to set the
   single global colour->radiance scale (3.2), recorded as a tuned value.
