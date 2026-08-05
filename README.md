# Texture Downscaler

SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

Textures ship as a stack of images, each half the size of the one above. This plugin skips the top of the stack, so a 4096 texture loaded at 1024 uses a sixteenth of the memory. Nothing is resampled and no file on disk is touched.

Needs Skyrim SE, AE or VR, [SKSE64](https://skse.silverlock.org/) and [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444). On VR, the [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101) is required, and the plugin is untested there.

## Presets

The installer offers four starting points. Each one is just a settings file, so you can edit it afterwards.

| Preset | Diffuse | Other types |
|---|---|---|
| Quality | 2048 | 2048 |
| Balanced | 1024 | 1024 |
| Performance | 1024 | 512 |
| Custom | full size | full size |

All of them leave interface, LOD, sky, terrain, faces, dragons and landscape at full size. Custom does nothing at all until you set your own limits.

Quality also leaves armour, clothing, PBR material maps and character textures untouched. Performance drops every type other than diffuse to 512 and caps character textures at 1024.

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

### Folders

A folder rule overrides the type and matches anywhere in the path.

```ini
[Folders]
\interface\=0
\lod\=0
\actors\character\=2048
```

The match is on the path as text, so a rule doesn't have to name a real folder. Skyrim has no doors folder, but `door` still catches every door texture wherever it lives. Close a rule on both sides when the fragment is short: `\lod` matches `\clutter\lodestone.dds` as well, `\lod\` does not.

Put a type in front and the rule only covers that type. Rules are read from top to bottom and the first one that matches wins, so the narrow rule goes above the broad one:

```ini
Normal:door=1024
door=0
```

Those two keep doors at full size except for their normal maps. A rule that an earlier one already covers can never fire, and gets reported in the log when the settings are read.

`LogLevel=1` writes a line for each texture loaded at a reduced size, with its name and the type it was read as.

## In-game menu

If [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) is installed, the settings are also editable in game, under TextureDownscaler. Changes take effect on the next texture load and can be written back to the ini from any page.

The menu is entirely optional. Without it the plugin behaves exactly as it did before, and none of its code runs.

`TrackUsedFolders=1` counts the textures your game loads, per folder, and fills the `Used` column. It is off by default because it costs a little on every texture load, and it can be switched on and off from the page itself.

`Browse folders` lists every folder under `Data\textures`, along with the ones the base game keeps in its archives, and shows how many textures your game has actually loaded from each. It is the quickest way to find out where a rule is worth adding for your own load order.

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

Built on [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG).

MIT licensed, except `include/SKSEMenuFramework.h`, redistributed unmodified from [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) under LGPL-2.1. See NOTICE.