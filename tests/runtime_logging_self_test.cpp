#include "avformat_input.h"
#include "runtime_logging.h"

extern "C" {
#include <libavutil/log.h>
}

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool Expect(bool condition, const std::string& message) {
    if (!condition)
        std::cerr << "runtime logging self-test failed: " << message << '\n';
    return condition;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const auto unique_id = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path() /
        std::filesystem::u8path("iamf_muxer_runtime_logging_caf\xc3\xa9_") /
        std::to_string(unique_id);
    TemporaryDirectory cleanup(temporary_root);
    const std::filesystem::path executable_directory = temporary_root / "app";
    const std::filesystem::path fallback_directory = temporary_root / "fallback";
    std::error_code filesystem_error;
    std::filesystem::create_directories(executable_directory, filesystem_error);
    if (!Expect(!filesystem_error, "cannot create temporary application directory"))
        return 1;
    const std::filesystem::path log_directory = executable_directory / "logs";
    std::filesystem::create_directories(log_directory, filesystem_error);
    if (!Expect(!filesystem_error, "cannot create temporary log directory"))
        return 1;
    const std::filesystem::path unrelated_file = log_directory / "keep-this-file.txt";
    {
        std::ofstream file(unrelated_file, std::ios::binary);
        file << "must not be removed by log retention";
    }

    std::filesystem::path first_log_path;
    std::filesystem::path latest_log_path;
    for (int run = 0; run < 16; ++run) {
        std::string initialize_error;
        if (!Expect(immersive_audio_muxer::logging::Initialize(
                        executable_directory, fallback_directory, &initialize_error),
                    "logger initialization failed: " + initialize_error))
            return 1;
        const std::filesystem::path current_log_path =
            immersive_audio_muxer::logging::CurrentLogPath();
        latest_log_path = current_log_path;
        if (!Expect(current_log_path.parent_path() == executable_directory / "logs",
                    "primary log directory is not beside the executable")) {
            immersive_audio_muxer::logging::Shutdown();
            return 1;
        }
        if (run == 0) {
            first_log_path = current_log_path;
            IAMF_MUX_LOG_DEBUG("[SelfTest] debug severity marker");
            IAMF_MUX_LOG_TRACE("[SelfTest] application trace diagnostic marker");
            IAMF_MUX_LOG_INFO("[SelfTest] UTF-8 caf\xc3\xa9 marker");
            IAMF_MUX_LOG_WARN("[SelfTest] warning severity marker");
            IAMF_MUX_LOG_ERROR("[SelfTest] error severity marker");
            immersive_audio_muxer::avformat_input::InitializeFfmpegLogging();
            av_log(nullptr, AV_LOG_INFO, "[SelfTest] FFmpeg callback message %d\n", 17);
            av_log_set_level(AV_LOG_TRACE);
            av_log(nullptr, AV_LOG_DEBUG, "[SelfTest] FFmpeg DEBUG marker\n");
            av_log(nullptr, AV_LOG_TRACE, "[SelfTest] FFmpeg TRACE marker\n");

            std::vector<std::thread> writers;
            for (int thread_index = 0; thread_index < 4; ++thread_index) {
                writers.emplace_back([thread_index]() {
                    for (int entry = 0; entry < 8; ++entry)
                        IAMF_MUX_LOG_INFO("[SelfTest] writer={} entry={}", thread_index, entry);
                });
            }
            for (std::thread& writer : writers)
                writer.join();
        } else {
            IAMF_MUX_LOG_INFO("[SelfTest] simulated application run={}", run + 1);
        }
        immersive_audio_muxer::logging::Shutdown();
        if (run == 0) {
            const std::string log_contents = ReadFile(first_log_path);
            if (!Expect(log_contents.find("UTF-8 caf\xc3\xa9 marker") !=
                            std::string::npos,
                        "UTF-8 message was not written"))
                return 1;
            if (!Expect(log_contents.find("FFmpeg callback message 17") != std::string::npos,
                        "FFmpeg callback message was not written"))
                return 1;
            if (!Expect(log_contents.find("FFmpeg DEBUG marker") != std::string::npos,
                        "FFmpeg DEBUG diagnostic was not written"))
                return 1;
            if (!Expect(log_contents.find("application trace diagnostic marker") == std::string::npos &&
                            log_contents.find("FFmpeg TRACE marker") == std::string::npos &&
                            log_contents.find("[T]") == std::string::npos,
                        "TRACE-level messages were written to the log"))
                return 1;
            if (!Expect(log_contents.find("[thread=") != std::string::npos,
                        "thread identifier is missing from the log pattern"))
                return 1;
            const std::regex expected_log_prefix(
                R"(\[\d{2}:\d{2}:\d{2}\.\d{3}\] \[thread=\d+\] \[I\])");
            if (!Expect(std::regex_search(log_contents, expected_log_prefix),
                        "timestamp or one-letter severity format is invalid"))
                return 1;
            if (!Expect(log_contents.find("[D]") != std::string::npos &&
                            log_contents.find("[W]") != std::string::npos &&
                            log_contents.find("[E]") != std::string::npos,
                        "uppercase single-letter severity codes are missing"))
                return 1;
            for (int thread_index = 0; thread_index < 4; ++thread_index) {
                for (int entry = 0; entry < 8; ++entry) {
                    const std::string expected = "writer=" + std::to_string(thread_index) +
                                                 " entry=" + std::to_string(entry);
                    if (!Expect(log_contents.find(expected) != std::string::npos,
                                "a concurrent writer entry is missing: " + expected))
                        return 1;
                }
            }
        }
    }

    std::size_t retained_logs = 0;
    for (std::filesystem::directory_iterator iterator(executable_directory / "logs", filesystem_error), end;
         !filesystem_error && iterator != end; iterator.increment(filesystem_error)) {
        if (iterator->is_regular_file(filesystem_error) &&
            iterator->path().filename().u8string().find("iamf_muxer_") == 0 &&
            iterator->path().extension() == ".log")
            ++retained_logs;
    }
    if (!Expect(!filesystem_error, "cannot enumerate retained log files") ||
        !Expect(retained_logs == immersive_audio_muxer::logging::kRetainedLogRuns,
                "16 simulated starts did not leave exactly 15 run logs"))
        return 1;
    if (!Expect(std::filesystem::exists(latest_log_path),
                "the newest run log was pruned") ||
        !Expect(!std::filesystem::exists(first_log_path),
                "the oldest of 16 run logs was not pruned"))
        return 1;
    if (!Expect(std::filesystem::exists(unrelated_file),
                "log retention removed an unrelated file"))
        return 1;

    const std::filesystem::path invalid_executable_directory = temporary_root / "not_a_directory";
    {
        std::ofstream file(invalid_executable_directory, std::ios::binary);
        file << "occupy the primary path";
    }
    std::string fallback_error;
    if (!Expect(immersive_audio_muxer::logging::Initialize(
                    invalid_executable_directory, fallback_directory, &fallback_error),
                "fallback logger initialization failed: " + fallback_error))
        return 1;
    if (!Expect(immersive_audio_muxer::logging::CurrentLogPath().parent_path() ==
                    fallback_directory / "logs",
                "logger did not fall back when the executable log path was unavailable")) {
        immersive_audio_muxer::logging::Shutdown();
        return 1;
    }
    IAMF_MUX_LOG_INFO("[SelfTest] fallback path active");
    immersive_audio_muxer::logging::Shutdown();
    return 0;
}
