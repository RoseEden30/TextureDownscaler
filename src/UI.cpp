#include "UI.h"

#include "Config.h"
#include "Hooks.h"
#include "Logging.h"
#include "TextureFolders.h"

#pragma warning(push)
#pragma warning(disable : 4099 4996 5054)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <format>
#include <utility>

namespace {
    constexpr ImGuiMCP::ImVec4 kGoodColour{0.45f, 0.85f, 0.45f, 1.0f};
    constexpr ImGuiMCP::ImVec4 kWarningColour{1.0f, 0.6f, 0.2f, 1.0f};
    constexpr ImGuiMCP::ImVec4 kErrorColour{1.0f, 0.35f, 0.35f, 1.0f};

    constexpr std::array<std::uint32_t, 8> kSizes{0, 128, 256, 512, 1024, 2048, 4096, 8192};

    void Tooltip(const char* text) { ImGuiMCP::SetItemTooltip("%s", text); }

    const std::string& PluginVersion() {
        static const std::string version = [] {
            const auto v = SKSE::PluginDeclaration::GetSingleton()->GetVersion();
            return std::format("{}.{}.{}", v.major(), v.minor(), v.patch());
        }();
        return version;
    }

    const std::string& SuffixList(Category category) {
        static const auto lists = [] {
            std::array<std::string, kCategoryCount> result;
            result[static_cast<std::size_t>(Category::Diffuse)] = "everything else";

            for (const auto& entry : kSuffixes) {
                auto& list = result[static_cast<std::size_t>(entry.category)];
                if (!list.empty()) list += "   ";
                list += std::format("*{}.dds", entry.suffix);
            }
            return result;
        }();
        return lists[static_cast<std::size_t>(category)];
    }

    void FillColumn() { ImGuiMCP::SetNextItemWidth(-FLT_MIN); }

    float TextWidth(const char* text) { return ImGuiMCP::CalcTextSize(text).x; }

    float ButtonWidth(const char* label) {
        const auto* style   = ImGuiMCP::GetStyle();
        const auto  padding = style ? style->FramePadding.x * 2.0f : ImGuiMCP::GetFontSize();

        return TextWidth(label) + padding;
    }

    // Room for the widest entry, the dropdown arrow and the frame padding.
    // Measured rather than guessed in font heights, since the interface scale
    // moves the two apart.
    float ComboWidth(float textWidth) {
        return textWidth + ImGuiMCP::GetFrameHeight() + ImGuiMCP::GetFontSize();
    }

    float CategoryComboWidth() {
        auto widest = TextWidth("Any type");
        for (const auto name : kCategoryNames)
            widest = std::max(widest, TextWidth(std::string(name).c_str()));

        return ComboWidth(widest);
    }

    float SizeComboWidth() { return ComboWidth(TextWidth("Full size")); }

    // Draws only the rows that are actually on screen. Every ImGui call here
    // resolves through GetProcAddress, so a few thousand rows a frame would be
    // felt even though each one is trivial.
    struct ListClipper {
        ImGuiMCP::ImGuiListClipper* handle = ImGuiMCP::ImGuiListClipperManager::Create();

        ListClipper(std::size_t items, float itemHeight) {
            ImGuiMCP::ImGuiListClipperManager::Begin(handle, static_cast<int>(items), itemHeight);
        }

        ~ListClipper() {
            ImGuiMCP::ImGuiListClipperManager::End(handle);
            ImGuiMCP::ImGuiListClipperManager::Destroy(handle);
        }

        ListClipper(const ListClipper&)            = delete;
        ListClipper& operator=(const ListClipper&) = delete;

        bool Step() { return ImGuiMCP::ImGuiListClipperManager::Step(handle); }

        int First() const { return handle->DisplayStart; }
        int Last() const { return handle->DisplayEnd; }
    };

    // What the save and reload row needs at the bottom of every page.
    float FooterHeight() {
        return ImGuiMCP::GetFrameHeightWithSpacing() + ImGuiMCP::GetTextLineHeightWithSpacing();
    }

    // A table sized from the room that's left, so the footer stays on screen
    // instead of being pushed under it. reservedBelow covers anything drawn
    // between the table and the footer.
    ImGuiMCP::ImVec2 TableHeight(float rowHeight, float reservedBelow = 0.0f) {
        const auto available = ImGuiMCP::GetContentRegionAvail().y - FooterHeight() - reservedBelow;
        return {0.0f, std::max(available, rowHeight * 5.0f)};
    }

    // A browser row is one button tall. Asking for it explicitly, rather than
    // letting the row size itself, is what makes every row identical.
    float RowContentHeight() { return ImGuiMCP::GetFrameHeight(); }

    // What a row measures once the table has padded it. The clipper is given
    // this instead of measuring the first row and extrapolating, which drifts
    // over a long list and only shows at the bottom.
    float RowHeight() {
        const auto* style = ImGuiMCP::GetStyle();
        if (!style) return ImGuiMCP::GetFrameHeightWithSpacing();

        return RowContentHeight() + style->CellPadding.y * 2.0f;
    }

    std::string SizeLabel(std::uint32_t size) {
        return size == 0 ? "Full size" : std::to_string(size);
    }

    bool IsStandardSize(std::uint32_t size) {
        return std::find(kSizes.begin(), kSizes.end(), size) != kSizes.end();
    }

    bool RenderSizeCombo(const char* label, std::uint32_t& size) {
        if (!ImGuiMCP::BeginCombo(label, SizeLabel(size).c_str())) return false;

        bool changed = false;

        for (const auto option : kSizes)
            if (ImGuiMCP::Selectable(SizeLabel(option).c_str(), option == size) && option != size) {
                size    = option;
                changed = true;
            }

        // A value typed straight into the ini keeps its place in the list
        // instead of disappearing the first time the menu is opened.
        if (!IsStandardSize(size)) ImGuiMCP::Selectable(SizeLabel(size).c_str(), true);

        ImGuiMCP::EndCombo();
        return changed;
    }

    std::string ComboLabel(int selected) {
        return selected == 0 ? "Any type" : std::string(kCategoryNames[selected - 1]);
    }

    // selected is 0 for "any type", 1..kCategoryCount for a category.
    bool RenderCategoryCombo(const char* label, int& selected) {
        if (!ImGuiMCP::BeginCombo(label, ComboLabel(selected).c_str())) return false;

        bool changed = false;

        if (ImGuiMCP::Selectable("Any type", selected == 0) && selected != 0) {
            selected = 0;
            changed  = true;
        }

        for (std::size_t i = 0; i < kCategoryCount; ++i) {
            const auto index = static_cast<int>(i) + 1;
            if (ImGuiMCP::Selectable(std::string(kCategoryNames[i]).c_str(), selected == index) &&
                selected != index) {
                selected = index;
                changed  = true;
            }
        }

        ImGuiMCP::EndCombo();
        return changed;
    }

    std::optional<Category> CategoryFromCombo(int selected) {
        if (selected == 0) return std::nullopt;
        return static_cast<Category>(selected - 1);
    }

    bool HasRuleFor(const Config& config, std::string_view folder) {
        return std::any_of(config.folders.begin(), config.folders.end(),
                           [&](const FolderRule& rule) { return EqualsFolded(rule.folder, folder); });
    }

    // Moves one rule and leaves the order of the others alone. A swap would do
    // for a neighbour, but not for a jump to either end.
    void MoveRule(std::vector<FolderRule>& rules, std::size_t from, std::size_t to) {
        if (from == to) return;

        const auto first = rules.begin();
        const auto at    = [&](std::size_t i) { return first + static_cast<std::ptrdiff_t>(i); };

        if (from < to)
            std::rotate(at(from), at(from + 1), at(to + 1));
        else
            std::rotate(at(to), at(from), at(from + 1));
    }

    // Where the rule for this folder and type sits, or folders.size() when
    // there isn't one. Matching on both is what lets the browser undo an add
    // without touching a rule the user wrote for another type.
    std::size_t FindRule(const Config& config, std::string_view folder,
                         std::optional<Category> category) {
        for (std::size_t i = 0; i < config.folders.size(); ++i)
            if (config.folders[i].category == category &&
                EqualsFolded(config.folders[i].folder, folder))
                return i;

        return config.folders.size();
    }

    // Shared by every page. Save is only offered when there is something to
    // save, and dropping edits asks first, so neither button can surprise
    // anyone. The pages the menu registers can't carry a marker of their own,
    // so the state is spelled out here instead.
    void RenderSaveReload() {
        static bool confirmDiscard = false;

        const bool dirty = ConfigDirty();
        if (!dirty) confirmDiscard = false;

        ImGuiMCP::Separator();

        if (confirmDiscard) {
            ImGuiMCP::TextColored(kWarningColour, "Discard the unsaved changes?");
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Keep editing")) confirmDiscard = false;
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Discard")) {
                LoadConfig();
                confirmDiscard = false;
            }
            return;
        }

        ImGuiMCP::BeginDisabled(!dirty);
        if (ImGuiMCP::Button("Save")) SaveConfig();
        ImGuiMCP::EndDisabled();

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Reload")) {
            if (dirty)
                confirmDiscard = true;
            else
                LoadConfig();
        }

        ImGuiMCP::SameLine();
        if (dirty)
            ImGuiMCP::TextColored(kWarningColour, "Unsaved changes");
        else
            ImGuiMCP::Text("Saved to TextureDownscaler.ini");
    }
}

void UI::Register() {
    if (!SKSEMenuFramework::IsInstalled()) return;

    SKSEMenuFramework::SetSection("TextureDownscaler");
    SKSEMenuFramework::AddSectionItem("General", General::Render);
    SKSEMenuFramework::AddSectionItem("Categories", Categories::Render);
    SKSEMenuFramework::AddSectionItem("Folder rules", Folders::Render);
    SKSEMenuFramework::AddSectionItem("Browse folders", Browse::Render);
    SKSEMenuFramework::AddSectionItem("Help", Help::Render);
}

namespace {
    void RenderStatus(const Config& config, HookStatus status) {
        switch (status) {
            case HookStatus::Installed:
                if (config.enabled) {
                    ImGuiMCP::TextColored(kGoodColour, "Active");
                } else {
                    ImGuiMCP::TextColored(kWarningColour, "Paused");
                    ImGuiMCP::SameLine();
                    ImGuiMCP::TextWrapped("Textures load at full size until Enabled is ticked again.");
                }
                break;

            case HookStatus::DisabledAtStartup:
                ImGuiMCP::TextColored(kWarningColour, "Off since startup");
                ImGuiMCP::TextWrapped("The game started with the plugin disabled, so it isn't running this "
                                      "session. Tick Enabled, save, then restart the game.");
                return;

            case HookStatus::NotInstalled:
                ImGuiMCP::TextColored(kErrorColour, "Not running");
                ImGuiMCP::TextWrapped("The plugin couldn't start. TextureDownscaler.log says why.");
                return;
        }

        const auto stats = GetReductionStats();
        if (stats.textures == 0) {
            ImGuiMCP::Text("No texture loaded at a reduced size yet.");
            return;
        }

        const auto megabytes = static_cast<double>(stats.savedBytes) / (1024.0 * 1024.0);
        const auto summary   = std::format("{} texture{} loaded at a reduced size this session, about {:.0f} MB "
                                           "smaller in total.",
                                           stats.textures, stats.textures == 1 ? "" : "s", megabytes);
        ImGuiMCP::TextWrapped("%s", summary.c_str());
    }
}

void __stdcall UI::General::Render() {
    auto& config = EditableConfig();

    const auto em = ImGuiMCP::GetFontSize();

    ImGuiMCP::Text("Texture Downscaler %s", PluginVersion().c_str());
    ImGuiMCP::Text("Loads textures at a smaller size to save video memory.");

    ImGuiMCP::SeparatorText("Status");
    RenderStatus(config, GetHookStatus());

    ImGuiMCP::SeparatorText("Settings");

    bool enabled = config.enabled;
    if (ImGuiMCP::Checkbox("Enabled", &enabled)) {
        config.enabled = enabled;
        PublishConfig();
    }
    Tooltip("When off, textures load at full size. Only affects textures loaded from now on.");

    static const char* const kLevels[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};

    int level = static_cast<int>(std::min<std::uint32_t>(config.logLevel, 5));
    ImGuiMCP::SetNextItemWidth(em * 10.0f);
    if (ImGuiMCP::Combo("Log level", &level, kLevels, 6)) {
        config.logLevel = static_cast<std::uint32_t>(level);
        SetLogLevel(config.logLevel);
        PublishConfig();
    }
    Tooltip("Debug writes a line to TextureDownscaler.log for every texture loaded at a reduced size, "
            "with its type.");

    ImGuiMCP::Spacing();
    RenderSaveReload();
}

void __stdcall UI::Categories::Render() {
    auto& config = EditableConfig();

    ImGuiMCP::TextWrapped("The largest size a texture keeps, by type. The type comes from the file name, "
                          "e.g. ironsword_n.dds is a Normal map.");
    ImGuiMCP::Spacing();

    bool changed = false;

    const auto em        = ImGuiMCP::GetFontSize();
    const auto sizeWidth = SizeComboWidth();

    constexpr auto kTableFlags = ImGuiMCP::ImGuiTableFlags_BordersInnerH |
                                 ImGuiMCP::ImGuiTableFlags_RowBg |
                                 ImGuiMCP::ImGuiTableFlags_SizingFixedFit;

    if (ImGuiMCP::BeginTable("categories", 3, kTableFlags)) {
        ImGuiMCP::TableSetupColumn("Type", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
        ImGuiMCP::TableSetupColumn("Files", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableSetupColumn("Max size", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, sizeWidth + em);
        ImGuiMCP::TableHeadersRow();

        for (std::size_t i = 0; i < kCategoryCount; ++i) {
            ImGuiMCP::PushID(static_cast<int>(i));
            ImGuiMCP::TableNextRow();

            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::AlignTextToFramePadding();
            const std::string name(kCategoryNames[i]);
            ImGuiMCP::TextUnformatted(static_cast<Category>(i) == Category::Material ? "Material/PBR"
                                                                                    : name.c_str());

            ImGuiMCP::TableSetColumnIndex(1);
            ImGuiMCP::AlignTextToFramePadding();
            ImGuiMCP::TextUnformatted(SuffixList(static_cast<Category>(i)).c_str());

            ImGuiMCP::TableSetColumnIndex(2);
            FillColumn();
            if (RenderSizeCombo("##size", config.maxSize[i])) changed = true;

            ImGuiMCP::PopID();
        }

        ImGuiMCP::EndTable();
    }

    if (changed) PublishConfig();

    ImGuiMCP::Spacing();
    ImGuiMCP::Text("Folder rules override these. Textures already in memory keep their size.");

    ImGuiMCP::Spacing();
    RenderSaveReload();
}

void __stdcall UI::Folders::Render() {
    auto& config = EditableConfig();

    const auto em = ImGuiMCP::GetFontSize();

    ImGuiMCP::TextWrapped("A rule applies to every texture whose path contains its text, and overrides the "
                          "type limit. Rules are checked from the top and the first match wins.");

    bool changed = false;

    const auto typeWidth = CategoryComboWidth();
    const auto sizeWidth = SizeComboWidth();

    ImGuiMCP::SeparatorText("New rule");
    {
        static char          newFolder[256] = "";
        static std::uint32_t newSize        = 1024;
        static int           newCategory    = 0;

        ImGuiMCP::SetNextItemWidth(typeWidth);
        RenderCategoryCombo("##newtype", newCategory);
        Tooltip("Any type, or only textures of this type.");

        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(em * 16.0f);
        ImGuiMCP::InputTextWithHint("##newfolder", "Text to match, e.g. \\armor\\", newFolder,
                                    sizeof(newFolder));
        Tooltip("Matched anywhere in the path. Put backslashes on both sides to match a whole folder "
                "name rather than part of one.");

        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(sizeWidth);
        RenderSizeCombo("##newsize", newSize);

        ImGuiMCP::SameLine();
        ImGuiMCP::BeginDisabled(newFolder[0] == '\0');
        if (ImGuiMCP::Button("Add")) {
            std::string folder(newFolder);
            for (auto& c : folder) c = Fold(c);

            config.folders.insert(config.folders.begin(),
                                  FolderRule{std::move(folder), newSize, CategoryFromCombo(newCategory)});

            newFolder[0] = '\0';
            changed      = true;
        }
        ImGuiMCP::EndDisabled();
        Tooltip("New rules go to the top, where a narrow rule belongs.");
    }

    const auto header = std::format("Rules ({})", config.folders.size());
    ImGuiMCP::SeparatorText(header.c_str());

    auto removeIndex = static_cast<std::size_t>(-1);
    std::optional<std::pair<std::size_t, std::size_t>> moveIndices;

    // Without a sizing policy a table stretches every column evenly and the
    // requested widths are only a starting point, which leaves the folder
    // column with whatever the others didn't take. FixedFit holds them.
    constexpr auto kTableFlags = ImGuiMCP::ImGuiTableFlags_Borders |
                                 ImGuiMCP::ImGuiTableFlags_RowBg |
                                 ImGuiMCP::ImGuiTableFlags_ScrollY |
                                 ImGuiMCP::ImGuiTableFlags_SizingFixedFit;

    constexpr auto kFixed   = ImGuiMCP::ImGuiTableColumnFlags_WidthFixed;
    constexpr auto kStretch = ImGuiMCP::ImGuiTableColumnFlags_WidthStretch;

    const auto ruleRowHeight = ImGuiMCP::GetFrameHeightWithSpacing();

    if (config.folders.empty()) {
        ImGuiMCP::Text("No rules. Every texture follows the limit for its type.");
    } else if (ImGuiMCP::BeginTable("folder-rules", 6, kTableFlags, TableHeight(ruleRowHeight))) {
        ImGuiMCP::TableSetupColumn("#", kFixed);
        ImGuiMCP::TableSetupColumn("Type", kFixed, typeWidth + em);
        ImGuiMCP::TableSetupColumn("Text to match", kStretch);
        ImGuiMCP::TableSetupColumn("Max size", kFixed, sizeWidth + em);
        ImGuiMCP::TableSetupColumn("##order", kFixed);
        ImGuiMCP::TableSetupColumn("##remove", kFixed);
        ImGuiMCP::TableSetupScrollFreeze(0, 1);
        ImGuiMCP::TableHeadersRow();

        for (std::size_t i = 0; i < config.folders.size(); ++i) {
            auto& rule = config.folders[i];

            ImGuiMCP::PushID(static_cast<int>(i));
            ImGuiMCP::TableNextRow();

            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::AlignTextToFramePadding();
            ImGuiMCP::Text("%d", static_cast<int>(i) + 1);

            ImGuiMCP::TableSetColumnIndex(1);
            int selected = rule.category ? static_cast<int>(*rule.category) + 1 : 0;
            FillColumn();
            if (RenderCategoryCombo("##type", selected)) {
                rule.category = CategoryFromCombo(selected);
                changed       = true;
            }

            ImGuiMCP::TableSetColumnIndex(2);
            char folder[256];
            std::snprintf(folder, sizeof(folder), "%s", rule.folder.c_str());
            FillColumn();
            if (ImGuiMCP::InputText("##folder", folder, sizeof(folder))) {
                rule.folder.assign(folder);
                for (auto& c : rule.folder) c = Fold(c);
                changed = true;
            }

            std::size_t shadowedBy = 0;
            if (IsRuleShadowed(config, i, &shadowedBy))
                ImGuiMCP::TextColored(kWarningColour, "Never used, rule %d matches first",
                                      static_cast<int>(shadowedBy) + 1);

            ImGuiMCP::TableSetColumnIndex(3);
            FillColumn();
            if (RenderSizeCombo("##size", rule.maxSize)) changed = true;

            ImGuiMCP::TableSetColumnIndex(4);

            const auto last = config.folders.size() - 1;

            ImGuiMCP::BeginDisabled(i == 0);
            if (ImGuiMCP::ArrowButton("##up", ImGuiMCP::ImGuiDir_Up)) moveIndices = {i, i - 1};
            Tooltip("Move up");
            ImGuiMCP::EndDisabled();

            ImGuiMCP::SameLine();
            ImGuiMCP::BeginDisabled(i == last);
            if (ImGuiMCP::ArrowButton("##down", ImGuiMCP::ImGuiDir_Down)) moveIndices = {i, i + 1};
            Tooltip("Move down");
            ImGuiMCP::EndDisabled();

            ImGuiMCP::SameLine();
            ImGuiMCP::BeginDisabled(i == 0);
            if (ImGuiMCP::SmallButton("Top")) moveIndices = {i, 0};
            ImGuiMCP::EndDisabled();

            ImGuiMCP::SameLine();
            ImGuiMCP::BeginDisabled(i == last);
            if (ImGuiMCP::SmallButton("Bottom")) moveIndices = {i, last};
            ImGuiMCP::EndDisabled();

            ImGuiMCP::TableSetColumnIndex(5);
            if (ImGuiMCP::SmallButton("Remove")) removeIndex = i;

            ImGuiMCP::PopID();
        }

        ImGuiMCP::EndTable();
    }

    // Deferred until the table is closed so the row loop keeps valid indices.
    if (removeIndex != static_cast<std::size_t>(-1)) {
        config.folders.erase(config.folders.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        changed = true;
    } else if (moveIndices) {
        MoveRule(config.folders, moveIndices->first, moveIndices->second);
        changed = true;
    }

    // Once per frame, after every edit has landed in the working copy.
    if (changed) PublishConfig();

    ImGuiMCP::Spacing();
    RenderSaveReload();
}

namespace {
    struct BrowseRow {
        std::string   path;
        std::string   rule;      // how the path reads as a folder rule
        std::uint32_t files = 0; // loose .dds sitting on disk
        std::uint32_t used  = 0; // textures loaded from it this session
    };

    std::vector<BrowseRow> g_rows;
    int                    g_sortColumn    = 0;
    bool                   g_sortAscending = true;

    void SortRows() {
        std::stable_sort(g_rows.begin(), g_rows.end(), [](const BrowseRow& a, const BrowseRow& b) {
            const auto& first  = g_sortAscending ? a : b;
            const auto& second = g_sortAscending ? b : a;

            if (g_sortColumn == 1 && first.files != second.files) return first.files < second.files;
            if (g_sortColumn == 2 && first.used != second.used) return first.used < second.used;
            return first.path < second.path;
        });
    }

    // The scan says what is on disk, the tally says what the game asked for. A
    // folder can be in one and not the other: an archive nobody touched, or a
    // mod that keeps its textures in an archive of its own.
    void RebuildRows() {
        auto used = UsedFolders();

        g_rows.clear();
        g_rows.reserve(ScannedFolders().size() + used.size());

        // The rule text is built here rather than per frame: it is read once
        // for the filter and once for the row, on every row, every frame.
        for (const auto& folder : ScannedFolders()) {
            BrowseRow row{folder.path, FolderRuleText(folder.path), folder.files, 0};

            if (const auto it = used.find(folder.path); it != used.end()) {
                row.used = it->second;
                used.erase(it);
            }

            g_rows.push_back(std::move(row));
        }

        for (const auto& [path, count] : used)
            g_rows.push_back(BrowseRow{path, FolderRuleText(path), 0, count});

        SortRows();
    }
}

void __stdcall UI::Browse::Render() {
    auto& config = EditableConfig();

    // The walk starts when someone opens this page, and only once.
    BeginFolderScan();

    if (!FolderScanReady()) {
        ImGuiMCP::Text("Reading Data\\textures...");
        ImGuiMCP::Spacing();
        RenderSaveReload();
        return;
    }

    // The counts keep moving as the game streams textures in. Rebuilding every
    // frame would copy the whole tally for nothing.
    static int refreshIn = 0;
    if (refreshIn-- <= 0) {
        RebuildRows();
        refreshIn = 30;
    }

    const auto em = ImGuiMCP::GetFontSize();

    static char          filter[128]     = "";
    static std::uint32_t addSize         = 1024;
    static int           addCategory     = 0;
    static bool          hideArchiveOnly = false;
    static bool          hideUnused      = false;
    static bool          hideRuled       = false;

    ImGuiMCP::TextWrapped("Every folder under Data\\textures, plus the ones the base game keeps in its "
                          "archives. Useful to see where a rule is worth adding.");

    ImGuiMCP::SeparatorText("Usage");

    bool track = config.trackUsedFolders;
    if (ImGuiMCP::Checkbox("Track used folders", &track)) {
        config.trackUsedFolders = track;
        PublishConfig();
    }
    ImGuiMCP::SetItemTooltip("Counts textures as the game loads them, to fill the Used column. "
                             "Off by default, since it costs a little on every texture load.");

    ImGuiMCP::SameLine();
    if (ImGuiMCP::Button("Reset counts")) {
        ClearUsedFolders();
        RebuildRows();
    }
    Tooltip("Sets every Used count back to zero.");

    if (!config.trackUsedFolders)
        ImGuiMCP::TextColored(kWarningColour, "Tracking is off, the Used column won't fill in.");

    ImGuiMCP::SeparatorText("Filter");

    ImGuiMCP::SetNextItemWidth(-FLT_MIN);
    ImGuiMCP::InputTextWithHint("##filter", "Filter by name", filter, sizeof(filter));

    ImGuiMCP::Checkbox("In use only", &hideUnused);
    ImGuiMCP::SetItemTooltip("Only folders a texture has been loaded from.");
    ImGuiMCP::SameLine();
    ImGuiMCP::Checkbox("Loose files only", &hideArchiveOnly);
    ImGuiMCP::SetItemTooltip("Hide folders with no loose .dds file of their own.");
    ImGuiMCP::SameLine();
    ImGuiMCP::Checkbox("Hide folders with a rule", &hideRuled);
    ImGuiMCP::SetItemTooltip("Hide folders that already have a rule of their own.");

    // Kept apart from the filters above: these two decide what the Add buttons
    // write, not what the list shows.
    ImGuiMCP::SeparatorText("Add with");

    ImGuiMCP::SetNextItemWidth(SizeComboWidth());
    RenderSizeCombo("Max size", addSize);
    ImGuiMCP::SameLine(0.0f, em);
    ImGuiMCP::SetNextItemWidth(CategoryComboWidth());
    RenderCategoryCombo("Type", addCategory);
    ImGuiMCP::SameLine();
    ImGuiMCP::Text("(used by the Add buttons below)");

    ImGuiMCP::Spacing();

    const auto category = CategoryFromCombo(addCategory);

    bool changed = false;

    constexpr auto kTableFlags = ImGuiMCP::ImGuiTableFlags_Borders |
                                 ImGuiMCP::ImGuiTableFlags_RowBg |
                                 ImGuiMCP::ImGuiTableFlags_ScrollY |
                                 ImGuiMCP::ImGuiTableFlags_Sortable |
                                 ImGuiMCP::ImGuiTableFlags_SizingFixedFit;

    constexpr auto kFixed   = ImGuiMCP::ImGuiTableColumnFlags_WidthFixed;
    constexpr auto kStretch = ImGuiMCP::ImGuiTableColumnFlags_WidthStretch;
    constexpr auto kNoSort  = ImGuiMCP::ImGuiTableColumnFlags_NoSort;

    // Kept between frames so the filtering doesn't reallocate on every one.
    static std::vector<std::size_t> visible;
    visible.clear();

    for (std::size_t i = 0; i < g_rows.size(); ++i) {
        const auto& row = g_rows[i];

        if (hideUnused && row.used == 0) continue;
        if (hideArchiveOnly && row.files == 0) continue;
        if (filter[0] != '\0' && !ContainsFolded(row.path, filter)) continue;
        if (hideRuled && HasRuleFor(config, row.rule)) continue;

        visible.push_back(i);
    }

    const auto rowHeight  = RowHeight();
    const auto filesWidth = TextWidth("Loose files") + em;
    const auto usedWidth  = std::max(TextWidth("Used"), TextWidth("000000")) + em;
    const auto addWidth   = std::max(ButtonWidth("Add"), ButtonWidth("Remove")) + em;

    // Both applied once the table is closed, so the row loop keeps working on
    // indices that still mean something.
    auto                      removeIndex = static_cast<std::size_t>(-1);
    std::optional<FolderRule> pendingAdd;

    if (ImGuiMCP::BeginTable("texture-folders", 4, kTableFlags,
                             TableHeight(rowHeight, ImGuiMCP::GetTextLineHeightWithSpacing()))) {
        // Sized for the widest they can ever get. Left to size themselves they
        // would follow the rows currently on screen, and the table would shift
        // every time a wider label scrolled into view.
        ImGuiMCP::TableSetupColumn("Folder", kStretch);
        ImGuiMCP::TableSetupColumn("Loose files", kFixed, filesWidth);
        ImGuiMCP::TableSetupColumn("Used", kFixed, usedWidth);
        ImGuiMCP::TableSetupColumn("##add", kFixed | kNoSort, addWidth);
        ImGuiMCP::TableSetupScrollFreeze(0, 1);
        ImGuiMCP::TableHeadersRow();

        if (auto* specs = ImGuiMCP::TableGetSortSpecs(); specs && specs->SpecsDirty) {
            if (specs->SpecsCount > 0) {
                const auto& column = specs->Specs[0];
                g_sortColumn       = column.ColumnIndex;
                g_sortAscending = column.SortDirection != ImGuiMCP::ImGuiSortDirection_Descending;
                SortRows();
            }
            specs->SpecsDirty = false;
        }

        {
            ListClipper clipper(visible.size(), rowHeight);

            while (clipper.Step())
                for (auto index = clipper.First(); index < clipper.Last(); ++index) {
                    const auto& row = g_rows[visible[static_cast<std::size_t>(index)]];

                    ImGuiMCP::PushID(index);

                    // Pinning the row height makes the value handed to the
                    // clipper true rather than merely close.
                    ImGuiMCP::TableNextRow(0, RowContentHeight());

                    ImGuiMCP::TableSetColumnIndex(0);
                    ImGuiMCP::TextUnformatted(row.rule.c_str());

                    ImGuiMCP::TableSetColumnIndex(1);
                    if (row.files > 0)
                        ImGuiMCP::Text("%u", row.files);
                    else
                        ImGuiMCP::TextDisabled("-");

                    ImGuiMCP::TableSetColumnIndex(2);
                    if (row.used > 0)
                        ImGuiMCP::Text("%u", row.used);
                    else
                        ImGuiMCP::TextDisabled("-");

                    ImGuiMCP::TableSetColumnIndex(3);

                    if (const auto rule = FindRule(config, row.rule, category);
                        rule == config.folders.size()) {
                        if (ImGuiMCP::Button("Add"))
                            pendingAdd = FolderRule{row.rule, addSize, category};
                    } else if (ImGuiMCP::Button("Remove")) {
                        removeIndex = rule;
                    }

                    ImGuiMCP::PopID();
                }
        }

        ImGuiMCP::EndTable();
    }

    if (removeIndex != static_cast<std::size_t>(-1)) {
        config.folders.erase(config.folders.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        changed = true;
    }

    // At the top, because first match wins and a rule added from here is
    // almost always narrower than the ones already in the list.
    if (pendingAdd) {
        config.folders.insert(config.folders.begin(), *std::move(pendingAdd));
        changed = true;
    }

    ImGuiMCP::Text("%zu of %zu folders. Used fills in as the game loads textures.",
                   visible.size(), g_rows.size());

    if (changed) PublishConfig();

    ImGuiMCP::Spacing();
    RenderSaveReload();
}

namespace {
    void Paragraph(const char* text) { ImGuiMCP::TextWrapped("%s", text); }

    void Example(const char* line) {
        ImGuiMCP::Indent();
        ImGuiMCP::TextUnformatted(line);
        ImGuiMCP::Unindent();
    }
}

void __stdcall UI::Help::Render() {
    ImGuiMCP::SeparatorText("How it works");
    Paragraph("A .dds texture carries smaller copies of itself, each half the size of the one above. The "
              "plugin skips the largest ones and hands the game the first that fits your limit. Nothing is "
              "resampled and no file on disk is changed.");
    Paragraph("A limit applies to the longer side: a 4096x2048 texture capped at 1024 loads at 1024x512.");
    Paragraph("Changes apply to textures loaded from then on. A texture already in memory keeps its size "
              "until the game loads it again.");
    Paragraph("Textures without smaller copies, cubemaps, texture arrays and render targets are never touched.");

    ImGuiMCP::SeparatorText("Texture types");
    Paragraph("The type comes from the end of the file name, see Categories. A texture whose name can't be "
              "read counts as Diffuse.");

    ImGuiMCP::SeparatorText("Folder rules");
    Paragraph("A rule matches anywhere in the path, file name included, and wins over the type limit. Case "
              "and slash direction don't matter. Full size leaves matching textures untouched.");
    Example("\\weapons\\silver\\  2048");
    Paragraph("Close a rule on both sides or it catches more than you meant: sky also matches "
              "actors\\character\\facegendata\\facetint\\skyrim.esm, \\sky\\ does not.");
    Paragraph("Give a rule a type and it only covers that type. Rules are checked from the top and the first "
              "match wins, so the narrow rule goes above the broad one:");
    Example("1.  Normal  \\actors\\character\\  2048");
    Example("2.  Any     \\actors\\character\\  Full size");
    Paragraph("That caps character normal maps at 2048 and leaves the rest of the folder alone. Swap them "
              "and the Normal rule never applies.");

    ImGuiMCP::SeparatorText("Browse folders");
    Paragraph("Lists every folder under Data\\textures, plus the ones the base game keeps in its archives.");
    Paragraph("Loose files: .dds files sitting directly in that folder. A dash means there are none: the "
              "folder only holds subfolders, or only exists in an archive.");
    Paragraph("Used: how many textures the game loaded from that folder while tracking was on. A texture "
              "loaded twice counts twice. Tracking costs a little on every texture load, so it is off by "
              "default.");
    Paragraph("Folders packed in a mod's own archive only appear once tracking has seen one of their "
              "textures load.");

    ImGuiMCP::SeparatorText("Saving");
    Paragraph("Changes made in this menu apply at once. Save writes them to "
              "Data\\SKSE\\Plugins\\TextureDownscaler.ini. Reload drops unsaved changes and reads the file "
              "again.");
    Paragraph("With Log level at Debug, TextureDownscaler.log gets a line for every texture loaded at a "
              "reduced size, with its name and type. The log sits in the SKSE folder under "
              "Documents\\My Games.");
}
