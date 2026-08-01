// TextureDownscaler
//
// Cuts video memory usage by dropping the largest mip levels of a texture when
// it's created. Nothing gets resampled — a .dds already ships a full pyramid of
// pre-scaled images, so we just ask D3D11 for a shorter chain and hand it the
// matching data. Files on disk are never touched.
//
// Limits are set per texture type, which means the file name has to be known,
// and D3D11 never sees one. The engine's loader keeps the NiSourceTexture alive
// in its call frames down to the D3D call, so the object is picked off the stack
// and asked which file its stream is reading.

namespace {
    // D3D11 wants the top mip of a block-compressed texture to be a multiple of
    // 4, so nothing can go below that.
    constexpr std::uint32_t kMinDimension = 4;

    enum class Category : std::size_t {
        Diffuse,
        Normal,
        Parallax,
        Material,
        Glow,
        Mask,
        Count
    };

    constexpr std::size_t kCategoryCount = static_cast<std::size_t>(Category::Count);

    // INI key per category, in enum order.
    constexpr std::array<std::string_view, kCategoryCount> kCategoryNames{
        "Diffuse", "Normal", "Parallax", "Material", "Glow", "Mask"
    };

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

    struct FolderRule {
        std::string   folder;  // lower case, backslashes
        std::uint32_t maxSize;
    };

    struct Config {
        bool          enabled  = true;
        std::uint32_t logLevel = 2;

        // Zero means that type is left at full size.
        std::array<std::uint32_t, kCategoryCount> maxSize{};

        // Kept in INI order so the first match wins.
        std::vector<FolderRule> folders;

        // The lowest limit any rule could act on. A texture at or below it is
        // out of reach of all of them, so its name never has to be resolved.
        std::uint32_t smallestLimit = 0;
    };

    Config g_config;

    // Touched from the render thread, read when a save is loaded.
    std::atomic<std::uint64_t> g_downscaledCount{0};
    std::atomic<std::uint64_t> g_savedBytes{0};

    // Lower case, and both path separators fold together.
    constexpr char Fold(char character) {
        if (character >= 'A' && character <= 'Z') return static_cast<char>(character + ('a' - 'A'));
        return character == '/' ? '\\' : character;
    }

    constexpr bool EqualsFolded(std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
            if (Fold(left[i]) != Fold(right[i])) return false;
        return true;
    }

    bool ContainsFolded(std::string_view haystack, std::string_view needle) {
        if (needle.empty() || needle.size() > haystack.size()) return false;

        const auto last = haystack.size() - needle.size();
        for (std::size_t start = 0; start <= last; ++start)
            if (EqualsFolded(haystack.substr(start, needle.size()), needle)) return true;
        return false;
    }

    void InitLogger() {
        auto dir = SKSE::log::log_directory();
        if (!dir) return;

        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        *dir /= std::format("{}.log", plugin->GetName());

        auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(dir->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));

        // Flushing every line would make the debug level unusable: the hook
        // traces from the render thread, one line per texture.
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
; 0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Fatal
; 1 logs every texture with its file name and category.
LogLevel=2

[Textures]
; Maximum size per texture type. 0 leaves that type at full size.
; The type is read from the end of the file name:
;   Normal    _n _msn
;   Parallax  _p
;   Material  _rmaos          (PBR / complex material)
;   Glow      _g
;   Mask      _m _em _s _sk _b
;   Diffuse   everything else
;
; Lighter on VRAM:   Diffuse=1024 Normal=512 Parallax=512
; Closer to vanilla: everything at 2048
Diffuse=1024
Normal=1024
Parallax=1024
Material=1024
Glow=1024
Mask=1024

[Folders]
; Maximum size for a folder, matched anywhere in the path. Wins over
; [Textures]. 0 leaves the folder at full size.
interface=0
actors\character=2048
landscape\mountains=2048
;landscape\trees=2048
;plants=2048
;lod=0

; Vanilla folders, for reference. Deeper paths work too, for example
; architecture\whiterun or actors\character\male.
;
; _byoh  actors  architecture  armor  blood  clothes  clutter
; creationclub  critters  dlc01  dlc02  dungeons  effects
; furniture  impactdecals  interface  landscape  lod  plants  puddle
; shadertests  sky  terrain  test  trap  water  weapons
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

    // Zero switches downscaling off for that entry, anything else is pulled up
    // to the 4 pixel floor.
    std::uint32_t ClampSize(long value, std::string_view setting) {
        if (value <= 0) return 0;
        if (value < static_cast<long>(kMinDimension)) {
            SKSE::log::warn("{}={} is too small, using {}", setting, value, kMinDimension);
            return kMinDimension;
        }
        return static_cast<std::uint32_t>(value);
    }

    void ReadFolders(const CSimpleIniA& ini, Config& config) {
        CSimpleIniA::TNamesDepend keys;
        if (!ini.GetAllKeys("Folders", keys)) return;

        keys.sort(CSimpleIniA::Entry::LoadOrder());

        for (const auto& key : keys) {
            std::string folder;
            for (const char* c = key.pItem; *c; ++c) folder.push_back(Fold(*c));
            if (folder.empty()) continue;

            const auto size = ClampSize(ini.GetLongValue("Folders", key.pItem, 0), folder);
            config.folders.emplace_back(std::move(folder), size);
        }
    }

    void LoadConfig() {
        const auto path = GetIniPath();
        WriteDefaultIni(path);

        CSimpleIniA ini;
        ini.SetUnicode();

        const bool loaded = ini.LoadFile(path.string().c_str()) >= 0;

        if (loaded) {
            g_config.enabled = ini.GetBoolValue("General", "Enabled", true);
            g_config.logLevel =
                static_cast<std::uint32_t>(ini.GetLongValue("General", "LogLevel", 2));
        }

        SetLogLevel(g_config.logLevel);

        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        SKSE::log::info("{} v{}", plugin->GetName(), plugin->GetVersion().string("."sv));

        if (!loaded) SKSE::log::warn("No {}, using defaults", path.string());

        for (std::size_t i = 0; i < kCategoryCount; ++i) {
            const std::string key(kCategoryNames[i]);
            g_config.maxSize[i] =
                loaded ? ClampSize(ini.GetLongValue("Textures", key.c_str(), 1024), key) : 1024;
        }

        if (loaded) ReadFolders(ini, g_config);

        // The 1.x settings would be read as "everything at default", which is
        // not what the file says. Worth a word rather than a silent surprise.
        if (loaded && (ini.GetSectionSize("Suffix") >= 0 || ini.GetSectionSize("Path") >= 0))
            SKSE::log::warn("[Suffix] and [Path] are gone, see [Textures] and [Folders]");

        std::string summary;
        for (std::size_t i = 0; i < kCategoryCount; ++i)
            summary += std::format("{}={} ", kCategoryNames[i], g_config.maxSize[i]);

        SKSE::log::info("Enabled={} {}", g_config.enabled, summary);

        for (const auto& folder : g_config.folders)
            SKSE::log::info("{}={}", folder.folder, folder.maxSize);

        // A zero can't start a downscale, so it can't lower the bar for
        // resolving names either.
        for (const auto size : g_config.maxSize)
            if (size != 0 && (g_config.smallestLimit == 0 || size < g_config.smallestLimit))
                g_config.smallestLimit = size;

        for (const auto& folder : g_config.folders)
            if (folder.maxSize != 0 &&
                (g_config.smallestLimit == 0 || folder.maxSize < g_config.smallestLimit))
                g_config.smallestLimit = folder.maxSize;

        if (g_config.enabled && g_config.smallestLimit == 0)
            SKSE::log::warn("Nothing can be downscaled with these settings");
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

    // Where the object was found last time. The loader's frames have a fixed
    // layout, so trying that slot first turns the search into one test.
    std::atomic<std::uint32_t> g_lastStackDepth{0};

    // The engine's texture loader holds a NiSourceTexture in its call frames all
    // the way down to the D3D resource, so it can be found by scanning the stack
    // for a pointer whose vtable is the one we're after. The depth it sits at
    // moves with how the game was built, which is why this stays a search.
    RE::NiSourceTexture* FindSourceTextureOnStack() {
        static const std::uintptr_t wanted = RE::VTABLE_NiSourceTexture[0].address();
        if (wanted == 0) return nullptr;

        const auto* tib      = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
        const auto  stackTop = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        const auto  stackLow = reinterpret_cast<std::uintptr_t>(tib->StackLimit);

        std::uintptr_t marker = 0;
        const auto     origin = reinterpret_cast<std::uintptr_t>(&marker);

        const auto at = [&](std::uint32_t depth) -> RE::NiSourceTexture* {
            const auto slot = origin + depth * sizeof(void*);
            if (slot >= stackTop) return nullptr;

            std::uintptr_t candidate = 0;
            if (!TryReadPointer(slot, candidate)) return nullptr;

            // Small integers, misaligned values and stack addresses can't be the
            // object, and skipping them saves a fault per slot.
            if (candidate < 0x10000 || (candidate & 7u) != 0) return nullptr;
            if (candidate >= stackLow && candidate < stackTop) return nullptr;

            std::uintptr_t vtable = 0;
            if (!TryReadPointer(candidate, vtable)) return nullptr;

            return vtable == wanted ? reinterpret_cast<RE::NiSourceTexture*>(candidate) : nullptr;
        };

        if (auto* hit = at(g_lastStackDepth.load(std::memory_order_relaxed))) return hit;

        // Past a few hundred words the search has left the loader's frames and
        // can't turn up anything relevant.
        constexpr std::uint32_t kMaxSlots = 512;

        for (std::uint32_t depth = 0; depth < kMaxSlots; ++depth) {
            if (auto* hit = at(depth)) {
                g_lastStackDepth.store(depth, std::memory_order_relaxed);
                return hit;
            }
        }

        return nullptr;
    }

    // BSResource::Stream::DoGetName, virtual number 10.
    RE::BSFixedString GetStreamName(const void* stream) {
        RE::BSFixedString name;
        if (!stream) return name;

        std::uintptr_t vtable = 0;
        if (!TryReadPointer(reinterpret_cast<std::uintptr_t>(stream), vtable)) return name;

        std::uintptr_t entry = 0;
        if (!TryReadPointer(vtable + 10 * sizeof(void*), entry) || entry == 0) return name;

        using DoGetName_t = bool (*)(const void*, RE::BSFixedString&);
        reinterpret_cast<DoGetName_t>(entry)(stream, name);

        return name;
    }

    // Holds the name alive for as long as the caller needs it, whichever source
    // it came from.
    struct TextureName {
        const char*       text = nullptr;
        RE::BSFixedString owned;

        std::string_view view() const { return text ? std::string_view(text) : std::string_view{}; }
    };

    TextureName ResolveTextureName() {
        TextureName result;

        auto* texture = FindSourceTextureOnStack();
        if (!texture) return result;

        // NiSourceTexture::name is only filled in once the resource exists, so
        // it's usually still empty here. The stream the texture reads from knows
        // the file already; it sits at 0x40, right after NiTexture.
        constexpr std::size_t kStreamSlot = 0x40 / sizeof(void*);

        result.owned = GetStreamName(reinterpret_cast<void* const*>(texture)[kStreamSlot]);
        result.text  = result.owned.c_str();

        if (!result.text || *result.text == '\0') result.text = texture->name.c_str();

        return result;
    }

    Category CategoryOf(std::string_view name) {
        auto stem = name;

        const auto slash = stem.find_last_of("\\/");
        if (slash != std::string_view::npos) stem = stem.substr(slash + 1);

        const auto dot = stem.rfind('.');
        if (dot != std::string_view::npos) stem = stem.substr(0, dot);

        for (const auto& entry : kSuffixes)
            if (stem.size() > entry.suffix.size() &&
                EqualsFolded(stem.substr(stem.size() - entry.suffix.size()), entry.suffix))
                return entry.category;

        return Category::Diffuse;
    }

    // A folder rule wins over the type: it's the more specific of the two.
    std::uint32_t LimitFor(std::string_view name, Category category) {
        for (const auto& folder : g_config.folders)
            if (ContainsFolded(name, folder.folder)) return folder.maxSize;

        return g_config.maxSize[static_cast<std::size_t>(category)];
    }

    // How many mip levels to drop off the front of the chain. Zero means leave
    // the texture alone.
    std::uint32_t ComputeSkip(const D3D11_TEXTURE2D_DESC& desc, std::string_view name) {
        const auto limit = LimitFor(name, CategoryOf(name));
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
    bool IsCandidate(const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data) {
        // No initial data means there's no pyramid to pick from: the game fills
        // the texture later and expects the size it asked for.
        if (!g_config.enabled || !data) return false;

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

        // Dynamic and staging textures get rewritten at runtime by code that
        // works out offsets from the original size.
        if (desc.Usage != D3D11_USAGE_DEFAULT && desc.Usage != D3D11_USAGE_IMMUTABLE) return false;

        // Under the smallest limit in the file, no rule can have a say.
        if (g_config.smallestLimit == 0) return false;
        return desc.Width > g_config.smallestLimit || desc.Height > g_config.smallestLimit;
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
        if (!desc || !IsCandidate(*desc, data))
            return g_originalCreateTexture2D(self, desc, data, out);

        const auto name = ResolveTextureName();
        const auto skip = ComputeSkip(*desc, name.view());
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

        SKSE::log::debug("{} {} {}x{} -> {}x{} {} KB",
                         name.view().empty() ? "<unnamed>" : name.view(),
                         kCategoryNames[static_cast<std::size_t>(CategoryOf(name.view()))],
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
