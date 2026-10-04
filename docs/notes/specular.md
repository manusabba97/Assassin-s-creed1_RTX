# Specular — colour-pass pixel shader census and Remix mapping (design notes)

Status: static analysis only (dx9 trace `frame_capture_01.jsonl` frame 0 pass 2 = RT 0x36523A20, shader bytecode
disassembly, Ghidra decompilation of AssassinsCreed_Dx9.exe v1.02, dxvk-remix source). Nothing here is live-verified.
Items marked **[unproven]** are not established by data yet.

Scripts (all in `Vibe-Reverse-Engineering/patches/AssassinsCreed/scripts/specular/`, outputs in `dumps/specular/`):

| script | what it does |
|---|---|
| `extract.py` | replays the trace; per frame-0 pass-2 draw: PS/VS handle + md5[:8] of the disasm (same naming as `nm_pairs.py`), full PS float-constant state c0..c223, textures, backtrace. Writes `f0_pass2.json`, `ps_<hash>.txt`, `vs_<hash>.txt` |
| `sm3.py` | symbolic ps_3_0/vs_3_0 data-flow evaluator (per register component; SM3 positional swizzles, scalar/dot/nrm/texld rules, if/else -> phi) |
| `analyze.py` | finds every `pow`, classifies its base by data flow, walks forward from it through multiplies to the add that joins the diffuse sum, reads the exponent. Writes `census.txt` / `census.json` / `vs_outputs.txt` |
| `values.py` | per-draw values of the constants by role. Writes `values.txt` / `values.json` |
| `table.py` | the census table below |
| `find_vt_1b4.py` | byte scan of the exe for `[reg+0x1B4]` (IDirect3DDevice9::SetPixelShaderConstantF) |

## 1. Method

Leaf classes come from data, not from names:
- **L**: CTAB light constants `g_DirectLights[i].m_Direction` (c40+2i), `g_OmniLights[i].m_PositionFar` (c32+2i).
- **V**: `g_EyePosition` (c12) in the PS, or a PS input whose VS output is built from `g_EyePosition` (every VS
  here: `texcoord6 = g_EyePosition - worldPos`, see `vs_outputs.txt`, e.g. static VS `add r4.xyz, -r2, c12`).
- **N**: a sampler whose CTAB name contains "normal", or a PS input whose VS output is built from the vertex
  normal/tangent/binormal.

`pow` classification: **PHONG** = `dot(A, B)` where A holds a `dot(L, N)` and only L/N leaves (the reflect vector)
and B only V; **BLINN** = `dot(N, H)` with H holding L and V; **RIM** = `pow(1 - sat(dot(N, V)), e)`; **SHEEN** =
`pow(|dot(N, V)|, e)`.

Checked by hand against the disassembly: `5491ee99` (the example in the brief), `e7f64a98`, `ec408913`, `7fd02edd`,
`4b10e484` (branch/phi + hard-coded Fresnel), `874b3319`. The tracer output matches the listing in each case.
Output swizzles like `.zxy` in `census.txt` only reflect `dest .yzw` register packing (SM3 positional swizzle). They
are the same xyz channels.

## 2. Census (frame 0, colour pass, 1531 draws, 40 distinct PS)

Result: **every specular lobe is Phong, `pow(sat(dot(reflect(-L, N), V)), n)`. No shader uses Blinn N.H.** Specular
exists only for the first directional light (`g_DirectLights[0]`, c40/c41) and, in 7 shaders, the first omni
(`g_OmniLights[0]`, c32/c33, with the usual `sat((Far² - d²) * c33.w)` falloff). Every lobe is multiplied by
`sat(dot(N, L))` and the light colour. It is then added to the diffuse sum and never multiplied by albedo, except that
`7fd02edd` uses albedo.r as a mask. **233 of 1531 draws** carry a Phong term: 220 skinned (0xAE1810), 13 static
(0xAE03C0). **1172 draws have no `pow` at all**, including the dominant static world shader `eceb2d0b` (566 draws),
the layered terrain/wall shader `7a87be5e` and the static `e56794d5`/`d11e037c`. None of those shaders uses
`exp`/`log` either (instruction histogram), so they have no specular term.

Notation: `T = tex(specular sampler)`, `F5 = (1 - dot(V, N))^5` (computed as `|x|^5`), `Lc` = light colour.

| PS | draws | submit | exponent n | intensity (times `sat(N.L)` and `Lc`) | constants (reg: CTAB name = role) |
|---|---|---|---|---|---|
| b1b370c7 | 69 | skinned | `T(s5 Operator9_5).y * 5` | `T.x * Operator6_11.rgb` | c130 Operator6_11 = COLOR |
| 7fd02edd | 43 | skinned | `T(s4 Operator23_4).y * 10` | `tex(s5 Operator17_5 @ uv*c129).x * T.x * 1.5 * (tex(s1 Operator48_1) * tex(s2 diffusemap_2)).r * SpecularColor_6.rgb` | c128 SpecularColor_6 = COLOR |
| d847d387 | 24 | skinned | `T(s6 SpecularMap_6).z + c134` | `c135 * T.rgb * c133.rgb` | c133 Operator6_33 = COLOR, c134 Operator7_35 = POWER, c135 Operator8_37 = FACTOR |
| c83dd075 | 22 | skinned | `T(s6 SpecularMap_6).z + c132` | `c133 * T.rgb * c131.rgb` | c131 Operator6_29 COLOR, c132 Operator7_31 POWER, c133 Operator8_33 FACTOR |
| ec408913 | 20 | skinned | `T(s3 SpecularMap_3).z + c132` | `c133 * T.rgb * c131.rgb` | c131 Operator6_20 COLOR, c132 Operator7_22 POWER, c133 Operator8_24 FACTOR |
| 466478d4 | 12 | skinned | same as ec408913 | same | same registers/names |
| c7b086de | 11 | skinned (+1 0xA17C10) | same as ec408913 (dir0 + omni0) | same | same |
| 74673c36 | 8 | skinned | `T(s5 Operator9_5).y * 5` | `T.x * Operator6_11.rgb` | c130 Operator6_11 COLOR |
| 554bde55 | 5 | static | `T(s4 Operator4_4).z + c131` | `lerp(lerp(c132, 1 - tex(s2).x, c129), tex(s2).w, c129) * lerp(c133, 1, F5) * T.rgb * lerp(1, tex(s2).rgb, c129)` | c131 SpecularPower_10, c132 SpecularFactor_12, c133 Fresnel_17, c129 BloodFXIntensity_4 (s2 = Operator29_2) |
| e7f64a98 | 5 | static 4, skinned 1 | `c132` | `c133 * T(s2 specularmap_2).rgb * c131.rgb` | c131 SpecularColor_13, c132 specpower_15, c133 specfactor_16 |
| 07067896 | 4 | skinned | same as ec408913 (omni0 + dir0) | same | same |
| 6fb31593 | 2 | skinned | `T(s3 Operator4_3).z + c130` | `c131 * lerp(c132, 1, F5) * T.rgb * c129.rgb` (omni0 + dir0) | c129 SpecularColor_5, c130 SpecularPower_8, c131 SpecularFactor_10, c132 Fresnel_11 |
| 5491ee99 | 2 | static | `T(s3 Operator4_3).z + c130` | `c131 * lerp(c132, 1, F5) * T.rgb * c129.rgb` | c129 SpecularColor_3, c130 SpecularPower_6, c131 SpecularFactor_8, c132 Fresnel_9 |
| 954f8f56 | 1 | static | as 554bde55 (omni0 + dir0) | as 554bde55 | as 554bde55 |
| f5089a0a | 1 | skinned | `c129` | `T(s4 SpecularMap_4).x * c128.rgb` (omni0 + dir0) | c128 SpecularColor_4, c129 SpecularPower_5 |
| 8fb55621 | 1 | skinned | as 5491ee99 (omni0 + dir0) | as 5491ee99 | as 5491ee99 |
| 4b10e484 (eye) | 1 | skinned | `c131` | `c132 * lerp(0.2 + 0.8 * F5', 1, F5)` (0.2/0.8 = `def c1.zw`; F5' uses V = normalize(eye - v2)); also `cube(s2 CubeMap_0, reflect)` added to albedo before lighting | c131 SpecularPower_15, c132 SpecularFactor_16 |
| af0c8766 | 1 | skinned | as ec408913 (Operator6_22/7_24/8_26 = c131/c132/c133) | same | same roles |
| 874b3319 (water) | 1 | static | `c139` Operator9_25 | `1` (lobe * Lc * sat(N.L) only); the albedo is `cube(s2).rgb * CubeMapIntensity c136 * Color c135` | c139 POWER |

Shaders without specular (draws): eceb2d0b 566, e56794d5 149, d11e037c 94, 7a87be5e 92, 9b85bc21 74, fe8e8298 60,
a8a8a446 35, 8e51fc79 33, e4687ad1 20, e11bd5f6 19, c771f4c5 9, 764c4858 8, e0b22c67 7, 62748094 4, e119d051 1,
9c7f099f 1. Skinned shaders with only non-specular `pow`s: 3acce3d4 61, ef891748 31, 15230681 19, fb6f164e 9,
1d96ce54 6.

Other `pow` uses (not light-dependent, listed so they are not mistaken for specular):
- **RIM** (all `TargettingRimFX*` shaders): `+ pow(1 - sat(N.V), TargettingRimFXFactor) * lerp(albedo, TargettingRimFXColor, BlendDiffuse) * TargettingRimFXIntensity`.
  The intensity is 0 in 343 draws and 1 in 9 draws in frame 0. It is a target highlight effect, not a material property.
- **SHEEN** (character diffuse, e.g. ec408913 instr 39..46): `albedo' = albedo + (0.7 * albedo + 0.1) * (1 - pow(|N.V|, e))`,
  e = RimPower / Operator29 / Operator34 / `def 0.7`. The frame-0 values are 0.7 (111 draws), 0 (30 draws; with pow = 1 the term is 0)
  and 0.5 (5 draws). This is a view-dependent albedo boost with no Remix opaque equivalent.
- Diffuse is a ramp lookup `tex(ramp, (N.L + 1) / 2)` in the character shaders (s2/s3 "Ramp"/"DiffuseRamp"/"skinramp"),
  not Lambert. This is a side note for the lighting work.

Constant-name families (observed in this capture only): in all 7 "Operator" character shaders with a SpecularMap,
`Operator6_*` = COLOR (xyz), `Operator7_*` = POWER (added to `SpecularMap.z`), `Operator8_*` = FACTOR. The spec-map
sampler is `SpecularMap_N`, `Operator4_N` (Specular* family), `Operator9_5`/`Operator23_4` (skin) or `specularmap_2`.
A mod-side role table should be keyed by the PS (bytecode hash or full CTAB signature) and generated by `analyze.py`.
Name matching alone cannot resolve `OperatorN`. Coverage of other levels: **[unproven]** (only this capture was
analysed).

## 3. Frame-0 values (from the replayed SetPixelShaderConstantF state at each draw, `values.txt`)

| role | values x draws |
|---|---|
| POWER (added to `T.z`, or the whole exponent where there is no `+T`) | 1.649 x24, 1.0 x22, 0.801 x19, 7.067 x12, 2.0 x11, 3.523 x5, 5.0 x4, 2.598 x3, 3.0 x2, 5.697 x2, 10.0 x1, 3.793 x1, 0.25 x1, 1.415 x1, 2.209 x1, 10.3 x1, 100 x1 (eye), 1.601 x1, 64 x1 (water) |
| texture exponent (skin) | `T.y*5` (b1b370c7/74673c36, 77 draws), `T.y*10` (7fd02edd, 43 draws) |
| FACTOR | 3.297 x24, 1.0 x24, 2.801 x19, 1.201 x12, 4.676 x6, 2.842 x5, 0.5 x5, 2.315 x4, 0.1 x3, 1.8 x2, 1.342, 0.689, 2.741, 0.131, 0.6, 10 (eye), 5.467 |
| COLOR | (1,1,1) x79, (0.6)x3 x44, (0.659,0.641,0.607) x21, (0.620,0.435,0.302) x19, (0.774)x3 x16, (0.455)x3 x9, (0.557,0.502,0.427) x8, (0.5)x3 x7, (0.730)x3 x7, (0.952,0.945,0.908) x5, (0.741)x3 x3, (0.329)x3 x3, (0.710,0.447,0.282), (0.137,0.184,0.2), (0.859,0.776,0.678), (0.369)x3 |
| FRESNEL (`lerp(F, 1, (1-N.V)^5)` = Schlick with F0 = F) | 1.0 x10, 2.047 x1 |
| BloodFXIntensity | 0 x6 |

So for the Specular* family, Fresnel = 1 makes the factor constant (no Fresnel effect) in 10 of 11 draws. The
exponents are low: for `T.z + POWER` with T.z in [0,1] (assuming a UNORM format, see section 6), n lies between 0.8 and 8
for most characters. Each shader uses 1-7 distinct parameter sets (`values.txt`, "distinct full parameter sets").

## 4. Where the values live at draw time

**Device wrapper PS float-constant shadow: `wrapper + 0x1010 + reg*16`, 224 float4 (c0..c223).**
- `FUN_00AA9540(reg, const float* data, n)` (thiscall on the device wrapper, ret 0xC) is the only
  SetPixelShaderConstantF path in the capture. It rejects `reg >= 0xE0 || reg+n > 0xE0`, compares each float4 with
  the shadow at `ECX + (reg + 0x101) * 16` = `+0x1010 + reg*16` (FUN_00AA83A0: 4-dword bitwise equality), copies the
  changed ones and calls `(*wrapper)->vtbl[0x1B4/4]` = IDirect3DDevice9::SetPixelShaderConstantF (trace slot 109) only
  for runs of changed registers.
- The VS twin `FUN_00AA93C0` uses `+0x10 + reg*16` (256 regs, vtbl +0x178 = SetVertexShaderConstantF). This matches
  the known skinning shadow `+0x790 = c120` (0x10 + 120*16 = 0x790). The PS block starts right after it (0x10 + 256*16 = 0x1010).
- State restore `FUN_00AA5F90`: at 0xAA8347..0xAA835B it re-uploads the whole shadow,
  `SetPixelShaderConstantF(0, wrapper+0x1010, 0xE0)`. It does the same for VS F `+0x10` x0x100, `+0x1F10` (vtbl 0x1BC,
  SetPixelShaderConstantI) and `+0x2050` (vtbl 0x1C4, SetPixelShaderConstantB). It also clears the shader caches
  +0x2994/+0x2998.
- Trace check: all 4188 SetPixelShaderConstantF calls in the capture come from these two sites (4174 with return
  address 0xAA9600 inside FUN_00AA9540, 14 with 0xAA835D, the 224-register restores). Two other raw call sites exist in
  the binary (`FUN_00AA5440` pass-through with 0 static xrefs, and 0xAA8B42) and bypass the shadow. Neither was used in
  the capture. Whether they run elsewhere is **[unproven]**.
- The wrapper is `item + 0x04` (game.h). MaterialInstance_BeginDraw passes `item[1]` to the command-list walker.

**Who writes c128+ (material parameters):**
- Material command lists: MaterialInstance_BeginDraw 0xA4B1B0 calls the walker FUN_00A173C0 at 0xA4B28E. The walker
  dispatches through table 0x193851C, where opcode **7 = FUN_00A17440 = PS float constants**. Its data block is
  `count x {u16 reg, u16 n}` headers followed by the float4 payloads in order. It calls `FUN_00AA9540(reg, payload, n)` per header.
  Opcode 10 = FUN_00A17530 calls FUN_00AA93C0 (VS float). Opcodes 8/9/11/12 call AA9D90/AA9620/AA9C90/AA9490
  (roles not checked). Trace: every c128..c139 upload in pass 2 has return 0xA17471 -> 0xA173ED -> 0xA4B293 in its backtrace.
- Dynamic material parameters: if `MI+0xC & 4`, BeginDraw calls FUN_00AC45D0 at 0xA4B2A4/0xA4B2B4 with
  ECX = `[rec+0xC]` / `[rec+0x10]` (rec = `[MI+4] + pass*0x18`). The entries are 16 bytes at `[list+0x24]`, count
  `[list+0x28] & 0x3FFF`: `+4 u16 reg`, `+6 flags (bit0: PS, else VS)`, `+8 n`. The value pointer comes from
  FUN_00A50750, and the upload goes through FUN_00AA9540/AA93C0. In frame 0 this path did 4 uploads
  (c128..c130 x2, c131..c133 x2, return 0xAC462D).
- MaterialInstance_EndDraw (0xAA3130..0xAA31A6) uploads only c71 (0x47) and c72 (0x48) after the draw, via
  FUN_00AA9540 at 0xAA3186/0xAA319B. It never touches c128+.

**Recommendation (data-backed):** in the existing post-draw hook, read `float4 c[reg] = *(wrapper + 0x1010 + reg*16)`
for the roles of the bound PS. The PS comes from cache +0x2998, and the CTAB is already parsed in materials.cpp. This
covers both the command-list and the dynamic path, and it is exactly what the draw used, because nothing writes
c128+ between BeginDraw and EndDraw. Live confirmation: `livetools` read of `[item+4]+0x1010+128*16`, 16 float4, at
0xAA3130 entry, compared against a dx9 trace of the same draw **[unproven]**. Texture channels (`T.z`, `T.xyz`) come
from the sampler bound at `+0x29D8 + stage*4`.

## 5. Remix side (dxvk-remix source)

- `remixapi_MaterialInfoOpaqueEXT` (public/include/remix/remix_c.h:240-263) has roughnessTexture/metallicTexture,
  anisotropy, albedo/opacity/roughness/metallic constants, thin film, height, and alpha/blend state. `remixapi_MaterialInfo`
  (remix_c.h:302-318) adds albedo/normal/tangent/emissive textures. **There is no specular level, specular colour,
  F0 or IOR input for opaque materials.**
- Roughness is **perceptual**. rtx_remix_api.cpp:594 passes `extOpaque->roughnessConstant` unchanged. The shader
  reads the constant or `roughnessSample.x` (opaque_surface_material_interaction.slangh:838-843) and applies
  `saturate(r * roughnessScale + roughnessBias)` (:853; `rtx.opaqueMaterial.roughnessScale` default 1.0,
  pass/material_args.h:81). `calcRoughness` then squares it: `alpha = r^2` (brdf.slangh:65-67 perceptualRoughnessToRoughness,
  used at :109, clamped to 1e-4 at :113/:35). The comment at :879 says so explicitly. `alpha` is the GGX alpha
  (`evalGGXNormalDistributionIsotropic`, brdf.slangh:299).
- Dielectric F0 = 0.04: `materialBaseReflectivityDielectric = vec3(0.04)` (brdf.slangh:36), and
  `calcBaseReflectivity = mix(0.04, albedo, metallic) * opacity` (brdf.slangh:58-60, used at
  opaque_surface_material_interaction.slangh:877). Remix has no Phong/specular-power conversion anywhere
  (searched src/dxvk/rtx_render and shaders).

## 6. Proposed mapping

**Roughness from the Phong exponent n** (n = `T.z + POWER`, `T.y*5`, `T.y*10` or `POWER`, per shader, section 2):
1. Phong (R.V) to Blinn-Phong (N.H): `m = 4n`. Derivation: the lobe is peaked at small angles; with θh the
   half-vector angle, the R-to-V angle is 2θh when L, N, V are coplanar, so `cos(2θh)^n ≈ exp(-2nθh²)` and
   `cos(θh)^m ≈ exp(-mθh²/2)`, which gives m = 4n. This is an approximation: it is exact only in the plane of incidence
   and at small angles.
2. Blinn-Phong to Beckmann: `alpha = sqrt(2 / (m + 2))`. Source: Walter, Marschner, Li, Torrance, "Microfacet Models for
   Refraction through Rough Surfaces", EGSR 2007, section 5.2 (the Phong/Beckmann correspondence `alpha_p = 2 alpha_b^-2 - 2`).
   Citation from the literature; the paper was not re-read in this session. They use the same alpha for GGX as for Beckmann
   (the GGX tails are longer, so the match is approximate).
3. Remix perceptual roughness: `r = sqrt(alpha)` (brdf.slangh:65-67). Combined:
   **`r = (2 / (4n + 2))^(1/4)`**.

   | n | 0.25 | 0.8 | 1 | 1.65 | 2 | 3 | 5 | 8 | 10 | 64 | 100 |
   |---|---|---|---|---|---|---|---|---|---|---|---|
   | r | 0.904 | 0.788 | 0.760 | 0.694 | 0.669 | 0.615 | 0.549 | 0.491 | 0.467 | 0.297 | 0.266 |

   Where n depends on a texel (`T.z + POWER`, `T.y * k`), a constant cannot represent it. The mod has to build a
   derived roughness texture from the spec map channel with the per-draw POWER/scale, the same way remix.cpp already
   decodes DXT normal maps and creates BC5 textures, and key it on the (texture, POWER) pair. The spec-map texture
   formats are not in this trace: all sampled textures were created before the capture, so `GetLevelDesc` at
   conversion time is needed to confirm they are UNORM DXT **[unproven]**.
4. Metallic = 0. In every shader the specular is added to the diffuse term and is not tinted by albedo; the only
   exception is `7fd02edd`, which masks by albedo.r. That is dielectric behaviour: diffuse is kept, F0 is not albedo.

**What cannot be represented** (no opaque input for it, section 5):
- The intensity/mask `FACTOR * COLOR * T.rgb` (0.1..10 x colour x texture) and the tinted COLOR values such as
  (0.62,0.44,0.30). Remix's specular amplitude is fixed by F0 = 0.04 and GGX normalisation. The engine lobe is
  unnormalised Phong (peak = FACTOR * COLOR * T.rgb * Lc * N.L), so energies do not match either. Metallic cannot be
  used for this: it would replace diffuse with albedo-tinted reflection.
- The engine Fresnel `lerp(F, 1, (1-N.V)^5)` with F = 1 (flat) or 2.05 (decreasing). Remix always applies Schlick
  from 0.04.
- The absence of specular: 1172 draws / 16 shaders have none. In Remix every opaque surface reflects with F0 0.04, so
  "no specular" can only be approximated by high roughness. Which value is a design choice, not data.
- SHEEN (view-dependent albedo boost) and RIM (targeting FX): no opaque equivalent.

## 7. Open items
- Coverage beyond this capture (other levels, other material graphs): run extract/analyze on more traces **[unproven]**.
- Spec-map texture formats and whether `T.z`/`T.y` are UNORM [0,1] **[unproven]**; read `GetLevelDesc` of the texture
  bound at the spec sampler.
- Live check of the PS shadow read described in section 4 **[unproven]**.
- The shadow-bypassing SetPixelShaderConstantF sites (FUN_00AA5440, 0xAA8B42) are unused in this capture; their callers
  are unknown.
