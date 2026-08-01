# Texture Downscaler

SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

Textures ship as a stack of images, each half the size of the one above. This plugin skips the top of the stack, so a 4096 texture loaded at 1024 uses a sixteenth of the memory. Nothing is resampled and no file on disk is touched.

Needs Skyrim SE or AE, [SKSE64](https://skse.silverlock.org/) and [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444). No VR.

## Settings

`Data/SKSE/Plugins/TextureDownscaler.ini`

**MaxTextureSize** — textures bigger than this get shrunk, smaller ones are left alone.

**MaxDownscaleFactor** — caps how far one texture may shrink. Never starts a downscale on its own.

**LogLevel** — `1` logs every texture. Log lives in `My Games\Skyrim Special Edition\SKSE`.

## Notes

Render targets, depth buffers, texture arrays, cubemaps and textures with no mips are skipped.

Everything else is treated the same. All the plugin sees is a size and a pixel format, so there's no way to tell a face from a rock.

## Building

Needs Visual Studio 2022 and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. From an x64 Native Tools Command Prompt:

```
cmake --preset release
cmake --build build/release
```

Set `SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` and the build deploys the plugin for you.

Built on [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG). MIT licensed.
