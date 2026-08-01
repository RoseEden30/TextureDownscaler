// TextureDownscaler
//
// Cuts video memory usage by dropping the largest mip levels of a texture when
// it's created. Nothing gets resampled — a .dds already ships a full pyramid of
// pre-scaled images, so we just ask D3D11 for a shorter chain and hand it the
// matching data. Files on disk are never touched.

namespace {
    // D3D11 wants the top mip of a block-compressed texture to be a multiple of
    // 4, so nothing can go below that.
    constexpr std::uint32_t kMinDimension = 4;

    // MaxDownscaleFactor tops out at 16, i.e. four halvings.
    constexpr std::uint32_t kMaxSkipLevels = 4;

    struct Config {
        bool          enabled        = true;
        std::uint32_t maxTextureSize = 1024;
        std::uint32_t maxSkipLevels  = kMaxSkipLevels;
        std::uint32_t logLevel       = 2;
    };

    Config g_config;

    // Touched from the render thread, read when a save is loaded.
    std::atomic<std::uint64_t> g_downscaledCount{0};
    std::atomic<std::uint64_t> g_savedBytes{0};

    void InitLogger() {
        auto dir = SKSE::log::log_directory();
        if (!dir) return;

        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        *dir /= std::format("{}.log", plugin->GetName());

        auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(dir->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));

        // Flushing every line would make the debug level unusable: the hooks
        // trace from the render thread, one line per texture.
        logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    void SetLogLevel(std::uint32_t level) {
        switch (level) {
            case 0:  spdlog::set_level(spdlog::level::trace);    break;
            case 1:  spdlog::set_level(spdlog::level::debug);    break;
            case 2:  spdlog::set_level(spdlog::level::info);     break;
            case 3:  spdlog::set_level(spdlog::level::warn);     break;
            case 4:  spdlog::set_level(spdlog::level::err);      break;
            case 5:  spdlog::set_level(spdlog::level::critical); break;
            default: spdlog::set_level(spdlog::level::info);     break;
        }
    }

    std::filesystem::path GetIniPath() {
        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        return std::filesystem::path(std::format("Data/SKSE/Plugins/{}.ini", plugin->GetName()));
    }

    // Kept in sync by hand with the .ini shipped alongside the plugin. This
    // copy is what gets written back when a user deletes theirs.
    constexpr std::string_view kDefaultIni = R"INI([General]

; 0 turns the plugin off.
Enabled=1

; Textures bigger than this get shrunk. Smaller ones are left alone.
MaxTextureSize=1024

; Safety net: how far a texture may shrink at most.
; 2 = half at most, 4 = a quarter at most, 1 = nothing shrinks.
MaxDownscaleFactor=16

; 0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Fatal
; 1 logs every texture and costs performance.
LogLevel=2
)INI";

    // Only ever writes when there's nothing there, so a user's edited settings
    // survive a plugin update.
    void WriteDefaultIni(const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) || ec) return;

        std::ofstream file(path);
        if (!file) {
            SKSE::log::warn("Can't write {}", path.string());
            return;
        }

        file << kDefaultIni;
        SKSE::log::info("Created {}", path.string());
    }

    // Zero switches downscaling off, anything else is pulled up to the 4 pixel
    // floor.
    std::uint32_t ReadSizeLimit(const CSimpleIniA& ini) {
        auto value = static_cast<std::uint32_t>(
            ini.GetLongValue("General", "MaxTextureSize", 1024));

        if (value != 0 && value < kMinDimension) {
            SKSE::log::warn("MaxTextureSize={} is too small, using {}", value, kMinDimension);
            value = kMinDimension;
        }
        return value;
    }

    // A factor of 4 means two halvings. Anything that isn't a power of two gets
    // rounded down, anything above 16 gets clamped.
    std::uint32_t ReadSkipLevels(const CSimpleIniA& ini) {
        const auto factor = static_cast<std::uint32_t>(
            ini.GetLongValue("General", "MaxDownscaleFactor", 16));

        if (factor < 2) {
            if (factor == 0)
                SKSE::log::warn("MaxDownscaleFactor=0 read as 1");
            return 0;
        }

        std::uint32_t levels    = 0;
        std::uint32_t remaining = factor;
        while (remaining >= 2 && levels < kMaxSkipLevels) {
            remaining >>= 1;
            ++levels;
        }

        const std::uint32_t effective = 1u << levels;
        if (effective != factor)
            SKSE::log::warn("MaxDownscaleFactor={} isn't a power of two, using {}", factor, effective);

        return levels;
    }

    void LoadConfig() {
        const auto path = GetIniPath();
        WriteDefaultIni(path);

        CSimpleIniA ini;
        ini.SetUnicode();

        if (ini.LoadFile(path.string().c_str()) < 0) {
            SKSE::log::warn("No {}, using defaults", path.string());
        } else {
            g_config.enabled        = ini.GetBoolValue("General", "Enabled", true);
            g_config.maxTextureSize = ReadSizeLimit(ini);
            g_config.maxSkipLevels  = ReadSkipLevels(ini);
            g_config.logLevel       = static_cast<std::uint32_t>(
                ini.GetLongValue("General", "LogLevel", 2));
        }

        SetLogLevel(g_config.logLevel);

        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        SKSE::log::info("{} v{}", plugin->GetName(), plugin->GetVersion().string("."sv));
        SKSE::log::info("Enabled={} MaxTextureSize={} MaxDownscaleFactor={} LogLevel={}",
                        g_config.enabled, g_config.maxTextureSize,
                        1u << g_config.maxSkipLevels, g_config.logLevel);

        if (g_config.enabled && g_config.maxTextureSize == 0)
            SKSE::log::warn("MaxTextureSize=0, nothing to do");
    }

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

    // Short names for the formats Skyrim actually ships, so the debug log stays
    // readable. Anything else falls back to the raw DXGI value.
    std::string FormatName(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_BC1_TYPELESS:
            case DXGI_FORMAT_BC1_UNORM:           return "BC1";
            case DXGI_FORMAT_BC1_UNORM_SRGB:      return "BC1s";
            case DXGI_FORMAT_BC2_TYPELESS:
            case DXGI_FORMAT_BC2_UNORM:           return "BC2";
            case DXGI_FORMAT_BC2_UNORM_SRGB:      return "BC2s";
            case DXGI_FORMAT_BC3_TYPELESS:
            case DXGI_FORMAT_BC3_UNORM:           return "BC3";
            case DXGI_FORMAT_BC3_UNORM_SRGB:      return "BC3s";
            case DXGI_FORMAT_BC4_TYPELESS:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:           return "BC4";
            case DXGI_FORMAT_BC5_TYPELESS:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC5_SNORM:           return "BC5";
            case DXGI_FORMAT_BC6H_TYPELESS:
            case DXGI_FORMAT_BC6H_UF16:
            case DXGI_FORMAT_BC6H_SF16:           return "BC6H";
            case DXGI_FORMAT_BC7_TYPELESS:
            case DXGI_FORMAT_BC7_UNORM:           return "BC7";
            case DXGI_FORMAT_BC7_UNORM_SRGB:      return "BC7s";
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM:      return "RGBA8";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8s";
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:      return "BGRA8";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8s";
            case DXGI_FORMAT_B8G8R8X8_UNORM:      return "BGRX8";
            default:                              return std::format("DXGI_{}", static_cast<int>(format));
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

    // How many mip levels to drop off the front of the chain. Zero means leave
    // the texture alone.
    std::uint32_t ComputeSkip(const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data) {
        // No initial data means there's no pyramid to pick from: the game fills
        // the texture later and expects the size it asked for.
        if (!g_config.enabled || !data) return 0;

        // Arrays and cubemaps would need every slice remapped, and a single mip
        // leaves nothing to drop.
        if (desc.ArraySize != 1 || desc.MipLevels <= 1) return 0;

        // Render targets and compute outputs are engine surfaces whose size is
        // assumed elsewhere in the frame.
        constexpr UINT kRejectedBinds = D3D11_BIND_RENDER_TARGET |
                                        D3D11_BIND_DEPTH_STENCIL |
                                        D3D11_BIND_UNORDERED_ACCESS;
        if (desc.BindFlags & kRejectedBinds) return 0;

        // A shared texture gets described again by an outside consumer that
        // won't see the reduction, and generated mips mean a writable surface
        // rather than authored content.
        constexpr UINT kRejectedMisc = D3D11_RESOURCE_MISC_SHARED |
                                       D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                       D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (desc.MiscFlags & kRejectedMisc) return 0;

        // Dynamic and staging textures get rewritten at runtime by code that
        // works out offsets from the original size.
        if (desc.Usage != D3D11_USAGE_DEFAULT && desc.Usage != D3D11_USAGE_IMMUTABLE) return 0;

        const std::uint32_t limit = g_config.maxTextureSize;
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

        // MaxDownscaleFactor only caps a reduction the size limit already
        // decided on. It never starts one.
        if (skip > g_config.maxSkipLevels) skip = g_config.maxSkipLevels;

        // The level we keep has to stay a multiple of 4 on both axes.
        // Truncating instead would put the description out of sync with the
        // data, whose pitch always describes the original mip.
        if (IsBlockCompressed(desc.Format)) {
            while (skip > 0 && (((desc.Width >> skip) & 3u) || ((desc.Height >> skip) & 3u)))
                --skip;
        }

        return skip;
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
        if (!desc) return g_originalCreateTexture2D(self, desc, data, out);

        const auto skip = ComputeSkip(*desc, data);
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
            SKSE::log::warn("{}x{} rejected (0x{:08X}), left as-is",
                            desc->Width, desc->Height, static_cast<std::uint32_t>(hr));
            return g_originalCreateTexture2D(self, desc, data, out);
        }

        g_downscaledCount.fetch_add(1, std::memory_order_relaxed);
        g_savedBytes.fetch_add(saved, std::memory_order_relaxed);
        SKSE::log::debug("{} {}x{} -> {}x{} {} KB",
                         FormatName(desc->Format), desc->Width, desc->Height,
                         reduced.Width, reduced.Height, saved / 1024);
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
        D3D11_SHADER_RESOURCE_VIEW_DESC clamped = *desc;
        bool                            changed = false;

        if (clamped.Texture2D.MostDetailedMip >= textureDesc.MipLevels) {
            clamped.Texture2D.MostDetailedMip = textureDesc.MipLevels - 1;
            changed                           = true;
        }
        if (clamped.Texture2D.MipLevels != static_cast<UINT>(-1) &&
            clamped.Texture2D.MostDetailedMip + clamped.Texture2D.MipLevels > textureDesc.MipLevels) {
            clamped.Texture2D.MipLevels = textureDesc.MipLevels - clamped.Texture2D.MostDetailedMip;
            changed                     = true;
        }

        if (!changed) return g_originalCreateSRV(self, resource, desc, out);
        return g_originalCreateSRV(self, resource, &clamped, out);
    }

    bool PatchSlot(ID3D11Device* device, std::size_t slot, void* hook, void** original) {
        auto** vtable = *reinterpret_cast<void***>(device);
        void** entry  = &vtable[slot];

        DWORD previousProtection = 0;
        if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &previousProtection)) return false;

        *original = InterlockedExchangePointer(static_cast<void* volatile*>(entry), hook);
        VirtualProtect(entry, sizeof(void*), previousProtection, &previousProtection);

        return *original != nullptr;
    }

    void InstallHooks() {
        if (!g_config.enabled) {
            SKSE::log::info("Disabled in the settings");
            return;
        }

        // VR lays BSRenderManager out differently, so the flat runtime accessor
        // would hand back a garbage device.
        if (REL::Module::IsVR()) {
            SKSE::log::error("Skyrim VR isn't supported");
            return;
        }

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
        // otherwise a view could be built against a chain that no longer
        // matches its description.
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
            void* discarded = nullptr;
            PatchSlot(device, kSlot_CreateShaderResourceView,
                      reinterpret_cast<void*>(g_originalCreateSRV), &discarded);
            g_originalCreateSRV = nullptr;
            return;
        }

        SKSE::log::info("Hooks installed");
    }

    void LogSummary() {
        const auto count = g_downscaledCount.load(std::memory_order_relaxed);
        if (count == 0) {
            SKSE::log::info("Nothing downscaled yet");
            return;
        }

        const auto megabytes = static_cast<double>(g_savedBytes.load(std::memory_order_relaxed)) /
                               (1024.0 * 1024.0);
        SKSE::log::info("{} textures downscaled, ~{:.1f} MB saved", count, megabytes);
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
