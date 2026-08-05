#pragma once

#include <cstdint>

// Opens Data/SKSE/Plugins/<plugin>.log and makes it the default sink.
void InitLogger();

// 0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Fatal, as written in the ini.
// Anything else falls back to info.
void SetLogLevel(std::uint32_t level);
