#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace immersive_audio_muxer {

struct InProcessIamfEncoderOptions {
    std::filesystem::path user_metadata;
    std::filesystem::path input_wav_directory;
    std::filesystem::path output_iamf_directory;
    std::function<bool()> cancellation_requested;
    std::size_t max_captured_output_bytes = 1024 * 1024;
};

class InProcessIamfEncoder final {
public:
    static bool Encode(const InProcessIamfEncoderOptions& options, std::filesystem::path* output_iamf,
                       std::string* error, std::string* process_output = nullptr);
};

}  // namespace immersive_audio_muxer
