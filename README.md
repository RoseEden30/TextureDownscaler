# Texture Downscaler

SKSE plugin that lowers Skyrim's video memory usage by loading textures at a smaller size.

A `.dds` file holds a stack of images, each half the size of the one above: 4096, 2048, 1024, and so on. The GPU picks one based on distance, but the whole stack sits in video memory regardless. This plugin skips the top of the stack as each texture is created, so a 4096 texture loaded at 1024 uses a sixteenth of the memory. Nothing is resampled and no file on disk is touched.

Loading into Riverwood on a heavily modded install frees around a gigabyte at default settings.

## Requirements

Skyrim SE or AE, [SKSE64](https://skse.silverlock.org/), and [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444). VR is not supported.

## Settings

`Data/SKSE/Plugins/TextureDownscaler.ini`

**MaxTextureSize** — textures bigger than this get shrunk, smaller ones are left alone. 2048 only touches 4K textures and is hard to notice. 1024 is the default. 512 is for 4 GB cards and looks it.

**MaxDownscaleFactor** — caps how far one texture may shrink, without ever starting a downscale on its own. `2` keeps everything at half its original size or above. The default of 16 never gets in the way.

**LogLevel** — `1` logs every texture, useful when tracking something down but slow. Log lives in `My Games\Skyrim Special Edition\SKSE`.

## Notes

Render targets, depth buffers and anything the engine writes to at runtime are skipped, along with texture arrays, cubemaps and textures with no mips.

Every remaining texture is treated the same. All the plugin sees is a size and a pixel format — no filename, no idea whether it's a face or a rock. Telling them apart would mean hooking the engine's texture loader, which is a much bigger job.

## Building

Needs Visual Studio 2022 and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. From an x64 Native Tools Command Prompt:

```
cmake --preset release
cmake --build build/release
```

Set `SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` and the build deploys the plugin for you.

Built on [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG). MIT licensed.
