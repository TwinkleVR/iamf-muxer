#pragma once

#include "app/project_types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer::app {

enum class PreparedAudioSampleFormat {
    kPcmS16,
    kPcmS24,
    kPcmS32,
    kFloat32,
};

enum class AudioPreparationPrecisionPolicy {
    kPreserveSource,
    kForcePcmS16,
};

struct AudioPreparationOptions {
    std::filesystem::path input_path;
    int stream_index = -1;
    std::filesystem::path output_wav_path;
    int output_sample_rate = 48000;
    AudioPreparationPrecisionPolicy precision_policy = AudioPreparationPrecisionPolicy::kPreserveSource;
    // When set to 16, 24, or 32, convert decoded audio to that integer PCM width.
    // Zero keeps the selected precision policy behavior.
    int target_pcm_bit_depth = 0;
    bool preserve_unspecified_channel_order = false;
    // For each output channel, the corresponding decoded source channel index.
    // Empty preserves the source order.
    std::vector<int> output_channel_to_input;
    std::function<bool()> cancellation_requested;
};

struct PreparedAudioInfo {
    std::filesystem::path wav_path;
    PreparedAudioSampleFormat sample_format = PreparedAudioSampleFormat::kPcmS16;
    int bit_depth = 16;
};

struct SharedPcmPreparationPlan {
    int target_bit_depth = 16;
    bool reprepare_first = false;
    bool reprepare_second = false;
};

// Chooses a common integer PCM representation for two audio inputs. If either
// input is IEEE float32, both outputs use signed 32-bit integer PCM.
SharedPcmPreparationPlan PlanSharedPcmPreparation(const PreparedAudioInfo& first,
                                                 const PreparedAudioInfo& second);

class AudioPreparationService final {
public:
    static bool PrepareToWav(const AudioPreparationOptions& options,
                             PreparedAudioInfo* info, std::string* error);
};

}  // namespace immersive_audio_muxer::app
