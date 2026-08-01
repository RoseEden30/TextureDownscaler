#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <SimpleIni.h>
#include <spdlog/sinks/basic_file_sink.h>

// Needed by the plugin declaration CMake generates, which uses "..."sv.
using namespace std::literals;
