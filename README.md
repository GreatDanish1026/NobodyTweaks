# NobodyTweaks 0.4.0

Graphics add-on for **Nobody Wants to Die** (PC, Steam), for ReShade with add-on support.
Switches on the game's hidden hardware ray tracing, raises its lighting, shadow and detail
settings beyond what the menu offers, live, from an in-game overlay, and adds HDR (the game has
none).

- **Ray tracing**: the game ships with ray tracing built in but its menu never turns it on.
  Hardware ray traced Lumen lighting and reflections, plus hit lighting for sharper, correctly
  lit reflections.
- **Lighting**: finer Lumen global illumination, detailed tracing, contact shadows, reflections
  on rougher surfaces and on glass, sharper or Cinematic shadows, finer volumetric fog.
- **Detail**: texture memory beyond the game's 2.5 GB, 16x anisotropic filtering, texture
  sharpness, finer geometry, longer draw distance, and switches for film grain, lens flares and
  bloom.
- **HDR**: Unreal's own HDR10 output with a peak brightness slider (400-10,000 nits), and
  optional UI and mid-grey brightness.
- **Quality presets**: *Ultra+* (about 13% slower) and *Cinematic* (about 97% slower), measured
  on an RTX 5080 at 4K with DLSS. They leave ray tracing and HDR alone.
  **Measure preset costs** and **Measure each option** check them on your PC.
- **F8** switches every tweak off and on, to compare.

DLSS (with its newer models), FSR and frame generation are in the companion add-on
**NobodyDLSS**.

Every option starts at the game's own value.

## Requirements

- Nobody Wants to Die for PC (Steam build 14922697 tested). If the game's code doesn't match, the
  add-on changes nothing and says so in the overlay and in `NobodyTweaks.log`.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `detnoir-Win64-Shipping.exe` with DirectX 10/11/12.
- HDR needs Windows HDR on.

## Install

1. Install ReShade with add-on support into `...\Nobody Wants to Die\detnoir\Binaries\Win64`.
2. Copy `NobodyTweaks.addon64` into the same folder.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under NobodyTweaks.

Settings are saved in `ReShade.ini` under `[NobodyTweaks]`. Nothing else is written to disk;
deleting the `.addon64` file removes the mod completely. Hardware ray tracing comes back on at
launch only after it has been seen running once.

## Building

`.\build.ps1` builds `bin\NobodyTweaks.addon64` (needs Visual Studio 2022 with the C++ tools).
`.\build.ps1 -Install` also copies it into the game folder (close the game first). See
`docs/FINDINGS.md` for how the game's settings were found.

## License

MIT (see `LICENSE`). Third-party headers: see `THIRD_PARTY_NOTICES.txt`.
