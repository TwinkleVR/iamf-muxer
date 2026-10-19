#pragma once

#include "app/project_types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer::app {

struct AudioPreparationOptions {
    std::filesystem::path input_path;
    int stream_index = -1;
    std::filesystem::path output_wav_path;
    int output_sample_rate = 48000;
    bool preserve_unspecified_channel_order = false;
    // For each output channel, the corresponding decoded source channel index.
    // Empty preserves the source order.
    std::vector<int> output_channel_to_input;
    std::function<bool()> cancellation_requested;
};

struct PreparedAudioInfo {
    std::filesystem::path wav_path;
};

class AudioPreparationService final {
public:
    static bool PrepareToWav(const AudioPreparationOptions& options,
                             PreparedAudioInfo* info, std::string* error);
};

}  // namespace immersive_audio_muxer::app
