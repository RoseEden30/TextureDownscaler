# Texture Downscaler

SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

Textures ship as a stack of images, each half the size of the one above. This plugin skips the top of the stack, so a 4096 texture loaded at 1024 uses a sixteenth of the memory. Nothing is resampled and no file on disk is touched.

Needs Skyrim SE or AE, [SKSE64](https://skse.silverlock.org/) and [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444). No VR.

## Settings

`Data/SKSE/Plugins/TextureDownscaler.ini`

### Texture types

Every texture gets a maximum size according to its type. `0` leaves a type at full size.

```ini
[Textures]
Diffuse=1024
Normal=1024
Parallax=1024
Material=1024
Glow=1024
Mask=1024
```

The type comes from the end of the file name, following the texture slots Skyrim uses:

| Type | Suffixes | Slot |
|---|---|---|
| Normal | `_n` `_msn` | normal map |
| Glow | `_g` | emissive |
| Parallax | `_p` | height map |
| Mask | `_m` `_em` `_s` `_sk` `_b` | reflection mask, subsurface, backlight |
| Material | `_rmaos` | PBR / complex material |
| Diffuse | everything else | base colour |

Normal maps and parallax height maps hold up better at lower resolution than diffuse does, so they're the usual place to save memory. For reference, [VRAMr](https://www.nexusmods.com/skyrimspecialedition/mods/91117) uses 2048 diffuse with 1024 normals and parallax for its quality preset, and 1024 diffuse with 512 for the rest as its performance preset.

Suffixes are a naming convention modders follow, not something the engine enforces. A texture named against the grain counts as Diffuse, and plenty of mods don't suffix at all. Set `LogLevel=1` to see how your load order is actually classified.

### Folders

A folder rule overrides the type, and matches anywhere in the path. Three come set up out of the box:

```ini
[Folders]
interface=0
actors\character=2048
landscape\mountains=2048
```

Menus are drawn pixel for pixel so any reduction shows, faces are what you look at from closest, and mountain cliffs are both the largest textures in the game and the ones you walk right past. A limit of 2048 rather than 0 keeps most of the memory saving: dropping 4096 to 1024 removes two mip levels, and the first accounts for four fifths of the gain.

## Notes

Render targets, depth buffers, texture arrays, cubemaps and textures with no mips are skipped.

File names come from the engine's texture loader. Reading one costs a stack scan, so it only happens for textures large enough for some limit to apply, and only when a texture is created — on a loading screen, or as the game streams new areas in. Nothing runs per frame.

Upgrading from 1.x: `MaxTextureSize` becomes the six entries in `[Textures]`, and `MaxDownscaleFactor` is gone — a target size says everything it said, and the two together were easy to get wrong. `[Suffix]` and `[Path]` become `[Textures]` and `[Folders]`.

## Building

Needs Visual Studio 2022 with the C++ workload, and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing at it.

Build from an **x64 Native Tools Command Prompt for VS 2022**. A plain command prompt picks up the 32 bit `cl.exe` and the configure step fails.

```
cmake --preset release
cmake --build build/release
```

Dependencies come from the registries in `vcpkg-configuration.json` and are fetched on the first configure. CommonLibSSE is built from source, so expect several minutes that one time.

Set `SKYRIM_MODS_FOLDER` to your mod manager's mods folder, or `SKYRIM_FOLDER` to the game install, and every build deploys the dll and the ini for you.

Built on [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG). MIT licensed.
