# Texture Downscaler

SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

Textures ship as a stack of images, each half the size of the one above. This plugin skips the top of the stack, so a 4096 texture loaded at 1024 uses a sixteenth of the memory. Nothing is resampled and no file on disk is touched.

Needs Skyrim SE, AE or VR, [SKSE64](https://skse.silverlock.org/) and [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444). On VR, the [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101) is required, and the plugin is untested there.

## Settings

`Data/SKSE/Plugins/TextureDownscaler.ini`

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

The type comes from the end of the file name:

| Type | Suffixes |
|---|---|
| Normal | `_n` `_msn` |
| Parallax | `_p` |
| Material | `_rmaos` |
| Glow | `_g` |
| Mask | `_m` `_em` `_s` `_sk` `_b` |
| Diffuse | everything else |

Normal maps and height maps hold up better at lower resolution than diffuse does, so they're the usual place to save memory.

A folder rule overrides the type, and matches anywhere in the path:

```ini
[Folders]
\interface\=0
\lod\=0
\actors\character\=2048
\landscape\mountains\=2048
```

Close a rule on both sides where you can: `\lod` matches `\clutter\lodestone.dds` as well, `\lod\` does not.

Set `LogLevel=1` to see how your load order is actually classified.

## Notes

Render targets, depth buffers, texture arrays, cubemaps and textures with no mips are skipped.

File names come from the engine's texture loader, which is hooked so the texture being built is known by the time D3D is called. Nothing runs per frame. Some textures arrive without a name and fall back to the Diffuse limit, so a folder rule won't reach them.

## Building

Needs Visual Studio 2022 and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. From an x64 Native Tools Command Prompt:

```
cmake --preset release
cmake --build build/release
```

Set `SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` and the build deploys the plugin for you.

Built on [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG). MIT licensed.
