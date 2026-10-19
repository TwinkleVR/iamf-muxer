#include "in_process_iamf_encoder.h"

#include "iamf_tools_encoder_api.h"
#include "path_utils.h"
#include "runtime_logging.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>

namespace immersive_audio_muxer {
namespace {

void SetError(std::string* error, const std::string& value) {
    if (error != nullptr)
        *error = value;
    IAMF_MUX_LOG_ERROR("[IAMFEncoder] {}", value);
}

bool Valid(const InProcessIamfEncoderOptions& options, std::string* error) {
    if (options.user_metadata.empty() || options.input_wav_directory.empty() ||
        options.output_iamf_directory.empty()) {
        SetError(error, "in-process official encoder requires metadata, WAV directory, and output directory");
        return false;
    }
    std::error_code filesystem_error;
    if (!std::filesystem::exists(options.user_metadata, filesystem_error) || filesystem_error) {
        SetError(error, "IAMF user metadata does not exist: " + path_utils::ToUtf8(options.user_metadata));
        return false;
    }
    filesystem_error.clear();
    if (!std::filesystem::is_directory(options.input_wav_directory, filesystem_error) || filesystem_error) {
        SetError(error, "IAMF input WAV directory does not exist: " + path_utils::ToUtf8(options.input_wav_directory));
        return false;
    }
    return true;
}

struct CallbackState {
    const InProcessIamfEncoderOptions* options = nullptr;
    std::string* output = nullptr;
    std::mutex mutex;
};

int CancellationCallback(void* opaque) {
    const auto* state = static_cast<const CallbackState*>(opaque);
    return state != nullptr && state->options != nullptr && state->options->cancellation_requested &&
           state->options->cancellation_requested();
}

void AppendOutput(std::string* output, const char* message, std::size_t limit) {
    if (output == nullptr || message == nullptr || limit == 0)
        return;
    output->append(message);
    if (output->size() <= limit)
        return;
    constexpr char kMarker[] = "[... official encoder output truncated ...]\n";
    if (limit <= sizeof(kMarker) - 1) {
        *output = output->substr(output->size() - limit);
        return;
    }
    const std::size_t tail_size = limit - (sizeof(kMarker) - 1);
    std::string truncated(kMarker);
    truncated.append(*output, output->size() - tail_size, tail_size);
    output->swap(truncated);
}

void LogCallback(int severity, const char* message, void* opaque) {
    try {
        auto* state = static_cast<CallbackState*>(opaque);
        if (message == nullptr)
            return;
        if (state != nullptr && state->options != nullptr) {
            std::lock_guard<std::mutex> lock(state->mutex);
            AppendOutput(state->output, message, state->options->max_captured_output_bytes);
        }
        if (auto logger = logging::GetLogger()) {
            const spdlog::level::level_enum level = severity < 0
                ? spdlog::level::debug
                : (severity == 0 ? spdlog::level::info
                                 : (severity == 1 ? spdlog::level::warn
                                                  : (severity == 2 ? spdlog::level::err
                                                                   : spdlog::level::critical)));
            logger->log(spdlog::source_loc{__FILE__, __LINE__, __func__}, level,
                        "[IAMFEncoder][severity={}] {}", severity, message);
        }
    } catch (...) {
    }
}

}  // namespace

bool InProcessIamfEncoder::Encode(const InProcessIamfEncoderOptions& options,
                                  std::filesystem::path* output_iamf, std::string* error,
                                  std::string* process_output) {
    if (process_output != nullptr)
        process_output->clear();
    if (output_iamf != nullptr)
        output_iamf->clear();
    IAMF_MUX_LOG_INFO(
        "[IAMFEncoder] starting: metadata={} input_wav_directory={} output_directory={} max_captured_output_bytes={}",
        path_utils::ToUtf8(options.user_metadata), path_utils::ToUtf8(options.input_wav_directory),
        path_utils::ToUtf8(options.output_iamf_directory), options.max_captured_output_bytes);
    if (output_iamf == nullptr || !Valid(options, error))
        return false;

#if !defined(IMMERSIVE_AUDIO_MUXER_HAVE_INPROCESS_IAMF_TOOLS)
    SetError(error, "this build does not include the in-process official iamf-tools encoder");
    return false;
#else
    std::error_code filesystem_error;
    std::filesystem::create_directories(options.output_iamf_directory, filesystem_error);
    if (filesystem_error) {
        SetError(error, "cannot create IAMF output directory: " + filesystem_error.message());
        return false;
    }

    const std::string metadata = path_utils::ToUtf8(options.user_metadata);
    const std::string input_directory = path_utils::ToUtf8(options.input_wav_directory);
    const std::string output_directory = path_utils::ToUtf8(options.output_iamf_directory);
    std::array<char, 32768> output_filename{};
    std::array<char, 32768> error_message{};
    std::string captured_output;
    CallbackState callback_state{&options, &captured_output, {}};
    IamfToolsEncoderRunOptions run_options;
    run_options.user_metadata_filename = metadata.c_str();
    run_options.input_wav_directory = input_directory.c_str();
    run_options.output_iamf_directory = output_directory.c_str();
    run_options.cancellation_requested = options.cancellation_requested ? &CancellationCallback : nullptr;
    run_options.log_callback = &LogCallback;
    run_options.opaque = &callback_state;

    const int status = iamf_tools_encoder_run(&run_options, output_filename.data(), output_filename.size(),
                                              error_message.data(), error_message.size());
    if (process_output != nullptr)
        *process_output = captured_output;
    if (status != 0) {
        std::string message = "in-process official iamf-tools encoder failed with status code " +
                              std::to_string(status);
        if (error_message[0] != '\0')
            message += ": " + std::string(error_message.data());
        if (error != nullptr && !captured_output.empty())
            message += "\nOfficial encoder log:\n" + captured_output;
        SetError(error, message);
        return false;
    }
    if (output_filename[0] == '\0') {
        SetError(error, "in-process official encoder completed without an IAMF output path");
        return false;
    }
    *output_iamf = std::filesystem::u8path(output_filename.data());
    filesystem_error.clear();
    if (!std::filesystem::is_regular_file(*output_iamf, filesystem_error) || filesystem_error) {
        SetError(error, "in-process official encoder returned a missing IAMF output path: " +
                           path_utils::ToUtf8(*output_iamf));
        output_iamf->clear();
        return false;
    }
    filesystem_error.clear();
    const uintmax_t output_size = std::filesystem::file_size(*output_iamf, filesystem_error);
    IAMF_MUX_LOG_INFO("[IAMFEncoder] completed: output={} file_size_bytes={}",
                      path_utils::ToUtf8(*output_iamf), filesystem_error ? 0 : output_size);
    return true;
#endif
}

}  // namespace immersive_audio_muxer
