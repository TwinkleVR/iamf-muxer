#include "runtime_logging.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace immersive_audio_muxer::logging {
namespace {

constexpr char kLoggerName[] = "immersive_audio_muxer";
constexpr char kLogFilePrefix[] = "iamf_muxer_";
std::mutex g_runtime_mutex;
std::filesystem::path g_current_log_path;

void WriteToDebugOutput(const std::string& message) noexcept {
#if defined(_WIN32)
    const int wide_size = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    if (wide_size <= 0) {
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");
        return;
    }
    std::wstring wide_message(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wide_message.data(), wide_size);
    OutputDebugStringW(wide_message.c_str());
    OutputDebugStringW(L"\n");
#else
    std::fputs(message.c_str(), stderr);
    std::fputc('\n', stderr);
#endif
}

unsigned long CurrentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

spdlog::filename_t NativeFilename(const std::filesystem::path& path) {
#if defined(_WIN32)
    return path.wstring();
#else
    return path.string();
#endif
}

std::filesystem::path MakeRunLogPath(const std::filesystem::path& directory) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    std::ostringstream filename;
    filename << kLogFilePrefix << std::put_time(&local_time, "%Y%m%d_%H%M%S_")
             << CurrentProcessId() << ".log";
    return directory / filename.str();
}

std::shared_ptr<spdlog::sinks::sink> OpenFileSink(
    const std::filesystem::path& log_directory,
    std::filesystem::path* log_path,
    std::string* failure) {
    if (log_directory.empty()) {
        if (failure)
            *failure = "log directory is empty";
        return {};
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(log_directory, filesystem_error);
    if (filesystem_error) {
        if (failure)
            *failure = "cannot create log directory " + log_directory.u8string() + ": " +
                       filesystem_error.message();
        return {};
    }
    if (!std::filesystem::is_directory(log_directory, filesystem_error) || filesystem_error) {
        if (failure)
            *failure = "log path is not a directory: " + log_directory.u8string();
        return {};
    }

    const std::filesystem::path candidate = MakeRunLogPath(log_directory);
    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            NativeFilename(candidate), true);
        if (log_path)
            *log_path = candidate;
        return sink;
    } catch (const std::exception& exception) {
        if (failure)
            *failure = "cannot open log file " + candidate.u8string() + ": " + exception.what();
    } catch (...) {
        if (failure)
            *failure = "cannot open log file " + candidate.u8string() + ": unknown error";
    }
    return {};
}

void PruneOldLogs(const std::filesystem::path& log_directory,
                  const std::filesystem::path& current_log_path,
                  const std::shared_ptr<spdlog::logger>& logger) {
    std::error_code filesystem_error;
    std::vector<std::filesystem::path> log_files;
    for (std::filesystem::directory_iterator iterator(log_directory, filesystem_error), end;
         !filesystem_error && iterator != end; iterator.increment(filesystem_error)) {
        if (!iterator->is_regular_file(filesystem_error) || filesystem_error)
            continue;
        const std::string filename = iterator->path().filename().u8string();
        if (filename.compare(0, sizeof(kLogFilePrefix) - 1, kLogFilePrefix) == 0 &&
            iterator->path().extension() == ".log")
            log_files.push_back(iterator->path());
    }
    if (filesystem_error) {
        logger->warn("[Logging] could not enumerate old logs in {}: {}",
                     log_directory.u8string(), filesystem_error.message());
        return;
    }

    std::sort(log_files.begin(), log_files.end(), [](const auto& left, const auto& right) {
        return left.filename().u8string() > right.filename().u8string();
    });
    std::vector<std::filesystem::path> retained_logs;
    retained_logs.reserve(kRetainedLogRuns);
    retained_logs.push_back(current_log_path);
    for (const auto& path : log_files) {
        if (path == current_log_path || retained_logs.size() >= kRetainedLogRuns)
            continue;
        retained_logs.push_back(path);
    }
    for (const auto& path : log_files) {
        if (std::find(retained_logs.begin(), retained_logs.end(), path) != retained_logs.end())
            continue;
        filesystem_error.clear();
        if (!std::filesystem::remove(path, filesystem_error) || filesystem_error) {
            logger->warn("[Logging] could not remove old log {}: {}",
                         path.u8string(), filesystem_error.message());
        }
    }
}

}  // namespace

bool Initialize(const std::filesystem::path& executable_directory,
                const std::filesystem::path& fallback_base_directory,
                std::string* error) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (error)
        error->clear();
    spdlog::shutdown();
    g_current_log_path.clear();

    const std::filesystem::path primary_log_directory = executable_directory / "logs";
    std::filesystem::path log_path;
    std::string primary_error;
    auto sink = OpenFileSink(primary_log_directory, &log_path, &primary_error);
    std::filesystem::path selected_log_directory = primary_log_directory;
    std::string fallback_error;
    if (!sink && !fallback_base_directory.empty()) {
        selected_log_directory = fallback_base_directory / "logs";
        sink = OpenFileSink(selected_log_directory, &log_path, &fallback_error);
    }
    if (!sink) {
        const std::string message = "unable to initialize file logging; primary: " + primary_error +
                                   (fallback_error.empty() ? std::string{} : "; fallback: " + fallback_error);
        if (error)
            *error = message;
        WriteToDebugOutput(message);
        return false;
    }

    auto logger = std::make_shared<spdlog::logger>(kLoggerName, std::move(sink));
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%H:%M:%S.%e] [thread=%t] [%^%L%$] [%s:%#] %v");
    logger->flush_on(spdlog::level::warn);
    logger->set_error_handler([](const std::string& message) {
        WriteToDebugOutput("spdlog sink error: " + message);
    });
    spdlog::register_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
    g_current_log_path = log_path;

    if (log_path.parent_path() != primary_log_directory) {
        logger->warn("[Logging] executable directory is not writable; using fallback log file {}",
                     log_path.u8string());
        WriteToDebugOutput("[Logging] primary directory unavailable: " + primary_error);
    }
    PruneOldLogs(selected_log_directory, log_path, logger);
    logger->info("[Logging] session started; file={}, retained_runs={}",
                 log_path.u8string(), kRetainedLogRuns);
    return true;
}

void Shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    try {
        if (auto logger = spdlog::get(kLoggerName))
            logger->flush();
        spdlog::shutdown();
        g_current_log_path.clear();
    } catch (const std::exception& exception) {
        WriteToDebugOutput(std::string("[Logging] shutdown failed: ") + exception.what());
    } catch (...) {
        WriteToDebugOutput("[Logging] shutdown failed with an unknown error");
    }
}

std::filesystem::path CurrentLogPath() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_current_log_path;
}

std::shared_ptr<spdlog::logger> GetLogger() {
    return spdlog::get(kLoggerName);
}

}  // namespace immersive_audio_muxer::logging
