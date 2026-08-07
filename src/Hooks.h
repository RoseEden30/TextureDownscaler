#pragma once

// Puts the texture loader and D3D device hooks in place. Does nothing when the
// plugin starts disabled. Call once, at kDataLoaded.
void InstallHooks();

// True once the device hooks are in place. Since they are skipped entirely on a
// disabled start, the menu has no way to turn the plugin on in that session.
bool HooksInstalled();
