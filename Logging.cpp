#include "Logging.h"

void InitLogger() {
    auto dir = SKSE::log::log_directory();
    if (!dir) return;

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    *dir /= std::format("{}.log", plugin->GetName());

    auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(dir->string(), true);
    auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));

    // The debug level logs one line per texture, so flushing every line would
    // be far too expensive.
    logger->flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

    SKSE::log::info("{} v{}", plugin->GetName(), plugin->GetVersion().string("."sv));
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

    // At debug level the log is only useful if it survives a crash, which is
    // worth the cost of flushing every line.
    spdlog::default_logger()->flush_on(level <= 1 ? spdlog::level::debug : spdlog::level::info);
}
