#include "Config.h"

#include "Logging.h"

namespace {
    Config g_editing;

    // What the ini held the last time it was read or written, so the menu can
    // tell whether anything is still only in memory.
    Config g_saved;

    // Never null, so a hook running before the first publish reads defaults
    // instead of dereferencing nothing.
    std::atomic<ConfigPtr> g_active{std::make_shared<const Config>()};

    std::filesystem::path GetIniPath() {
        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        return std::filesystem::path(std::format("Data/SKSE/Plugins/{}.ini", plugin->GetName()));
    }

    // Must stay identical to the .ini shipped with the plugin. Written back
    // when the user's copy is missing.
constexpr std::string_view kDefaultIni = R"INI([General]
; 0 turns the plugin off.
Enabled=1
; 0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Fatal
; 1 logs each texture loaded at a reduced size, with its name and category.
LogLevel=2
; Counts the textures the game loads, per folder. Shown by the in-game menu,
; which needs SKSE Menu Framework. Costs a little, so it is off by default.
TrackUsedFolders=0

[Textures]
; Maximum size per texture type. 0 leaves that type at full size.
; The type is read from the end of the file name:
;   Normal    _n _msn
;   Parallax  _p
;   Material  _rmaos          (PBR / complex material)
;   Glow      _g
;   Mask      _m _em _s _sk _b
;   Diffuse   everything else
Diffuse=1024
Normal=1024
Parallax=1024
Material=1024
Glow=1024
Mask=1024

[Folders]
; Maximum size for a folder, matched anywhere in the path. Wins over
; [Textures]. 0 leaves the folder at full size. Close a rule on both sides:
; \lod matches \clutter\lodestone.dds as well, \lod\ does not.
;
; A rule can start with a texture type, and then it only applies to that
; type. Rules are read from top to bottom and the first one that matches
; wins, so these two keep doors at full size except for their normal maps:
;   Normal:door=1024
;   door=0

\interface\=0
\sky\=0
\terrain\=0
\lod\=0
\dyndolod\=0
\lodgen\=0
Normal:\landscape\mountains\=2048
\landscape\mountains\=0
Normal:grass=2048
grass=0
Normal:rocks=2048
rocks=0
Normal:\alduin\=2048
\alduin\=0
Normal:\dragon\=2048
\dragon\=0
Normal:\parthurnax\=2048
\parthurnax\=0
Normal:\undeaddragon\=2048
\undeaddragon\=0
\tintmasks\=0
\facegendata\=0
\effects\gradients\=0
Normal:\pbr\=1024
\pbr\=2048
\actors\character\=2048
Normal:\armor\=1024
\armor\=2048
Normal:\clothes\=1024
\clothes\=2048

; Your own rules go here.

; Vanilla folders, for reference. Deeper paths work too, for example
; \architecture\whiterun\ or \actors\character\male\.
;
; _byoh  actors  architecture  armor  blood  clothes  clutter
; creationclub  critters  dlc01  dlc02  dungeons  effects
; furniture  impactdecals  interface  landscape  lod  plants  puddle
; shadertests  sky  terrain  test  trap  water  weapons
)INI";

    // Only writes when the file is absent, so edited settings survive an
    // update.
    void WriteDefaultIni(const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) || ec) return;

        // Binary, so the line endings above aren't translated a second time.
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            SKSE::log::warn("Can't write {}", path.string());
            return;
        }

        file << kDefaultIni;
        SKSE::log::info("Created {}", path.string());
    }

    // Zero disables downscaling for that entry, anything else is raised to the
    // floor.
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
            std::string entry;
            for (const char* c = key.pItem; *c; ++c) entry.push_back(Fold(*c));

            std::optional<Category> category;
            std::string_view        folder{entry};

            if (!SplitCategory(folder, category)) {
                SKSE::log::warn("{} isn't a texture type, rule ignored", key.pItem);
                continue;
            }

            if (folder.empty()) continue;

            const auto size = ClampSize(ini.GetLongValue("Folders", key.pItem, 0), key.pItem);
            config.folders.emplace_back(std::string(folder), size, category);
        }
    }

    // A rule an earlier one already covers never fires.
    void WarnShadowedFolders(const Config& config) {
        for (std::size_t i = 0; i < config.folders.size(); ++i) {
            std::size_t shadowedBy = 0;
            if (IsRuleShadowed(config, i, &shadowedBy))
                SKSE::log::warn("{} never applies, {} above it matches first",
                                RuleName(config.folders[i]), RuleName(config.folders[shadowedBy]));
        }
    }
}

std::atomic<bool>          g_enabled{false};
std::atomic<bool>          g_trackUsedFolders{false};
std::atomic<std::uint32_t> g_smallestLimit{0};

Config& EditableConfig() { return g_editing; }

ConfigPtr ActiveConfig() { return g_active.load(std::memory_order_acquire); }

void PublishConfig() {
    RecomputeSmallestLimit(g_editing);

    g_active.store(std::make_shared<const Config>(g_editing), std::memory_order_release);

    // The gates only decide whether a texture is worth inspecting, so a reader
    // that sees them out of step with the snapshot loses at most one texture.
    g_enabled.store(g_editing.enabled, std::memory_order_relaxed);
    g_trackUsedFolders.store(g_editing.trackUsedFolders, std::memory_order_relaxed);
    g_smallestLimit.store(g_editing.smallestLimit, std::memory_order_relaxed);
}

void LoadConfig() {
    const auto path = GetIniPath();
    WriteDefaultIni(path);

    auto& config = EditableConfig();

    CSimpleIniA ini;
    ini.SetUnicode();

    const bool loaded = ini.LoadFile(path.string().c_str()) >= 0;

    if (loaded) {
        config.enabled          = ini.GetBoolValue("General", "Enabled", true);
        config.trackUsedFolders = ini.GetBoolValue("General", "TrackUsedFolders", false);
        config.logLevel = static_cast<std::uint32_t>(ini.GetLongValue("General", "LogLevel", 2));
    }

    // Applied here so the configured level covers everything that follows.
    SetLogLevel(config.logLevel);

    if (!loaded) SKSE::log::warn("No {}, using defaults", path.string());

    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        const std::string key(kCategoryNames[i]);
        config.maxSize[i] =
            loaded ? ClampSize(ini.GetLongValue("Textures", key.c_str(), 1024), key) : 1024;
    }

    config.folders.clear();
    if (loaded) ReadFolders(ini, config);

    // A 1.x file would otherwise read as "everything at default".
    if (loaded && (ini.GetSectionSize("Suffix") >= 0 || ini.GetSectionSize("Path") >= 0))
        SKSE::log::warn("[Suffix] and [Path] are gone, see [Textures] and [Folders]");

    std::string summary;
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        if (i > 0) summary += ' ';
        summary += std::format("{}={}", kCategoryNames[i], config.maxSize[i]);
    }

    SKSE::log::info("Enabled={} {}", config.enabled, summary);

    for (const auto& folder : config.folders)
        SKSE::log::info("Folder {}={}", RuleName(folder), folder.maxSize);

    WarnShadowedFolders(config);

    PublishConfig();

    g_saved = config;

    if (config.enabled && config.smallestLimit == 0)
        SKSE::log::warn("Nothing can be downscaled with these settings");
}

void SaveConfig() {
    const auto  path   = GetIniPath();
    const auto& config = EditableConfig();

    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(path.string().c_str());

    ini.SetBoolValue("General", "Enabled", config.enabled);
    ini.SetBoolValue("General", "TrackUsedFolders", config.trackUsedFolders);
    ini.SetLongValue("General", "LogLevel", static_cast<long>(config.logLevel));

    for (std::size_t i = 0; i < kCategoryCount; ++i)
        ini.SetLongValue("Textures", std::string(kCategoryNames[i]).c_str(),
                         static_cast<long>(config.maxSize[i]));

    // Adding, removing, reordering or renaming a rule changes which keys should
    // exist or in what order, and SimpleIni cannot move a key. Comments in the
    // section are lost when this happens.
    CSimpleIniA::TNamesDepend onDisk;
    ini.GetAllKeys("Folders", onDisk);
    onDisk.sort(CSimpleIniA::Entry::LoadOrder());

    bool sameShape = onDisk.size() == config.folders.size();
    if (sameShape) {
        auto key = onDisk.begin();
        for (const auto& folder : config.folders) {
            if (RuleName(folder) != key->pItem) {
                sameShape = false;
                break;
            }
            ++key;
        }
    }

    if (!sameShape) ini.Delete("Folders", nullptr);
    for (const auto& folder : config.folders)
        ini.SetLongValue("Folders", RuleName(folder).c_str(), static_cast<long>(folder.maxSize));

    if (ini.SaveFile(path.string().c_str()) < 0) {
        SKSE::log::warn("Couldn't write {}", path.string());
        return;
    }

    g_saved = config;
    SKSE::log::info("Settings saved to {}", path.string());
}

bool ConfigDirty() { return !(g_editing == g_saved); }

bool EqualsFolded(std::string_view left, std::string_view right) {
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

std::optional<Category> CategoryFromName(std::string_view name) {
    for (std::size_t i = 0; i < kCategoryCount; ++i)
        if (EqualsFolded(name, kCategoryNames[i])) return static_cast<Category>(i);

    return std::nullopt;
}

std::string RuleName(const FolderRule& rule) {
    if (!rule.category) return rule.folder;

    return std::format("{}:{}", kCategoryNames[static_cast<std::size_t>(*rule.category)], rule.folder);
}

bool SplitCategory(std::string_view& folder, std::optional<Category>& category) {
    const auto colon = folder.find(':');
    if (colon == std::string_view::npos) return true;

    category = CategoryFromName(folder.substr(0, colon));
    if (!category) return false;

    folder.remove_prefix(colon + 1);
    return true;
}

bool IsRuleShadowed(const Config& config, std::size_t index, std::size_t* shadowedBy) {
    const auto& rule = config.folders[index];

    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        const auto& candidate = config.folders[earlier];

        if (candidate.category && candidate.category != rule.category) continue;
        if (!ContainsFolded(rule.folder, candidate.folder)) continue;

        if (shadowedBy) *shadowedBy = earlier;
        return true;
    }
    return false;
}

void RecomputeSmallestLimit(Config& config) {
    config.smallestLimit = 0;

    for (const auto size : config.maxSize)
        if (size != 0 && (config.smallestLimit == 0 || size < config.smallestLimit))
            config.smallestLimit = size;

    for (const auto& folder : config.folders)
        if (folder.maxSize != 0 &&
            (config.smallestLimit == 0 || folder.maxSize < config.smallestLimit))
            config.smallestLimit = folder.maxSize;
}
