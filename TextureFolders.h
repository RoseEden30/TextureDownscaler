#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The folder picker's backing list. Every directory under Data\textures, found
// by scanning the disk, merged with the folders the base game ships inside its
// archives. Loose files are what mods overwhelmingly use, so the scan covers
// them; a mod shipping its textures in its own archive stays invisible.
struct TextureFolder {
    std::string   path;  // relative to textures, lower case, backslashes
    std::uint32_t files; // .dds found on disk, zero when only known from the built-in list
};

// Starts the scan on a worker thread, or does nothing if it already ran. Safe
// to call every frame.
void BeginFolderScan();

// False while the scan is running, and the list is empty until it turns true.
bool FolderScanReady();

// Sorted by path. Only meaningful once FolderScanReady returns true.
const std::vector<TextureFolder>& ScannedFolders();

// How a folder reads as a rule, closed on both sides so it can't match a
// longer name that merely starts the same way.
std::string FolderRuleText(std::string_view path);

// Counts one texture against the folder its file sits in. Called from the
// engine's loader threads, and only while g_trackUsedFolders is set, for every
// texture that comes from a file whatever its size, so the tally reflects the
// load order rather than the settings.
void RecordUsedFolder(std::string_view name);

// Throws the tally away. The counts live in memory only.
void ClearUsedFolders();

// Textures seen per folder since the game started, keyed the same way as
// TextureFolder::path. Copied out, so the caller can keep it.
std::unordered_map<std::string, std::uint32_t> UsedFolders();
