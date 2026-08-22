# Texture Downscaler

SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

A .dds file holds a stack of images, each one half the size of the one above. This plugin skips the top of the stack and hands the game the level below, so a 4096 texture loaded at 1024 takes a sixteenth of the memory. Nothing is resampled and no file on disk is touched.

Needs Skyrim SE, AE or VR, [SKSE64](https://skse.silverlock.org/) and [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444). On VR, the [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101) is required, and the plugin is untested there.

## Presets

The installer offers four starting points. Each one is only a settings file, so nothing stops you editing it afterwards.

| Preset | Diffuse | Other types |
|---|---|---|
| Quality | 2048 | 2048 |
| Balanced | 1024 | 1024 |
| Performance | 1024 | 512 |
| Custom | full size | full size |

All three working presets leave the interface, LOD, sky, terrain, face tints and fur shells alone. Landscape, trees and dragons keep their diffuse textures at full size and only have their normal maps capped.

Quality also leaves armour, clothing, PBR material maps and character textures untouched. Performance drops every type other than diffuse to 512 and caps character textures at 1024. Custom does nothing until you set your own limits.

## Settings

`Data/SKSE/Plugins/TextureDownscaler.ini`

Each texture gets a maximum size according to its type. `0` leaves a type at full size.

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

Normal maps and height maps hold up better at low resolution than diffuse does, so they are the usual place to save memory.

### Folders

A folder rule overrides the type limit. It matches anywhere in the path, file name included.

```ini
[Folders]
_shell=0
\interface\=0
\actors\character\=2048
```

Since the match is plain text, a rule doesn't have to name a real folder. Skyrim has no doors folder, but `door` catches every door texture wherever it lives, and `_shell` reaches the fur shell textures of every mod that adds them.

Put a type in front and the rule only covers that type. Rules are read from top to bottom and the first match wins, so the narrow rule goes above the broad one:

```ini
Normal:door=1024
door=0
```

Those two keep doors at full size except for their normal maps.

`LogLevel=1` writes a line for each texture loaded at a reduced size, with its name and the type it was read as.

## In-game menu

With [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) installed, the settings can also be edited in game, under TextureDownscaler. Changes apply to the next texture that loads and can be written back to the ini from any page.

The menu is optional. Without it the plugin behaves exactly as it did before, and none of its code runs.

`TrackUsedFolders=1` counts the textures your game loads, per folder, and fills the `Used` column. It is off by default because it costs a little on every texture load, and the page can switch it on and off.

`Browse folders` lists every folder under `Data\textures` along with the ones the base game keeps in its archives, and shows how many textures have been loaded from each. It is the quickest way to see where a rule is worth adding for your own load order.

## Notes

Render targets, depth buffers, texture arrays, cubemaps and textures with no mips are skipped.

File names come from the engine's texture loader, which is hooked so that the texture being built is known by the time D3D is called.

## Building

Needs Visual Studio 2022, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, and [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG/tree/ng) cloned next to this repo's folder (i.e. `../CommonLibSSE-NG`). From an x64 Native Tools Command Prompt:

```
cmake --preset release
cmake --build build/release
```

Set `SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` and the build deploys the plugin for you.

## License

[GPL-3.0-or-later](LICENSE) WITH [Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS.md).