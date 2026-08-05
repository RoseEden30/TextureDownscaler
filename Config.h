#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// D3D11 requires the top mip of a block-compressed texture to be a multiple of
// 4, so no limit can usefully go below that.
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

struct FolderRule {
    std::string             folder;   // lower case, backslashes
    std::uint32_t           maxSize;
    std::optional<Category> category; // unset applies to every type

    bool operator==(const FolderRule&) const = default;
};

struct Config {
    bool          enabled          = true;
    bool          trackUsedFolders = false;
    std::uint32_t logLevel         = 2;

    // Zero means that type is left at full size.
    std::array<std::uint32_t, kCategoryCount> maxSize{};

    // Kept in INI order so the first match wins.
    std::vector<FolderRule> folders;

    // The lowest limit any rule could act on. A texture at or below it is out
    // of reach of all of them, so its name never has to be resolved.
    std::uint32_t smallestLimit = 0;

    bool operator==(const Config&) const = default;
};

// The hooks run on the engine's texture loader threads and the menu on the
// render thread. Rather than lock, the menu edits a private copy and publishes
// an immutable snapshot; a reader holds its snapshot for as long as it needs.
using ConfigPtr = std::shared_ptr<const Config>;

// Working copy. Render thread only.
Config& EditableConfig();

// Snapshot for the hooks, valid as long as the caller keeps the pointer.
ConfigPtr ActiveConfig();

// Recomputes smallestLimit and publishes the working copy.
void PublishConfig();

// Fast path gates, kept out of the snapshot so rejecting a texture costs a
// couple of atomic loads.
extern std::atomic<bool>          g_enabled;
extern std::atomic<bool>          g_trackUsedFolders;
extern std::atomic<std::uint32_t> g_smallestLimit;

// Writes the default ini if none exists, reads it, applies the log level and
// publishes. Called at startup, and again by the menu to reload from disk.
void LoadConfig();

// Writes the working copy back to the ini. [General] and [Textures] are set in
// place. [Folders] is rebuilt from scratch only when the keys on disk no longer
// match, since a rule's key is its name and SimpleIni cannot move a key.
void SaveConfig();

// True when the working copy no longer matches what was last read from or
// written to the ini.
bool ConfigDirty();

// Lower case, and both path separators fold together.
constexpr char Fold(char character) {
    if (character >= 'A' && character <= 'Z') return static_cast<char>(character + ('a' - 'A'));
    return character == '/' ? '\\' : character;
}

bool EqualsFolded(std::string_view left, std::string_view right);
bool ContainsFolded(std::string_view haystack, std::string_view needle);

std::optional<Category> CategoryFromName(std::string_view name);

// How a rule reads back, prefix included.
std::string RuleName(const FolderRule& rule);

// A rule can be narrowed to one texture type, written Normal:folder. Strips the
// prefix and returns false if it doesn't name a type.
bool SplitCategory(std::string_view& folder, std::optional<Category>& category);

// True if an earlier rule already matches everything this one would, making it
// unreachable. The earlier rule's index is written to shadowedBy if given.
bool IsRuleShadowed(const Config& config, std::size_t index, std::size_t* shadowedBy = nullptr);

// PublishConfig calls this, so an edit made through it is already covered.
void RecomputeSmallestLimit(Config& config);
