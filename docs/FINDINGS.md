# Nobody Wants to Die: findings

Game: Nobody Wants to Die (Critical Hit Games), Steam app 1939970, build 14922697. Unreal
Engine 5.3 (5.3.2 per PCGamingWiki; the exe carries no version string), DirectX 12, project
`detnoir`. No Denuvo, no SteamStub, no PDB.

Paths:
- Game: `E:\SteamLibrary\steamapps\common\Nobody Wants to Die`
- Executable: `detnoir\Binaries\Win64\detnoir-Win64-Shipping.exe` (`detnoir.exe` in the root is
  the launcher stub; a second ReShade `dxgi.dll` there only hooks the stub and does nothing)
- User config: `%LOCALAPPDATA%\detnoir\Saved\Config\Windows\` (`GameUserSettings.ini`,
  `Engine.ini` with only `r.LUT.UpdateEveryFrame=1` added). No engine log (only `Logs\NGX`).

Ships DLSS (310.9.16 already in the plugin folder), XeSS and FSR 3. The menu
(`NWTD_GameUserSettings`) has ray tracing switches: `bRaytracing`, `RtGlobalIllumination`,
`bRtAmbientOcclusion`, `bRtShadows`, `bRtTranslucency`, `bRtReflections`, `bRtVolumetricFog`,
`bRtRayReconstruction`, plus `bFrameGeneration`, upscaling method/preset/sharpness, field of
view, depth of field, chromatic aberration and motion blur. No HDR setting
(`bUseHDRDisplayOutput=False` from the base class).

## Console variable layout (UE 5.3)

Same as Lost Records / Double Exposure (UE 5.2-5.3), read from the exe on disk:
- 7 lookups of `r.ScreenPercentage` and 7 of `r.MaxAnisotropy` load the singleton from RVA
  0x83d0bd8 and call `[rax+0xb8]` (FindConsoleVariable).
- After `r.MaxAnisotropy` lookups: `+0x58` (AsVariableInt); after `r.ScreenPercentage`: `+0x90`.
- Calls after `mov r8d, 0x0?000000`: 0x09, 0x0A and 0x0B all go to `+0x80` (Set).

So `cvars.hpp` is LostRecordsTweaks' unchanged (sets at SetByConsole 0x0A, then copies the value
to the render thread, since UE 5.3 has no SetOnly_Unsafe).

## HDR

The game has no HDR. The add-on uses TrueColorsTweaks' method: `r.HDR.EnableHDROutput=1`,
`r.HDR.Display.OutputDevice=3` (1000 nits) or 4 (2000 nits), `r.HDR.Display.ColorGamut=2`; if
the swap chain isn't HDR10 two seconds later, the add-on sets the colour space itself.

## Probe of 2026-11-01 (menu maxed: sg groups 3 = Epic, AA 2, DLSS Balanced 58%)

The menu has no ray tracing or frame generation options (the user checked); the RT fields in
GameUserSettings.ini are leftovers. Values:
- Ray tracing: `r.RayTracing=1` (start-up, project), `r.RayTracing.EnableOnDemand=1` ("ray tracing
  features can be toggled on demand at runtime"), `r.RayTracing.Enable=0` (0x06),
  `r.Lumen.HardwareRayTracing=0`, `r.PathTracing=1`, `r.RayTracing.Nanite.Mode=1`. No
  acceleration structure builds seen.
- Lumen: GI method 1, reflection method 1, software only, `r.Lumen.TraceMeshSDFs=0` (project),
  probe budget 300, downsample 16, octahedron 8, `ShortRangeAO=0` (project),
  `Reflections.MaxRoughnessToTrace=0.4`, front-layer translucency reflections off
  (`EnableForProject=0`), `HardwareRayTracing.LightingMode=0`.
- Shadows: VSM, 4096 pages, `ResolutionLodBiasLocalMoving=1`, SMRT 8 rays / 4 samples.
- Fog 8 px / 256 slices / 4 supersamples. Pool 2500 MB, anisotropy 8, Nanite 1 px, view
  distance 1. Bloom 5, lens flares 2, film grain 1; DoF, motion blur and CA from the menu.
- Upscalers: DLSS (plugin presets only A-G: no K/M through `r.NGX.DLSS.Preset`; the plugin has
  `r.NGX.DLSS.DenoiserMode` for Ray Reconstruction but no `nvngx_dlssd.dll` ships), FSR 3 with
  frame interpolation (`r.FidelityFX.FI.Enabled=0` by project, `FI.OverrideSwapChainDX12=1`),
  XeSS. No Streamline / DLSS-G.
- HDR: `r.HDR.EnableHDROutput=0` (0x02), `OutputDevice=0`, `MaxLuminance=0`, `MidLuminance=15`,
  `UI.Luminance=300`, `Aces.SceneColorMultiplier=1.5`. Choosing the 0.1.0 HDR option switched the
  swap chain to HDR10 PQ within 0.2 s (the engine did it; no fallback needed).

0.2.0 options follow from this: hardware ray tracing (Enable + Lumen HWRT), hit lighting
(LightingMode 2 + Retrace.HitLighting), RT translucency, RT shadows, Lumen detail, mesh SDF
tracing, short-range AO, reflections roughness and glass, VSM shadows, fog, textures, geometry,
post, FSR 3 frame generation (experimental), HDR with peak (`MaxLuminance`, output device 3), UI and
mid-grey sliders. Presets: Game and Cinematic (no RT, FG or HDR in it).

## 0.2.0 in game (2026-11-01) and the move to the game thread

- Turning on hardware ray tracing (r.RayTracing.Enable + r.Lumen.HardwareRayTracing, set from
  ReShade's present thread) locked the game up. FSR 3 frame generation did nothing (user).
- In the next session 11 values read back unchanged right after being set: every Lumen value,
  front-layer reflections, short-range AO, mesh SDF tracing and the fog grid. They're Ref
  variables (pointing at an engine global): off the game thread UE's OnCVarChange doesn't copy
  the value. VSM, streaming, anisotropy, Nanite and post values (data variables) did change.
  So 0.2.0's "Cinematic +18%" (77.6 -> 65.7 fps) measured only part of the preset.
- HDR worked: user settings peak 3700 nits, UI 250 nits.

0.3.0 is rebuilt on TrueColorsTweaks: every Set runs on the game thread from a hooked
CallAllConsoleVariableSinks (vtable copy on the console manager), the overlay reads a
published snapshot. cvars.hpp is TrueColors' with FindConsoleVariable +0xb8 and SetBy
0x09/0x0A. A launch guard keeps hardware ray tracing off at start until it has been seen running
for 15 s (`HwRtConfirmed` in ReShade.ini).

## 0.3.0 in game (2026-11-01)

- All values take now (only `r.Lumen.Reflections.MaxRoughnessToTraceClamp` doesn't exist in
  5.3; removed). Hardware ray tracing switched on from the game thread without locking up: 701
  acceleration structure builds and 8 DispatchRays per second. The user: "looked really bad"
  (hit lighting, UE's standalone RT translucency and RT shadows were on at the same time). 0.3.1
  drops the standalone RT shadows and translucency.
- Cinematic with every value applied: Game 78.0 fps, Cinematic 39.2 fps (+99% frame time). Needs
  "Measure each option" to see which parts cost most.
- HDR peak: `r.HDR.Display.MaxLuminance` was set back to 1000 about once a second (the add-on put
  3700 back 100 times in two minutes) while its priority stayed 0x0A: the engine's HDR sink
  re-applies GameUserSettings `HDRDisplayOutputNits` (1000) whenever a console variable changed.
  0.3.1 puts the peak back right after the sinks run (before the frame renders) and writes the
  peak to `HDRDisplayOutputNits` in GameUserSettings.ini (at load, on slider release and at unload);
  the user's file was set to 3700 by hand on 2026-11-01 (backup `GameUserSettings.ini.bak-nobodytweaks`).

## 0.3.1 in game (2026-11-01)

- The HDR sink kept asking for 1000 even with `HDRDisplayOutputNits=3700` in the ini (the game
  doesn't use it while its own HDR is off), and re-setting the peak with Set after the sinks
  flagged a change, so the sink ran every frame (7000 times in two minutes). 0.3.2 writes the
  peak straight into the variable's data (both thread copies) after the sinks, which doesn't
  flag a change, and no longer writes the ini.
- Dragging a slider re-applied all 29 values every frame; 0.3.2's Apply skips values already in
  force.
- Hardware ray tracing alone and with hit lighting ran (753 / 1820 structure builds per second);
  the user's verdict on the look is pending. Cinematic measured again: Game 12.47 ms, Cinematic
  24.98 ms (+100%).

## 0.3.2 crashed (2026-11-01 09:48)

Access violation on the game thread in `FSlateRHIResourceManager::FindOrCreateDynamicTextureResource`
(SlateRHIResourceManager.cpp:846), 0.3 s after the swap chain switched to HDR10, calling through a
heap address: memory corruption. The only new thing in 0.3.2 was writing the peak straight into
`r.HDR.Display.MaxLuminance`'s data (8 bytes at AsVariableInt's pointer, +0x48), so that's the
suspect. 0.3.3 drops the direct write: the peak is Set after the sinks and the sinks flag
(FConsoleManager +0xe0, read from CallAllConsoleVariableSinks' code before hooking) is cleared
again if only our Set raised it.

## 0.3.3 in game (2026-11-01 09:50): no crash, per-option costs

No crash at the HDR switch; the HDR sink fired three times at start and then stopped (the flag
clearing works). "Measure each option" (RTX 5080, 4K, DLSS, HDR, one scene; Game 12.43 ms):

| option | frame time |
|---|---|
| GI detail Cinematic | +60.1% |
| Fog Finest (4 px) | +18.1% |
| Shadows Cinematic | +17.6% |
| Reflections roughness 1.0 / 0.8 / 0.6 | +6.2% / +5.4% / +5.4% |
| Fog Fine (6 px) | +4.1% |
| GI detail High | +2.4% |
| Detailed tracing (mesh SDFs) | +1.8% |
| Glass reflections | +0.8% |
| Contact shadows, texture memory, anisotropy, mip bias, sharp shadows, Nanite, draw distance | within +-1.3% |
| Bloom off | -9.3% |

0.3.4 adds Ultra+ (the cheap ones: GI High, detailed tracing, contact shadows, glass, roughness
0.6, sharp shadows, fine fog, 8 GB, 16x, Nanite, draw distance; about +15% expected) between Game
and Cinematic (+100% measured).

## User verdicts (2026-11-01)

- Hardware ray tracing (Lumen HWRT, with or without hit lighting, no standalone RT shadows or
  translucency): "looked a lot better". Not in any preset.
- FSR 3 frame generation works once FSR is chosen as the upscaler in the game's menu (not with
  DLSS upscaling).
- The game has no NVIDIA driver profile, so Smooth Motion isn't one click away for players.
  DLSS frame generation would need a separate add-on with its own Streamline (the game ships no
  Streamline or DLSS-G), like DoubleExposureDLSS (UE 5.2: replaces TSR with DLSS/FSR and adds
  DLSS-G). That route also gets around the game's DLSS plugin only allowing presets A-G.

## Preset costs (0.3.4, 2026-11-01)

RTX 5080, 4K, DLSS, HDR, one scene: Game 12.84 ms (77.9 fps), Ultra+ 14.48 ms (69.0 fps, +13%),
Cinematic 25.26 ms (39.6 fps, +97%). 0.3.5 shows these in the picker.
