#pragma once

#include <cstdint>

// Puts the texture loader and D3D device hooks in place. Does nothing when the
// plugin starts disabled. Call once, at kDataLoaded.
void InstallHooks();

enum class HookStatus {
    NotInstalled,
    DisabledAtStartup,
    Installed
};

HookStatus GetHookStatus();

struct ReductionStats {
    std::uint64_t textures   = 0;
    std::uint64_t savedBytes = 0;
};

ReductionStats GetReductionStats();
