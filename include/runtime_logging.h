#pragma once

#include <spdlog/logger.h>

#include <filesystem>
#include <memory>
#include <string>

namespace immersive_audio_muxer::logging {

constexpr std::size_t kRetainedLogRuns = 15;

// executable_directory is normally QCoreApplication::applicationDirPath().
// fallback_base_directory is a per-user writable base such as LocalAppData.
bool Initialize(const std::filesystem::path& executable_directory,
                const std::filesystem::path& fallback_base_directory,
                std::string* error = nullptr);
void Shutdown() noexcept;
std::filesystem::path CurrentLogPath();
std::shared_ptr<spdlog::logger> GetLogger();

}  // namespace immersive_audio_muxer::logging

#define IAMF_MUX_LOG_IMPL(level_value, ...)                                      \
    do {                                                                         \
        const auto iamf_mux_logger =                                             \
            ::immersive_audio_muxer::logging::GetLogger();                       \
        if (iamf_mux_logger)                                                     \
            iamf_mux_logger->log(                                                \
                spdlog::source_loc{__FILE__, __LINE__, __func__},                \
                level_value, __VA_ARGS__);                                       \
    } while (false)

#define IAMF_MUX_LOG_TRACE(...) IAMF_MUX_LOG_IMPL(spdlog::level::trace, __VA_ARGS__)
#define IAMF_MUX_LOG_DEBUG(...) IAMF_MUX_LOG_IMPL(spdlog::level::debug, __VA_ARGS__)
#define IAMF_MUX_LOG_INFO(...) IAMF_MUX_LOG_IMPL(spdlog::level::info, __VA_ARGS__)
#define IAMF_MUX_LOG_WARN(...) IAMF_MUX_LOG_IMPL(spdlog::level::warn, __VA_ARGS__)
#define IAMF_MUX_LOG_ERROR(...) IAMF_MUX_LOG_IMPL(spdlog::level::err, __VA_ARGS__)
