// TextureDownscaler
//
// Cuts video memory usage by dropping the largest mip levels of a texture when
// it's created. Nothing gets resampled — a .dds already ships a full pyramid of
// pre-scaled images, so we just ask D3D11 for a shorter chain and hand it the
// matching data. Files on disk are never touched.
//
// Limits are set per texture type, which means the file name has to be known,
// and D3D11 never sees one. The engine hands the NiSourceTexture to its texture
// loader before any D3D resource exists, so that call is hooked and the object
// kept aside for the D3D hook to ask where it came from. Textures created
// outside that path, by another plugin, stay unidentified and get the Diffuse
// limit. The log says how many.

#include "Config.h"
#include "Hooks.h"
#include "Logging.h"
#include "TextureFolders.h"
#include "UI.h"

namespace {
    // Touched from the render thread, read when a save is loaded.
    std::atomic<std::uint64_t> g_reducedCount{0};
    std::atomic<std::uint64_t> g_savedBytes{0};

    // How name resolution went, reported at the end of the log.
    struct NameStats {
        std::atomic<std::uint64_t> resolved{0};
        std::atomic<std::uint64_t> fromTexture{0};
        std::atomic<std::uint64_t> noTexture{0};
        std::atomic<std::uint64_t> unnamed{0};
        std::atomic<std::uint64_t> skippedStream{0};
    };

    NameStats g_names;

    bool IsBlockCompressed(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_BC1_TYPELESS:
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC2_TYPELESS:
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_TYPELESS:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC4_TYPELESS:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:
            case DXGI_FORMAT_BC5_TYPELESS:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC5_SNORM:
            case DXGI_FORMAT_BC6H_TYPELESS:
            case DXGI_FORMAT_BC6H_UF16:
            case DXGI_FORMAT_BC6H_SF16:
            case DXGI_FORMAT_BC7_TYPELESS:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return true;
            default:
                return false;
        }
    }

    std::uint32_t BitsPerPixel(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_BC1_TYPELESS:
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC4_TYPELESS:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM:
                return 64;
            case DXGI_FORMAT_R8_TYPELESS:
            case DXGI_FORMAT_R8_UNORM:
                return 8;
            default:
                // Every other BCn format is 8 bpp, and the usual uncompressed
                // ones are 32.
                return IsBlockCompressed(format) ? 8 : 32;
        }
    }

    // Only used for the "memory saved" figure in the log. BCn levels are padded
    // out to whole 4x4 blocks.
    std::uint64_t EstimateLevelBytes(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height) {
        if (IsBlockCompressed(format)) {
            const std::uint64_t blocksX   = (width + 3u) / 4u;
            const std::uint64_t blocksY   = (height + 3u) / 4u;
            const std::uint64_t blockSize = BitsPerPixel(format) == 4 ? 8u : 16u;
            return blocksX * blocksY * blockSize;
        }
        return (static_cast<std::uint64_t>(width) * height * BitsPerPixel(format)) / 8u;
    }

    std::uint64_t EstimateSavedBytes(const D3D11_TEXTURE2D_DESC& desc, std::uint32_t skip) {
        std::uint64_t total = 0;
        for (std::uint32_t level = 0; level < skip; ++level)
            total += EstimateLevelBytes(desc.Format, desc.Width >> level, desc.Height >> level);
        return total;
    }

    // Guarded read: the values examined on the stack are arbitrary, and
    // following one can land on a page that isn't mapped.
    __declspec(noinline) bool TryReadPointer(std::uintptr_t address, std::uintptr_t& out) {
        __try {
            out = *reinterpret_cast<const std::uintptr_t*>(address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Engine vtables sit in the game image, which is enough to tell a real
    // object from a heap value that looks like a pointer.
    bool InGameImage(std::uintptr_t address) {
        struct Range {
            std::uintptr_t low  = 0;
            std::uintptr_t high = 0;
        };

        static const Range image = [] {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
            if (!dos) return Range{};

            const auto  base = reinterpret_cast<std::uintptr_t>(dos);
            const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

            return Range{ base, base + nt->OptionalHeader.SizeOfImage };
        }();

        return address >= image.low && address < image.high;
    }

    // The name is read while the loader is still running, so the stream is live.
    // The guard stays because another plugin can send anything through the same
    // slot. Its own function because a scope holding __except can't unwind
    // objects.
    __declspec(noinline) void TryGetName(std::uintptr_t     entry,
                                         const void*        stream,
                                         RE::BSFixedString& name) {
        using DoGetName_t = bool (*)(const void*, RE::BSFixedString&);

        __try {
            reinterpret_cast<DoGetName_t>(entry)(stream, name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Same reason: reading a BSFixedString walks back from its data pointer
    // into a pool header, and its owner isn't proven alive.
    __declspec(noinline) bool TryReadText(const RE::BSFixedString& string,
                                          std::string_view&        out) {
        __try {
            const char* text = string.c_str();
            if (!text || *text == '\0') return false;

            out = std::string_view(text);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // BSResource::Stream::DoGetName, virtual number 10.
    RE::BSFixedString GetStreamName(const void* stream) {
        RE::BSFixedString name;
        if (!stream) return name;

        std::uintptr_t vtable = 0;
        if (!TryReadPointer(reinterpret_cast<std::uintptr_t>(stream), vtable)) return name;

        // Two comparisons, and they keep the call below from jumping into
        // whatever a dead object left at this offset.
        if (!InGameImage(vtable)) {
            g_names.skippedStream.fetch_add(1, std::memory_order_relaxed);
            return name;
        }

        std::uintptr_t entry = 0;
        if (!TryReadPointer(vtable + 10 * sizeof(void*), entry) || entry == 0) return name;

        TryGetName(entry, stream, name);

        return name;
    }

    // BSShaderResourceManager hands the texture to the loader before the D3D
    // resource exists, which is the only place on this path where the object can
    // be had for certain. Engine Fixes replaces the same slot, so whatever sits
    // there is chained into rather than assumed to be the engine's.
    constexpr std::size_t kSlot_LoadTexture = 26;

    using LoadTexture_t = void (*)(void*, RE::NiSourceTexture*);

    LoadTexture_t g_originalLoadTexture = nullptr;

    // Read from the D3D hook, which runs below this call. Loads nest, hence the
    // save and restore rather than a clear.
    thread_local RE::NiSourceTexture* t_loadingTexture = nullptr;

    void Hook_LoadTexture(void* self, RE::NiSourceTexture* texture) {
        RE::NiSourceTexture* previous = t_loadingTexture;
        t_loadingTexture              = texture;

        g_originalLoadTexture(self, texture);

        t_loadingTexture = previous;
    }

    // Holds the name alive for as long as the caller needs it, whichever source
    // it came from.
    struct TextureName {
        RE::BSFixedString owned;     // from the stream
        std::string_view  borrowed;  // from the object, valid while it lives

        std::string_view view() const {
            const std::string_view fromStream{owned.c_str()};
            return fromStream.empty() ? borrowed : fromStream;
        }
    };

    TextureName ResolveTextureName() {
        TextureName result;

        auto* texture = t_loadingTexture;
        if (!texture) {
            g_names.noTexture.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        // The stream the texture reads from knows the file it came from.
        result.owned = GetStreamName(texture->unk40);

        if (!result.view().empty()) {
            g_names.resolved.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        // Streams reading out of an archive often refuse to name themselves, and
        // the texture carries the path in that case.
        if (TryReadText(texture->name, result.borrowed)) {
            g_names.resolved.fetch_add(1, std::memory_order_relaxed);
            g_names.fromTexture.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        g_names.unnamed.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // Suffix to category, following the BSShaderTextureSet slots. No two
    // entries can match the same name: a suffix only counts when the underscore
    // lines up, so "_msn" is never read as "_n".
    struct SuffixEntry {
        std::string_view suffix;
        Category         category;
    };

    constexpr std::array kSuffixes{
        SuffixEntry{"_rmaos", Category::Material},
        SuffixEntry{"_msn", Category::Normal},
        SuffixEntry{"_em", Category::Mask},
        SuffixEntry{"_sk", Category::Mask},
        SuffixEntry{"_n", Category::Normal},
        SuffixEntry{"_p", Category::Parallax},
        SuffixEntry{"_g", Category::Glow},
        SuffixEntry{"_m", Category::Mask},
        SuffixEntry{"_s", Category::Mask},
        SuffixEntry{"_b", Category::Mask},
    };

    // Takes the name already folded, so the suffix and rule comparisons below
    // are plain byte matches.
    Category CategoryOf(std::string_view name) {
        auto stem = name;

        const auto slash = stem.find_last_of('\\');
        if (slash != std::string_view::npos) stem = stem.substr(slash + 1);

        const auto dot = stem.rfind('.');
        if (dot != std::string_view::npos) stem = stem.substr(0, dot);

        for (const auto& entry : kSuffixes)
            if (stem.size() > entry.suffix.size() && stem.ends_with(entry.suffix))
                return entry.category;

        return Category::Diffuse;
    }

    // A folder rule wins over the type: it's the more specific of the two. The
    // type check comes first, it's cheaper than the substring search.
    std::uint32_t LimitFor(const Config& config, std::string_view name, Category category) {
        for (const auto& folder : config.folders) {
            if (folder.category && *folder.category != category) continue;
            if (Contains(name, folder.folder)) return folder.maxSize;
        }

        return config.maxSize[static_cast<std::size_t>(category)];
    }

    // How many mip levels to drop off the front of the chain. Zero means leave
    // the texture alone.
    std::uint32_t ComputeSkip(const Config&               config,
                              const D3D11_TEXTURE2D_DESC& desc,
                              std::string_view            name,
                              Category                    category) {
        const auto limit = LimitFor(config, name, category);
        if (limit == 0) return 0;
        if (desc.Width <= limit && desc.Height <= limit) return 0;

        std::uint32_t skip = 0;
        std::uint32_t w    = desc.Width;
        std::uint32_t h    = desc.Height;

        while ((w > limit || h > limit) &&
               skip + 1 < desc.MipLevels &&
               (w >> 1) >= kMinDimension &&
               (h >> 1) >= kMinDimension) {
            w >>= 1;
            h >>= 1;
            ++skip;
        }

        // The level we keep has to stay a multiple of 4 on both axes.
        // Truncating instead would put the description out of sync with the
        // data, whose pitch always describes the original mip.
        if (IsBlockCompressed(desc.Format)) {
            while (skip > 0 && (((desc.Width >> skip) & 3u) || ((desc.Height >> skip) & 3u)))
                --skip;
        }

        return skip;
    }

    // Everything that can be decided from the description alone, before paying
    // for a name.
    // Whether the texture came out of a file, as opposed to being a surface the
    // engine draws into. Only these carry a name worth resolving.
    bool IsFromFile(const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data) {
        // No initial data means there's no pyramid to pick from: the game fills
        // the texture later and expects the size it asked for.
        if (!g_enabled.load(std::memory_order_relaxed) || !data) return false;

        // Arrays and cubemaps would need every slice remapped, and a single mip
        // leaves nothing to drop.
        if (desc.ArraySize != 1 || desc.MipLevels <= 1) return false;

        // Render targets and compute outputs are engine surfaces whose size is
        // assumed elsewhere in the frame.
        constexpr UINT kRejectedBinds = D3D11_BIND_RENDER_TARGET |
                                        D3D11_BIND_DEPTH_STENCIL |
                                        D3D11_BIND_UNORDERED_ACCESS;
        if (desc.BindFlags & kRejectedBinds) return false;

        // A shared texture gets described again by an outside consumer that
        // won't see the reduction, and generated mips mean a writable surface
        // rather than authored content.
        constexpr UINT kRejectedMisc = D3D11_RESOURCE_MISC_SHARED |
                                       D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                       D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (desc.MiscFlags & kRejectedMisc) return false;

        // A texture the CPU can still write to gets rewritten at runtime by code
        // that works out offsets from the size it was created at.
        if (desc.CPUAccessFlags != 0) return false;

        // Dynamic and staging textures get rewritten at runtime by code that
        // works out offsets from the original size.
        return desc.Usage == D3D11_USAGE_DEFAULT || desc.Usage == D3D11_USAGE_IMMUTABLE;
    }

    // Under the smallest limit in the settings, no rule can have a say.
    bool WorthReducing(const D3D11_TEXTURE2D_DESC& desc) {
        const auto smallest = g_smallestLimit.load(std::memory_order_relaxed);
        if (smallest == 0) return false;
        return desc.Width > smallest || desc.Height > smallest;
    }

    // ID3D11Device vtable slots.
    constexpr std::size_t kSlot_CreateTexture2D          = 5;
    constexpr std::size_t kSlot_CreateShaderResourceView = 7;

    using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                          const D3D11_TEXTURE2D_DESC*,
                                                          const D3D11_SUBRESOURCE_DATA*,
                                                          ID3D11Texture2D**);
    using CreateSRV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                    ID3D11Resource*,
                                                    const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                    ID3D11ShaderResourceView**);

    CreateTexture2D_t g_originalCreateTexture2D = nullptr;
    CreateSRV_t       g_originalCreateSRV       = nullptr;

    HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(ID3D11Device*                 self,
                                                   const D3D11_TEXTURE2D_DESC*   desc,
                                                   const D3D11_SUBRESOURCE_DATA* data,
                                                   ID3D11Texture2D**             out) {
        if (!desc || !IsFromFile(*desc, data))
            return g_originalCreateTexture2D(self, desc, data, out);

        const bool reduce = WorthReducing(*desc);
        const bool track     = g_trackUsedFolders.load(std::memory_order_relaxed);

        // Nothing to decide and nobody to tell, so the name is never looked up.
        // This is the path every texture takes when the menu isn't installed.
        if (!reduce && !track) return g_originalCreateTexture2D(self, desc, data, out);

        const auto resolved = ResolveTextureName();

        // Folded once here. Everything downstream compares against rules that
        // are already folded, so nothing has to fold again per comparison.
        thread_local std::string buffer;
        const auto               name = FoldInto(resolved.view(), buffer);

        // Counted whatever the size, so the menu shows what this load order
        // uses rather than only what the current limits reach.
        if (track) RecordUsedFolder(name);

        if (!reduce) return g_originalCreateTexture2D(self, desc, data, out);

        const auto category = CategoryOf(name);

        // One snapshot for the whole decision, so the rules can't change
        // underneath it.
        const auto config = ActiveConfig();

        const auto skip = ComputeSkip(*config, *desc, name, category);
        if (skip == 0) return g_originalCreateTexture2D(self, desc, data, out);

        D3D11_TEXTURE2D_DESC reduced = *desc;
        reduced.Width     = desc->Width >> skip;
        reduced.Height    = desc->Height >> skip;
        reduced.MipLevels = desc->MipLevels - skip;

        const auto saved = EstimateSavedBytes(*desc, skip);

        // Skipping the same number of entries in the data array keeps every
        // remaining one lined up with the level it now describes.
        const HRESULT hr = g_originalCreateTexture2D(self, &reduced, data + skip, out);

        if (FAILED(hr)) {
            // Something about this texture we didn't account for. Better a full
            // size texture than none at all.
            SKSE::log::warn("D3D refused {} at {}x{} mip {} format {} (0x{:08X}), left as-is",
                            name.empty() ? "<unnamed>" : name,
                            reduced.Width, reduced.Height, reduced.MipLevels,
                            static_cast<std::uint32_t>(desc->Format),
                            static_cast<std::uint32_t>(hr));
            return g_originalCreateTexture2D(self, desc, data, out);
        }

        g_reducedCount.fetch_add(1, std::memory_order_relaxed);
        g_savedBytes.fetch_add(saved, std::memory_order_relaxed);

        SKSE::log::debug("{} {} {}x{} -> {}x{} {} KB",
                         name.empty() ? "<unnamed>" : name,
                         kCategoryNames[static_cast<std::size_t>(category)],
                         desc->Width, desc->Height, reduced.Width, reduced.Height, saved / 1024);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateSRV(ID3D11Device*                          self,
                                             ID3D11Resource*                        resource,
                                             const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                             ID3D11ShaderResourceView**             out) {
        if (!resource || !desc || desc->ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
            return g_originalCreateSRV(self, resource, desc, out);

        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        resource->GetType(&dimension);
        if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            return g_originalCreateSRV(self, resource, desc, out);

        D3D11_TEXTURE2D_DESC textureDesc{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&textureDesc);
        if (textureDesc.MipLevels == 0)
            return g_originalCreateSRV(self, resource, desc, out);

        // Views get described from the size the caller thinks the texture has,
        // so the levels it asks for can run past the chain we actually made.
        // Trimming the count is the normal case for a reduced texture. A first
        // level past the end is not, and worth a word.
        D3D11_SHADER_RESOURCE_VIEW_DESC clamped = *desc;
        bool                            changed = false;

        if (clamped.Texture2D.MostDetailedMip >= textureDesc.MipLevels) {
            SKSE::log::debug("View asked for mip {} of a {} level {}x{} texture",
                             clamped.Texture2D.MostDetailedMip, textureDesc.MipLevels,
                             textureDesc.Width, textureDesc.Height);

            clamped.Texture2D.MostDetailedMip = textureDesc.MipLevels - 1;
            changed                           = true;
        }

        if (clamped.Texture2D.MipLevels != static_cast<UINT>(-1) &&
            clamped.Texture2D.MostDetailedMip + clamped.Texture2D.MipLevels > textureDesc.MipLevels) {
            clamped.Texture2D.MipLevels = textureDesc.MipLevels - clamped.Texture2D.MostDetailedMip;
            changed                     = true;
        }

        if (!changed) return g_originalCreateSRV(self, resource, desc, out);

        const HRESULT hr = g_originalCreateSRV(self, resource, &clamped, out);

        if (FAILED(hr))
            SKSE::log::warn("D3D refused a clamped view on a {}x{} texture (0x{:08X})",
                            textureDesc.Width, textureDesc.Height,
                            static_cast<std::uint32_t>(hr));

        return hr;
    }

    bool PatchSlot(ID3D11Device* device, std::size_t slot, void* hook, void** original) {
        auto** vtable = *reinterpret_cast<void***>(device);
        void** entry  = &vtable[slot];

        DWORD previousProtection = 0;
        if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &previousProtection)) return false;

        // The swap goes live for every thread at once, and the render thread
        // is already creating textures, so what the hook chains into has to be
        // readable first.
        void* previous = *entry;
        *original      = previous;

        void* swapped = InterlockedExchangePointer(static_cast<void* volatile*>(entry), hook);

        // Someone else patched the slot in between, so what we overwrote isn't
        // what we'd chain into. Put theirs back and stay out.
        const bool installed = swapped == previous && previous != nullptr;
        if (!installed) {
            InterlockedExchangePointer(static_cast<void* volatile*>(entry), swapped);
            *original = nullptr;
        }

        VirtualProtect(entry, sizeof(void*), previousProtection, &previousProtection);

        return installed;
    }

    void InstallTextureLoadHook() {
        REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSShaderResourceManager[0] };

        // Read before writing: replacing the slot and only then finding out
        // there was nothing to chain into would leave the hook calling null.
        const auto slot = vtable.address() + kSlot_LoadTexture * sizeof(void*);

        if (*reinterpret_cast<const std::uintptr_t*>(slot) == 0) {
            SKSE::log::warn("Couldn't hook the texture loader, file names won't be known");
            return;
        }

        g_originalLoadTexture = reinterpret_cast<LoadTexture_t>(
            vtable.write_vfunc(kSlot_LoadTexture, Hook_LoadTexture));

        SKSE::log::debug("Texture loader hooked");
    }

    std::atomic<bool> g_hooksInstalled{false};
}

void InstallHooks() {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        SKSE::log::info("Disabled in the settings");
        return;
    }

    // Nothing here is specific to a runtime, but no VR install has been tested.
    if (REL::Module::IsVR()) {
        SKSE::log::warn("Skyrim VR is untested");
    }

    // Live before the first texture reaches the D3D hook, otherwise the
    // early ones have no name to go on.
    InstallTextureLoadHook();

    auto* manager = RE::BSRenderManager::GetSingleton();
    if (!manager) {
        SKSE::log::error("No BSRenderManager");
        return;
    }

    auto* device = manager->GetRuntimeData().forwarder;
    if (!device) {
        SKSE::log::error("No D3D11 device");
        return;
    }

    // The view hook has to be live before the first reduced texture exists,
    // otherwise a view could be built against a chain that no longer matches
    // its description.
    if (!PatchSlot(device, kSlot_CreateShaderResourceView,
                   reinterpret_cast<void*>(&Hook_CreateSRV),
                   reinterpret_cast<void**>(&g_originalCreateSRV))) {
        SKSE::log::error("Couldn't hook CreateShaderResourceView");
        return;
    }

    if (!PatchSlot(device, kSlot_CreateTexture2D,
                   reinterpret_cast<void*>(&Hook_CreateTexture2D),
                   reinterpret_cast<void**>(&g_originalCreateTexture2D))) {
        SKSE::log::error("Couldn't hook CreateTexture2D");

        // Leaving the view hook in place would clamp views for no reason.
        // Left pointing at the real function on purpose: a call already inside
        // the view hook on another thread would otherwise chain into null.
        void* discarded = nullptr;
        PatchSlot(device, kSlot_CreateShaderResourceView,
                  reinterpret_cast<void*>(g_originalCreateSRV), &discarded);
        return;
    }

    g_hooksInstalled.store(true, std::memory_order_release);
    SKSE::log::info("Hooks installed");
}

bool HooksInstalled() { return g_hooksInstalled.load(std::memory_order_acquire); }

namespace {
    void LogNameStats() {
        const auto resolved  = g_names.resolved.load(std::memory_order_relaxed);
        const auto noTexture = g_names.noTexture.load(std::memory_order_relaxed);
        const auto unnamed   = g_names.unnamed.load(std::memory_order_relaxed);
        const auto total     = resolved + noTexture + unnamed;
        if (total == 0) return;

        // The rest fall back to Diffuse, so this is how much of the settings
        // file had a say.
        SKSE::log::info("File name found for {} of {} textures", resolved, total);

        // Nothing named at all means the loader hook never ran: something else
        // took the slot without chaining, and only [Textures] Diffuse applies.
        // Worth saying out loud, since the plugin otherwise looks like it works.
        if (resolved == 0)
            SKSE::log::warn("No file name could be read, every texture was treated as Diffuse");

        const auto fromTexture   = g_names.fromTexture.load(std::memory_order_relaxed);
        const auto skippedStream = g_names.skippedStream.load(std::memory_order_relaxed);

        SKSE::log::debug("Lookup: {} named by the texture rather than its stream, {} created "
                         "outside the loader, {} left unnamed, {} streams rejected",
                         fromTexture, noTexture, unnamed, skippedStream);
    }

    void LogSummary() {
        const auto count = g_reducedCount.load(std::memory_order_relaxed);
        if (count == 0) {
            SKSE::log::info("Nothing loaded at a reduced size yet");
            return;
        }

        const auto megabytes = static_cast<double>(g_savedBytes.load(std::memory_order_relaxed)) /
                               (1024.0 * 1024.0);
        SKSE::log::info("{} textures loaded at a reduced size, ~{:.1f} MB saved", count, megabytes);

        LogNameStats();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    InitLogger();
    SKSE::Init(skse);

    // Read the settings here so the requested log level covers everything that
    // follows, hook installation included.
    LoadConfig();

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        switch (message->type) {
            case SKSE::MessagingInterface::kDataLoaded:
                InstallHooks();
                UI::Register();
                break;
            case SKSE::MessagingInterface::kNewGame:
            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kSaveGame:
                LogSummary();
                break;
            default:
                break;
        }
    });

    return true;
}