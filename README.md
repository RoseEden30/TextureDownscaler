# Texture Downscaler

An SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

## What it does

A `.dds` file doesn't hold one image, it holds a stack of them: 4096, 2048, 1024, and so on down to a single pixel. The GPU picks a level based on distance, but every level sits in video memory the whole time, including the biggest one, even when the object is fifty metres away.

This plugin drops the top of that stack as each texture is created. A 4096 texture loaded at 1024 uses a sixteenth of the memory. Nothing is resampled and no file on disk is touched — the smaller images were already in there, the plugin just skips the big ones.

Loading into Riverwood on a heavily modded install frees around a gigabyte at the default settings.

## Requirements

- Skyrim Special Edition or Anniversary Edition
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

Skyrim VR is not supported. The plugin detects it and stays out of the way.

## Installation

Use a mod manager, or drop `TextureDownscaler.dll` and `TextureDownscaler.ini` into `Data/SKSE/Plugins`.

If the .ini goes missing it gets written again on the next launch. An existing one is never overwritten, so updating won't wipe your settings.

## Settings

**MaxTextureSize** — textures bigger than this get shrunk, smaller ones are left alone. `0` shrinks nothing.

| | |
| --- | --- |
| 2048 | only touches 4K textures, hard to notice |
| 1024 | default, clear savings with some softness up close |
| 512 | for 4 GB cards, visibly blurry |

**MaxDownscaleFactor** — caps how far one texture may shrink. It never starts a downscale on its own. `2` means nothing drops below half its original size, `4` below a quarter, `1` means nothing shrinks. The default of `16` is high enough that it never gets in the way.

**LogLevel** — `2` writes a few lines. `1` logs every texture, which helps when tracking something down but slows the game. The log is in `My Games\Skyrim Special Edition\SKSE`.

## Compatibility

Render targets, depth buffers, shared textures and anything the engine writes to at runtime are skipped, so this stays out of the way of other plugins and shader mods.

Texture mods work as intended. Their files stay on disk exactly as installed, they just load at a lower level, so you can keep a 4K pack and run it at 1024.

## Known limitations

Every texture is treated the same. At the point where the plugin runs, all it sees is a size and a pixel format — no filename, no idea whether it's a face, a rock, or a normal map. Telling them apart would mean hooking the engine's texture loader, which is a much bigger piece of work.

Textures without mips, texture arrays and cubemaps are skipped.

## Building

Needs [Visual Studio 2022](https://visualstudio.microsoft.com/) and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. From an x64 Native Tools Command Prompt:

```
cmake --preset release
cmake --build build/release
```

Set `SKYRIM_MODS_FOLDER` to your mod manager's mods folder, or `SKYRIM_FOLDER` to your Skyrim install, and the build drops the plugin straight in.

Built on [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG).

## License

MIT, see [LICENSE](LICENSE).
